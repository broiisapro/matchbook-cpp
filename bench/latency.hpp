#pragma once

#include <cstdint>
#include <string_view>

#include "bench/timer.hpp"

struct hdr_histogram;

namespace lob::bench {

/// How per-operation latency is sampled. The counter frequency decides:
/// per-op timestamps are only meaningful when the clock is fine-grained
/// relative to an operation, so a counter below 1 GHz is used to time batches
/// of kBatchSize operations and the per-op cost is the batch divided by its
/// size (decision C11). The mode is labelled with every report.
enum class LatencyMode : std::uint8_t { PerOp = 0, Batch16 = 1 };

inline constexpr int kBatchSize = 16;

/// Counters slower than this produce batch-timed samples.
inline constexpr double kPerOpMinHertz = 1e9;

inline LatencyMode latency_mode_for(double ticks_per_second_value) {
  return ticks_per_second_value >= kPerOpMinHertz ? LatencyMode::PerOp : LatencyMode::Batch16;
}

constexpr std::string_view to_string(LatencyMode mode) {
  return mode == LatencyMode::PerOp ? "per_op" : "batch16";
}

struct Percentiles {
  std::uint64_t p50_ns = 0;
  std::uint64_t p90_ns = 0;
  std::uint64_t p99_ns = 0;
  std::uint64_t p999_ns = 0;
  std::uint64_t max_ns = 0;
};

/// Records per-operation latency samples into an HDR histogram covering
/// 1 ns .. 10 ms at 3 significant figures.
///
/// The sample resolution follows LatencyMode: in per-op mode each timed
/// operation is one sample of `elapsed - overhead` (clamped at 0); in batch16
/// mode kBatchSize consecutive operations are timed together and one sample of
/// `(elapsed - overhead) / kBatchSize` is recorded. Samples above the 10 ms
/// ceiling are recorded at the ceiling and counted separately.
class LatencyRecorder {
 public:
  LatencyRecorder();
  ~LatencyRecorder();

  LatencyRecorder(const LatencyRecorder&) = delete;
  LatencyRecorder& operator=(const LatencyRecorder&) = delete;
  LatencyRecorder(LatencyRecorder&&) = delete;
  LatencyRecorder& operator=(LatencyRecorder&&) = delete;

  /// Times one operation: `op` is invoked once (per-op mode) or kBatchSize
  /// times (batch16 mode). Returns the number of operations performed, so
  /// callers can account for the mode uniformly.
  template <class F>
  int record_op(F&& op) {
    if (mode_ == LatencyMode::PerOp) {
      const Ticks begin = now_ticks();
      op();
      const Ticks end = now_ticks();
      record_ticks(end - begin, 1);
      return 1;
    }
    const Ticks begin = now_ticks();
    for (int i = 0; i < kBatchSize; ++i) {
      op();
    }
    const Ticks end = now_ticks();
    record_ticks(end - begin, kBatchSize);
    return kBatchSize;
  }

  /// Times one invocation of `batch`, which must perform exactly kBatchSize
  /// operations, and records one sample of `(elapsed - overhead) / kBatchSize`.
  /// For the common single-operation callable use record_op.
  template <class F>
  void record_batch(F&& batch) {
    const Ticks begin = now_ticks();
    batch();
    const Ticks end = now_ticks();
    record_ticks(end - begin, kBatchSize);
  }

  Percentiles percentiles() const;

  /// Mean of the recorded samples in nanoseconds (0 when empty).
  double mean_ns() const;

  LatencyMode mode() const { return mode_; }
  std::string_view mode_name() const { return to_string(mode_); }

  /// Operations per recorded sample: 1 in per-op mode, kBatchSize otherwise.
  int ops_per_sample() const { return mode_ == LatencyMode::PerOp ? 1 : kBatchSize; }

  double overhead_ns() const { return overhead_.ns; }
  Ticks overhead_ticks() const { return overhead_.ticks; }

  /// Samples recorded (not operations; multiply by ops_per_sample()).
  std::uint64_t sample_count() const;

  /// Samples that hit the 10 ms ceiling.
  std::uint64_t overflow_count() const { return overflow_count_; }

 private:
  void record_ticks(Ticks elapsed, int ops);

  hdr_histogram* histogram_ = nullptr;
  LatencyMode mode_ = LatencyMode::PerOp;
  TimerOverhead overhead_{0, 0.0};
  std::uint64_t overflow_count_ = 0;
};

}  // namespace lob::bench
