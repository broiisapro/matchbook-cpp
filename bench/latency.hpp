#pragma once

#include <concepts>
#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>

#include "bench/timer.hpp"

struct hdr_histogram;

namespace lob::bench {

/// Effective timer step above which per-operation timestamps are quantized too
/// coarsely to report: with a step of, say, 41.7 ns every sub-step operation
/// rounds to the same handful of values. Per-op mode is only allowed when the
/// measured granularity is at most this (decision C32).
inline constexpr double kPerOpMaxGranularityNs = 2.0;

/// A batch sample should span at least this many timer steps, so quantization
/// contributes at most a couple of percent to the per-op average.
inline constexpr int kBatchGranularityTarget = 50;

/// Auto mode never chooses a batch smaller than this many operations.
inline constexpr int kMinBatchSize = 16;

/// Upper bound on the batch the harness will choose (and the largest number of
/// operations one `record_op` call can perform; fixtures size their headroom
/// against it).
inline constexpr int kMaxBatchSize = 1024;

/// Operation-cost estimate used when no calibration of the real operation is
/// available. Callers with a real workload should calibrate with
/// `calibrate_op_ns` instead.
inline constexpr double kFallbackOpEstimateNs = 25.0;

enum class LatencyModeKind : std::uint8_t { PerOp = 0, Batch = 1 };

/// How per-operation latency is sampled.
struct LatencyMode {
  LatencyModeKind kind = LatencyModeKind::PerOp;
  int batch_size = 1;  ///< 1 in per-op mode

  static LatencyMode per_op() { return LatencyMode{LatencyModeKind::PerOp, 1}; }
  static LatencyMode batch(int size) { return LatencyMode{LatencyModeKind::Batch, size}; }

  bool is_per_op() const { return kind == LatencyModeKind::PerOp; }

  /// Operations behind one recorded sample: 1 in per-op mode, else the batch
  /// size. Also the number of operations one `record_op` call performs.
  int ops_per_sample() const { return is_per_op() ? 1 : batch_size; }

  /// "per_op" or "batchN" (e.g. "batch64"); used in console output, the JSON
  /// context and the report.
  std::string label() const;

  friend bool operator==(const LatencyMode&, const LatencyMode&) = default;
};

enum class LatencyModeRequestKind : std::uint8_t { Auto = 0, PerOp = 1, Batch = 2 };

/// What the command line (or a test) asked for; auto resolves against the
/// measured granularity and the calibrated operation cost.
struct LatencyModeRequest {
  LatencyModeRequestKind kind = LatencyModeRequestKind::Auto;
  int batch_size = kMinBatchSize;  ///< used when kind == Batch

  std::string label() const;  ///< "auto", "per_op" or "batchN"

  friend bool operator==(const LatencyModeRequest&, const LatencyModeRequest&) = default;
};

/// Process-wide request read by every `LatencyRecorder`; set once at startup
/// from `--latency-mode` and restored by tests that force modes.
void set_latency_mode_request(LatencyModeRequest request);
LatencyModeRequest latency_mode_request();

/// Parses "auto", "per_op" or "batchN" (N at least 1). Returns false and
/// leaves `out` untouched on anything else.
bool parse_latency_mode_request(std::string_view text, LatencyModeRequest& out);

/// Resolves a request into the mode a recorder uses. A forced request wins.
/// Auto mode selects per-op only when the measured granularity is at most
/// `kPerOpMaxGranularityNs`; otherwise it picks the smallest power-of-two batch
/// of at least `kMinBatchSize` operations whose calibrated estimate spans
/// `kBatchGranularityTarget` timer steps (capped at `kMaxBatchSize`).
LatencyMode select_latency_mode(LatencyModeRequest request, double granularity_ns,
                                double typical_op_ns);

/// Mean per-call cost of `op` in nanoseconds, from a short calibration: `op`
/// is called until the measured span exceeds `kBatchGranularityTarget` timer
/// steps, starting at 64 calls and doubling. Runs `op` for real, so callers
/// should calibrate on the same operation they are about to time.
template <class F>
double calibrate_op_ns(F&& op) {
  constexpr std::uint64_t kMaxIterations = std::uint64_t{1} << 20U;
  double span_target_ns = kBatchGranularityTarget * timer_granularity().step_ns;
  if (!(span_target_ns > 0.0)) {
    span_target_ns = 1000.0;  // a frozen counter cannot be calibrated against
  }
  for (int i = 0; i < 32; ++i) {
    op();  // warm up caches and the branch predictor
  }
  std::uint64_t iterations = 64;
  double span_ns = 0.0;
  for (;;) {
    const Ticks begin = now_ticks();
    for (std::uint64_t i = 0; i < iterations; ++i) {
      op();
    }
    const Ticks end = now_ticks();
    span_ns = ticks_to_ns(end - begin);
    if (span_ns >= span_target_ns || iterations >= kMaxIterations) {
      break;
    }
    iterations *= 2;
  }
  return span_ns > 0.0 ? span_ns / static_cast<double>(iterations) : 0.0;
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
/// One `record_op` call performs `ops_per_sample()` operations and records one
/// sample of `(elapsed - overhead) / ops_per_sample()`: one operation per
/// sample in per-op mode, the per-op average of a batch otherwise. Samples
/// above the 10 ms ceiling are recorded at the ceiling and counted separately.
class LatencyRecorder {
 public:
  /// Uses `kFallbackOpEstimateNs` for auto mode; prefer the calibrating
  /// constructors below when the real operation is available.
  LatencyRecorder();

  /// Auto mode sizes its batch from `typical_op_ns`. The auto decision is made
  /// once per process, from the first operation calibrated, so every benchmark
  /// repetition in a run reports the same mode label and the before/after
  /// comparison does not mix batch sizes.
  explicit LatencyRecorder(double typical_op_ns);

  /// Calibrates `op`'s cost (a short, real run) and uses that estimate, so the
  /// batch is sized for the workload actually being measured.
  template <class F>
    requires std::invocable<F&> && (!std::same_as<std::remove_cvref_t<F>, LatencyRecorder>)
  explicit LatencyRecorder(F&& op) : LatencyRecorder(calibrate_op_ns(op)) {}

  ~LatencyRecorder();

  LatencyRecorder(const LatencyRecorder&) = delete;
  LatencyRecorder& operator=(const LatencyRecorder&) = delete;
  LatencyRecorder(LatencyRecorder&&) = delete;
  LatencyRecorder& operator=(LatencyRecorder&&) = delete;

  /// Times `ops_per_sample()` operations and records one sample. Returns the
  /// number of operations performed, so callers can account for the mode
  /// uniformly.
  template <class F>
  int record_op(F&& op) {
    const int operations = mode_.ops_per_sample();
    const Ticks begin = now_ticks();
    for (int i = 0; i < operations; ++i) {
      op();
    }
    const Ticks end = now_ticks();
    record_ticks(end - begin, operations);
    return operations;
  }

  Percentiles percentiles() const;

  /// Mean of the recorded samples in nanoseconds (0 when empty).
  double mean_ns() const;

  LatencyMode mode() const { return mode_; }
  std::string label() const { return mode_.label(); }

  /// Operations per recorded sample: 1 in per-op mode, else the batch size.
  int ops_per_sample() const { return mode_.ops_per_sample(); }

  double overhead_ns() const { return overhead_.ns; }
  Ticks overhead_ticks() const { return overhead_.ticks; }

  /// Samples recorded (not operations; multiply by ops_per_sample()).
  std::uint64_t sample_count() const;

  /// Samples that hit the 10 ms ceiling.
  std::uint64_t overflow_count() const { return overflow_count_; }

 private:
  void record_ticks(Ticks elapsed, int ops);

  hdr_histogram* histogram_ = nullptr;
  LatencyMode mode_ = LatencyMode::per_op();
  TimerOverhead overhead_{0, 0.0};
  std::uint64_t overflow_count_ = 0;
};

}  // namespace lob::bench
