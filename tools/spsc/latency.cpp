#include "bench/latency.hpp"

#include <hdr/hdr_histogram.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <type_traits>

#include "bench/timer.hpp"
#include "lob/order_book.hpp"
#include "lob/spsc_ring.hpp"
#include "tools/flowfile/replay_config.hpp"
#include "tools/opgen/opgen.hpp"

namespace {

using lob::bench::Ticks;
using lob::opgen::Op;

/// Ring capacity of the handoff. Small enough to keep the handoff latency
/// visible (a huge ring would let the producer run far ahead and measure queue
/// depth instead of plumbing), large enough that neither thread waits on the
/// other for most messages.
constexpr std::size_t kCapacity = 256;

/// The HDR range used by lob_bench (1 ns .. 10 ms, 3 significant figures).
constexpr std::int64_t kHighestTrackableNs = 10'000'000;

/// One order event with the counter value read just before it was enqueued.
struct Event {
  Ticks enqueued;
  Op op;
};

static_assert(std::is_trivially_copyable_v<Event>);

struct ModeRequest {
  enum class Kind : std::uint8_t { Auto, PerOp, Batch };
  Kind kind = Kind::Auto;
  int batch_size = lob::bench::kMinBatchSize;
};

/// Resolved sampling mode. Mirrors lob::bench::select_latency_mode (defined in
/// bench/latency.cpp, which this tool does not link); the thresholds and batch
/// bounds come from bench/latency.hpp so the two harnesses cannot drift.
struct Mode {
  bool per_op = true;
  int batch_size = 1;

  std::string label() const { return per_op ? "per_op" : "batch" + std::to_string(batch_size); }
};

Mode select_mode(ModeRequest request, double granularity_ns, double typical_op_ns) {
  if (request.kind == ModeRequest::Kind::PerOp) {
    return Mode{true, 1};
  }
  if (request.kind == ModeRequest::Kind::Batch) {
    return Mode{false, request.batch_size};
  }
  if (granularity_ns <= lob::bench::kPerOpMaxGranularityNs) {
    return Mode{true, 1};
  }
  const double estimate = typical_op_ns > 0.0 ? typical_op_ns : lob::bench::kFallbackOpEstimateNs;
  const double span_target_ns =
      static_cast<double>(lob::bench::kBatchGranularityTarget) * granularity_ns;
  int batch_size = lob::bench::kMinBatchSize;
  while (static_cast<double>(batch_size) * estimate < span_target_ns &&
         batch_size < lob::bench::kMaxBatchSize) {
    batch_size *= 2;
  }
  return Mode{false, batch_size};
}

/// Enqueue-to-on_accept latency samples, grouped the way lob_bench groups its
/// per-operation samples: one sample per message in per-op mode, or the mean of
/// up to `batch_size` messages per sample in batch mode (the timer overhead is
/// subtracted once per sample either way).
class LatencySink {
 public:
  LatencySink(Mode mode, hdr_histogram* histogram, lob::bench::TimerOverhead overhead)
      : mode_(mode), histogram_(histogram), overhead_(overhead) {}

  void record_delta(Ticks ticks) {
    if (mode_.per_op) {
      record_ns(static_cast<double>(ticks) - static_cast<double>(overhead_.ticks));
      return;
    }
    batch_sum_ += ticks;
    ++batch_count_;
    if (batch_count_ == mode_.batch_size) {
      flush();
    }
  }

  /// Records the partial final batch, if any.
  void flush() {
    if (batch_count_ == 0) {
      return;
    }
    const double elapsed = static_cast<double>(batch_sum_) - static_cast<double>(overhead_.ticks);
    record_ns(std::max(0.0, elapsed) / static_cast<double>(batch_count_));
    batch_sum_ = 0;
    batch_count_ = 0;
  }

  std::uint64_t accepted() const { return accepted_; }
  std::uint64_t samples() const { return static_cast<std::uint64_t>(histogram_->total_count); }
  std::uint64_t overflow() const { return overflow_; }

  /// Called by the listener for every accepted order.
  void note_accept(Ticks enqueued, Ticks now) {
    ++accepted_;
    record_delta(now - enqueued);
  }

 private:
  void record_ns(double ns) {
    if (ns > static_cast<double>(kHighestTrackableNs)) {
      ++overflow_;
      ns = static_cast<double>(kHighestTrackableNs);
    }
    hdr_record_value(histogram_, static_cast<std::int64_t>(ns < 0.0 ? 0.0 : ns + 0.5));
  }

  Mode mode_;
  hdr_histogram* histogram_;
  lob::bench::TimerOverhead overhead_;
  Ticks batch_sum_ = 0;
  int batch_count_ = 0;
  std::uint64_t accepted_ = 0;
  std::uint64_t overflow_ = 0;
};

/// Timestamps on_accept. The engine thread sets the enqueue tick immediately
/// before applying the popped event; every callback happens synchronously
/// inside that call, so the tick belongs to the event being accepted.
class AcceptLatencyListener final : public lob::Listener {
 public:
  explicit AcceptLatencyListener(LatencySink& sink) : sink_(sink) {}

  void set_enqueued(Ticks ticks) { enqueued_ = ticks; }

  void on_accept(lob::OrderId) override { sink_.note_accept(enqueued_, lob::bench::now_ticks()); }
  void on_reject(lob::OrderId, lob::RejectReason) override {}
  void on_trade(lob::OrderId, lob::OrderId, lob::Price, lob::Qty) override {}
  void on_cancel(lob::OrderId, lob::Qty) override {}
  void on_modify(lob::OrderId, lob::Price, lob::Qty) override {}
  void on_book_update(lob::Side, lob::Price, lob::Qty) override {}

 private:
  LatencySink& sink_;
  Ticks enqueued_ = 0;
};

void usage(const char* program) {
  std::fprintf(stderr,
               "usage: %s [--messages N] [--seed S] [--latency-mode auto|per_op|batchN] [--help]\n"
               "  --messages N      order events to feed (default 1000000)\n"
               "  --seed S          generator seed (default 1)\n"
               "  --latency-mode M  sampling mode (default auto: the same rule as lob_bench)\n",
               program);
}

bool parse_u64(const char* text, std::uint64_t& out) {
  if (text == nullptr || *text == '\0') {
    return false;
  }
  std::uint64_t value = 0;
  for (const char* digit = text; *digit != '\0'; ++digit) {
    if (*digit < '0' || *digit > '9') {
      return false;
    }
    const std::uint64_t next = value * 10 + static_cast<std::uint64_t>(*digit - '0');
    if (next < value) {
      return false;
    }
    value = next;
  }
  out = value;
  return true;
}

bool parse_mode(const char* text, ModeRequest& out) {
  if (std::strcmp(text, "auto") == 0) {
    out = ModeRequest{};
    return true;
  }
  if (std::strcmp(text, "per_op") == 0) {
    out = ModeRequest{ModeRequest::Kind::PerOp, 1};
    return true;
  }
  constexpr const char* kBatchPrefix = "batch";
  const std::size_t prefix_length = std::strlen(kBatchPrefix);
  if (std::strncmp(text, kBatchPrefix, prefix_length) != 0 || text[prefix_length] == '\0') {
    return false;
  }
  int batch_size = 0;
  for (const char* digit = text + prefix_length; *digit != '\0'; ++digit) {
    if (*digit < '0' || *digit > '9') {
      return false;
    }
    batch_size = batch_size * 10 + (*digit - '0');
    if (batch_size > lob::bench::kMaxBatchSize) {
      return false;
    }
  }
  if (batch_size < 1) {
    return false;
  }
  out = ModeRequest{ModeRequest::Kind::Batch, batch_size};
  return true;
}

/// One pause instruction while the ring is full/empty.
inline void cpu_relax() {
#if defined(__aarch64__)
  __asm__ __volatile__("yield");
#else
  __builtin_ia32_pause();
#endif
}

/// Mean cost of one engine event (a submit that rests followed by its cancel)
/// on a scratch book, used for auto mode's batch selection. Calibrating on the
/// real submit/listener path, rather than on the ring plumbing, keeps the
/// chosen batch close to lob_bench's choice for a comparable operation.
double calibrate_event_ns() {
  lob::NullListener quiet;
  lob::OrderBook scratch(lob::Config{1, 1000, 1024, 256}, quiet);
  lob::OrderId id = 1;
  const double pair_ns = lob::bench::calibrate_op_ns([&] {
    scratch.submit(id, lob::Side::Buy, lob::OrderType::Limit, 500, 1);
    scratch.cancel(id);
    ++id;
  });
  return pair_ns / 2.0;  // submit + cancel is two events
}

std::uint64_t percentile(hdr_histogram* histogram, double value) {
  return static_cast<std::uint64_t>(hdr_value_at_percentile(histogram, value));
}

}  // namespace

int main(int argc, char** argv) {
  std::uint64_t messages = 1'000'000;
  std::uint64_t seed = 1;
  ModeRequest mode_request;

  for (int arg = 1; arg < argc; ++arg) {
    if (std::strcmp(argv[arg], "--messages") == 0) {
      if (arg + 1 >= argc || !parse_u64(argv[++arg], messages) || messages == 0) {
        std::fprintf(stderr, "spsc_latency: --messages expects a positive decimal count\n");
        return 2;
      }
    } else if (std::strcmp(argv[arg], "--seed") == 0) {
      if (arg + 1 >= argc || !parse_u64(argv[++arg], seed)) {
        std::fprintf(stderr, "spsc_latency: --seed expects a decimal number\n");
        return 2;
      }
    } else if (std::strcmp(argv[arg], "--latency-mode") == 0) {
      if (arg + 1 >= argc || !parse_mode(argv[++arg], mode_request)) {
        std::fprintf(stderr, "spsc_latency: --latency-mode expects auto, per_op or batchN\n");
        return 2;
      }
    } else if (std::strcmp(argv[arg], "--help") == 0 || std::strcmp(argv[arg], "-h") == 0) {
      usage(argv[0]);
      return 0;
    } else {
      std::fprintf(stderr, "spsc_latency: unknown argument '%s'\n", argv[arg]);
      usage(argv[0]);
      return 2;
    }
  }

  hdr_histogram* histogram = nullptr;
  if (hdr_init(1, kHighestTrackableNs, 3, &histogram) != 0) {
    std::fprintf(stderr, "spsc_latency: hdr_init failed\n");
    return 1;
  }

  const lob::bench::GranularityMeasurement& granularity = lob::bench::timer_granularity();
  const double event_ns = calibrate_event_ns();
  const Mode mode = select_mode(mode_request, granularity.step_ns, event_ns);
  const lob::bench::TimerOverhead overhead = lob::bench::calibrate_overhead();
  LatencySink sink(mode, histogram, overhead);
  AcceptLatencyListener listener(sink);

  const lob::Config config = lob::flowfile::default_replay_config();
  lob::OrderBook book(config, listener);
  lob::SpscRing<Event, kCapacity> ring;
  lob::opgen::OpGenerator generator(seed, config);

  const auto start = std::chrono::steady_clock::now();
  std::thread feed([&] {
    Event event{};
    for (std::uint64_t i = 0; i < messages; ++i) {
      event.op = generator.next();
      for (;;) {
        // Stamp just before the attempt that succeeds: queue-full waiting is
        // producer backpressure, not handoff latency.
        event.enqueued = lob::bench::now_ticks();
        if (ring.try_push(event)) {
          break;
        }
        cpu_relax();
      }
    }
  });

  for (std::uint64_t i = 0; i < messages; ++i) {
    Event event{};
    while (!ring.try_pop(event)) {
      cpu_relax();
    }
    listener.set_enqueued(event.enqueued);
    switch (event.op.kind) {
      case lob::opgen::OpKind::Submit:
        book.submit(event.op.id, event.op.side, event.op.type, event.op.price, event.op.qty);
        break;
      case lob::opgen::OpKind::Cancel:
        book.cancel(event.op.id);
        break;
      case lob::opgen::OpKind::Modify:
        book.modify(event.op.id, event.op.price, event.op.qty);
        break;
    }
  }
  feed.join();
  sink.flush();
  const auto end = std::chrono::steady_clock::now();

  const double seconds = std::chrono::duration<double>(end - start).count();
  const double per_second = seconds > 0.0 ? static_cast<double>(messages) / seconds : 0.0;

  std::printf("spsc_latency: enqueue to on_accept handoff latency\n");
  std::printf("source: seeded operation generator, seed %llu, default mix\n",
              static_cast<unsigned long long>(seed));
  std::printf("messages: %llu, accepted: %llu, events/s: %.0f\n",
              static_cast<unsigned long long>(messages),
              static_cast<unsigned long long>(sink.accepted()), per_second);
  std::printf("ring capacity: %zu\n", kCapacity);
  std::printf("timer: granularity %.1f ns (step), calibrated engine event %.1f ns\n",
              granularity.step_ns, event_ns);
  std::printf("mode: %s\n", mode.label().c_str());
  std::printf("samples: %llu", static_cast<unsigned long long>(sink.samples()));
  if (sink.overflow() != 0) {
    std::printf(" (%llu at the 10 ms ceiling)", static_cast<unsigned long long>(sink.overflow()));
  }
  std::printf("\n");
  std::printf("latency ns: p50 %llu, p90 %llu, p99 %llu, p99.9 %llu, max %llu, mean %.1f\n",
              static_cast<unsigned long long>(percentile(histogram, 50.0)),
              static_cast<unsigned long long>(percentile(histogram, 90.0)),
              static_cast<unsigned long long>(percentile(histogram, 99.0)),
              static_cast<unsigned long long>(percentile(histogram, 99.9)),
              static_cast<unsigned long long>(hdr_max(histogram)), hdr_mean(histogram));
  std::printf(
      "note: noisy run on a shared machine; an exploratory measurement, not a publishable "
      "number\n");

  hdr_close(histogram);
  return 0;
}
