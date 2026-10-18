#include "bench/latency.hpp"

#include <hdr/hdr_histogram.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>

namespace lob::bench {
namespace {

/// 10 ms in nanoseconds: the ceiling of the HDR range (1 ns .. 10 ms).
constexpr std::int64_t kHighestTrackableNs = 10'000'000;

/// The process-wide request set from `--latency-mode` (or by tests).
LatencyModeRequest g_request;

/// The auto-mode decision, made once per process so that every benchmark
/// repetition and every JSON context entry reports the same mode and batch.
std::optional<LatencyMode> g_auto_mode;

/// State of the dependent multiply-add chain used to estimate a representative
/// operation cost at startup.
std::uint64_t g_representative_state = 1;

/// A ~25 ns dependent chain: the shape (and rough cost) of a book operation,
/// with a data dependency so the compiler cannot fold it away. Sized to land
/// well inside the 16..32 ns band that selects the same batch size, so run-to-
/// run calibration noise cannot flip the power-of-two batch.
void representative_op() {
  std::uint64_t state = g_representative_state;
  for (int i = 0; i < 24; ++i) {
    state = state * 6364136223846793005ULL + 1442695040888963407ULL;
    __asm__ __volatile__("" : "+r"(state));
  }
  g_representative_state = state;
}

LatencyMode resolve_auto_mode(double typical_op_ns) {
  if (!g_auto_mode.has_value()) {
    g_auto_mode =
        select_latency_mode(LatencyModeRequest{}, timer_granularity().step_ns, typical_op_ns);
  }
  return *g_auto_mode;
}

LatencyMode resolved_mode(double typical_op_ns) {
  const LatencyModeRequest request = g_request;
  if (request.kind == LatencyModeRequestKind::Auto) {
    return resolve_auto_mode(typical_op_ns);
  }
  return select_latency_mode(request, timer_granularity().step_ns, typical_op_ns);
}

/// Records one whole-nanosecond sample, clamped at zero.
void record_ns(hdr_histogram* histogram, double ns) {
  std::int64_t value = static_cast<std::int64_t>(std::llround(ns));
  if (value < 0) {
    value = 0;
  }
  hdr_record_value(histogram, value);
}

std::string batch_label(int batch_size) { return "batch" + std::to_string(batch_size); }

}  // namespace

std::string LatencyMode::label() const { return is_per_op() ? "per_op" : batch_label(batch_size); }

std::string LatencyModeRequest::label() const {
  switch (kind) {
    case LatencyModeRequestKind::PerOp:
      return "per_op";
    case LatencyModeRequestKind::Batch:
      return batch_label(batch_size);
    case LatencyModeRequestKind::Auto:
      break;
  }
  return "auto";
}

void set_latency_mode_request(LatencyModeRequest request) { g_request = request; }

LatencyModeRequest latency_mode_request() { return g_request; }

LatencyMode process_latency_mode() { return resolved_mode(calibrate_op_ns(representative_op)); }

void reset_latency_mode_decision() { g_auto_mode.reset(); }

bool parse_latency_mode_request(std::string_view text, LatencyModeRequest& out) {
  if (text == "auto") {
    out = LatencyModeRequest{};
    return true;
  }
  if (text == "per_op") {
    out = LatencyModeRequest{LatencyModeRequestKind::PerOp, 1};
    return true;
  }
  constexpr std::string_view kBatchPrefix = "batch";
  if (text.size() <= kBatchPrefix.size() || !text.starts_with(kBatchPrefix)) {
    return false;
  }
  int batch_size = 0;
  for (const char digit : text.substr(kBatchPrefix.size())) {
    if (digit < '0' || digit > '9') {
      return false;
    }
    batch_size = batch_size * 10 + (digit - '0');
    if (batch_size > kMaxBatchSize) {
      return false;
    }
  }
  if (batch_size < 1) {
    return false;
  }
  out = LatencyModeRequest{LatencyModeRequestKind::Batch, batch_size};
  return true;
}

LatencyMode select_latency_mode(LatencyModeRequest request, double granularity_ns,
                                double typical_op_ns) {
  switch (request.kind) {
    case LatencyModeRequestKind::PerOp:
      return LatencyMode::per_op();
    case LatencyModeRequestKind::Batch:
      return LatencyMode::batch(request.batch_size);
    case LatencyModeRequestKind::Auto:
      break;
  }
  if (granularity_ns <= kPerOpMaxGranularityNs) {
    return LatencyMode::per_op();
  }
  const double estimate = typical_op_ns > 0.0 ? typical_op_ns : kFallbackOpEstimateNs;
  const double span_target_ns = static_cast<double>(kBatchGranularityTarget) * granularity_ns;
  int batch_size = kMinBatchSize;
  while (static_cast<double>(batch_size) * estimate < span_target_ns &&
         batch_size < kMaxBatchSize) {
    batch_size *= 2;
  }
  return LatencyMode::batch(batch_size);
}

LatencyRecorder::LatencyRecorder() : LatencyRecorder(kFallbackOpEstimateNs) {}

LatencyRecorder::LatencyRecorder(double typical_op_ns)
    : mode_(resolved_mode(typical_op_ns)), overhead_(calibrate_overhead()) {
  const int status = hdr_init(1, kHighestTrackableNs, 3, &histogram_);
  if (status != 0) {
    std::fprintf(stderr, "latency: hdr_init failed (%d)\n", status);
    std::exit(1);
  }
}

LatencyRecorder::~LatencyRecorder() { hdr_close(histogram_); }

void LatencyRecorder::record_ticks(Ticks elapsed, int ops) {
  const double without_overhead =
      std::max(0.0, static_cast<double>(elapsed) - static_cast<double>(overhead_.ticks));
  const double ns = ticks_to_ns(without_overhead) / static_cast<double>(ops);
  if (ns > static_cast<double>(kHighestTrackableNs)) {
    ++overflow_count_;
    record_ns(histogram_, static_cast<double>(kHighestTrackableNs));
    return;
  }
  record_ns(histogram_, ns);
}

Percentiles LatencyRecorder::percentiles() const {
  Percentiles result;
  result.p50_ns = static_cast<std::uint64_t>(hdr_value_at_percentile(histogram_, 50.0));
  result.p90_ns = static_cast<std::uint64_t>(hdr_value_at_percentile(histogram_, 90.0));
  result.p99_ns = static_cast<std::uint64_t>(hdr_value_at_percentile(histogram_, 99.0));
  result.p999_ns = static_cast<std::uint64_t>(hdr_value_at_percentile(histogram_, 99.9));
  result.max_ns = static_cast<std::uint64_t>(hdr_max(histogram_));
  return result;
}

std::uint64_t LatencyRecorder::sample_count() const {
  return static_cast<std::uint64_t>(histogram_->total_count);
}

double LatencyRecorder::mean_ns() const { return sample_count() == 0 ? 0.0 : hdr_mean(histogram_); }

}  // namespace lob::bench
