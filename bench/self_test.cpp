#include "bench/self_test.hpp"

#include <algorithm>
#include <array>
#include <cinttypes>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <iterator>
#include <string>
#include <vector>

#include "bench/latency.hpp"
#include "bench/timer.hpp"

namespace lob::bench {
namespace {

/// A dependent unsigned multiply-add chain: each iteration depends on the
/// previous one, so the loop cannot be vectorized or folded away, and `asm`
/// keeps the compiler from removing the state update.
std::uint64_t g_black_hole = 1;

void spin(std::uint64_t iterations) {
  std::uint64_t state = g_black_hole;
  for (std::uint64_t i = 0; i < iterations; ++i) {
    state = state * 6364136223846793005ULL + 1442695040888963407ULL;
    __asm__ __volatile__("" : "+r"(state));
  }
  g_black_hole = state;
}

/// Preemption only ever adds time to a measurement, so the minimum over
/// several trials is the cleanest estimate of how long a piece of code takes
/// when it is actually running. Nine trials (an odd number, so the median is a
/// real sample) give an outlier-resistant minimum and a median for the log.
constexpr int kCalibrationTrials = 9;

/// Each calibration trial times enough back-to-back calls that the window is
/// thousands of timer steps long: 200 us against a 41.7 ns step makes read
/// quantization and timer overhead irrelevant.
constexpr double kCalibrationWindowNs = 200'000.0;

/// A calibrated delay is only used when its minimum long-window cost is
/// within this fraction of the target. The self-test's own p50 band is wider
/// (+-25 %), so a calibration that passes here leaves room for the natural
/// scatter of the recorded samples.
constexpr double kCalibrationTolerance = 0.10;

/// Upper bound on refinement passes. Each pass measures the delay the current
/// iteration count actually produces and rescales it; a handful of passes is
/// ample when the probe was taken on a different core.
constexpr int kMaxCalibrationPasses = 8;

/// Minimum and maximum calls per calibration window, so very small and very
/// large targets still produce a measurable window.
constexpr std::uint64_t kMinWindowCalls = 32;
constexpr std::uint64_t kMaxWindowCalls = 65'536;

/// Minimum, median and maximum of the per-call cost over
/// `kCalibrationTrials` long windows.
struct WindowStats {
  double min_ns = 0.0;
  double median_ns = 0.0;
  double max_ns = 0.0;
};

/// Calls per window for a target delay: enough that one window spans
/// `kCalibrationWindowNs`, clamped so tiny targets stay cheap and huge ones
/// do not run for milliseconds per call.
std::uint64_t window_calls_for(double target_ns) {
  if (!(target_ns > 0.0)) {
    return kMinWindowCalls;
  }
  const auto calls = static_cast<std::uint64_t>(std::llround(kCalibrationWindowNs / target_ns));
  return std::clamp<std::uint64_t>(calls, kMinWindowCalls, kMaxWindowCalls);
}

/// Times `calls` back-to-back `spin(iterations)` calls `kCalibrationTrials`
/// times and reduces each window to a per-call cost.
WindowStats measure_windows(std::uint64_t iterations, std::uint64_t calls) {
  std::array<double, kCalibrationTrials> samples{};
  for (double& sample : samples) {
    const Ticks begin = now_ticks();
    for (std::uint64_t call = 0; call < calls; ++call) {
      spin(iterations);
    }
    const Ticks end = now_ticks();
    sample = ticks_to_ns(end - begin) / static_cast<double>(calls);
  }
  std::sort(samples.begin(), samples.end());
  return WindowStats{samples.front(), samples[kCalibrationTrials / 2], samples.back()};
}

/// Cost of one `spin` iteration, as the minimum over `kCalibrationTrials`
/// long probe runs. A quarter-million iterations is a few hundred
/// microseconds per trial: long enough that read quantization is negligible,
/// short enough that the whole probe stays a couple of milliseconds.
WindowStats probe_iteration_ns() {
  constexpr std::uint64_t kProbeIterations = 250'000;
  spin(kProbeIterations);  // warm up the loop and the branch predictor
  std::array<double, kCalibrationTrials> samples{};
  for (double& sample : samples) {
    const Ticks begin = now_ticks();
    spin(kProbeIterations);
    const Ticks end = now_ticks();
    sample = ticks_to_ns(end - begin) / static_cast<double>(kProbeIterations);
  }
  std::sort(samples.begin(), samples.end());
  return WindowStats{samples.front(), samples[kCalibrationTrials / 2], samples.back()};
}

/// A calibrated busy-wait: how many `spin` iterations approximate `target_ns`,
/// plus the evidence that the raw counter agreed with the target before the
/// delay was handed to the recorder.
struct SpinCalibration {
  std::uint64_t iterations = 0;
  std::uint64_t calls = 0;
  WindowStats probe;   ///< per-iteration cost, min/median/max over the probe trials
  WindowStats verify;  ///< per-call cost, min/median/max over the long verification windows
  int passes = 0;
  bool verified = false;

  double error_fraction(double target_ns) const {
    return target_ns > 0.0 ? (verify.min_ns - target_ns) / target_ns : 0.0;
  }
};

/// Calibrates a `spin` delay to `target_ns` and verifies it against the raw
/// counter over long windows. The probe supplies the starting iteration count;
/// each refinement pass then measures the delay actually produced and scales
/// the count, so a probe taken on a slow core (or a boosted one) cannot leave
/// the delay far from the target. Every trial minimum is taken over
/// `kCalibrationTrials` windows, and the final `verify` always belongs to the
/// iteration count that is returned.
SpinCalibration calibrate_spin(double target_ns) {
  SpinCalibration result;
  result.calls = window_calls_for(target_ns);
  result.probe = probe_iteration_ns();
  if (!(target_ns > 0.0) || !(result.probe.min_ns > 0.0)) {
    return result;  // verified stays false; the caller reports the numbers
  }

  result.iterations = std::max<std::uint64_t>(
      1, static_cast<std::uint64_t>(std::llround(target_ns / result.probe.min_ns)));
  for (int pass = 0; pass < kMaxCalibrationPasses; ++pass) {
    result.verify = measure_windows(result.iterations, result.calls);
    result.passes = pass + 1;
    if (!(result.verify.min_ns > 0.0)) {
      break;
    }
    if (std::abs(result.verify.min_ns - target_ns) <= kCalibrationTolerance * target_ns) {
      break;  // already inside the band; scaling further only chases noise
    }
    const auto adjusted = static_cast<std::uint64_t>(
        std::llround(static_cast<double>(result.iterations) * target_ns / result.verify.min_ns));
    const auto clamped = std::clamp<std::uint64_t>(adjusted, 1, std::uint64_t{1} << 24U);
    if (clamped == result.iterations) {
      break;
    }
    result.iterations = clamped;
  }

  // Always verify the exact iteration count that is returned, even when the
  // last refinement pass changed it.
  result.verify = measure_windows(result.iterations, result.calls);
  result.verified = result.verify.min_ns > 0.0 &&
                    std::abs(result.verify.min_ns - target_ns) <= kCalibrationTolerance * target_ns;
  return result;
}

/// Runs a calibrated ~target_ns delay through the recorder with `request`
/// forced (auto resolves against the measured granularity) and checks that the
/// reported p50 lands within +-25 % of the target. Each attempt re-calibrates
/// immediately before the measurement, verifies the delay against the raw
/// counter over long windows both before and after recording, and retries on a
/// fresh calibration when the machine's apparent speed changed underneath the
/// measurement. A failure therefore reports a delay that the raw counter
/// agrees with, not a stale calibration.
bool check_delay(double target_ns, LatencyModeRequest request, const char* what) {
  constexpr double kTolerance = 0.25;
  constexpr std::size_t kSamples = 2001;
  constexpr int kMaxAttempts = 3;
  const double low = target_ns * (1.0 - kTolerance);
  const double high = target_ns * (1.0 + kTolerance);

  SpinCalibration calibration;
  Percentiles p;
  std::string mode_label = request.label();
  WindowStats after;
  bool measured = false;
  for (int attempt = 1; attempt <= kMaxAttempts; ++attempt) {
    set_latency_mode_request(request);
    calibration = calibrate_spin(target_ns);  // immediately before this measurement
    if (!calibration.verified) {
      std::printf(
          "self-test: %s target ~%.0f ns attempt %d/%d: calibrated delay did not verify "
          "(probe min %.3f ns/iteration, median %.3f; %" PRIu64 " iterations x %" PRIu64
          " calls; long-window min %.1f ns, median %.1f ns; tolerance +-%.0f%%)\n",
          what, target_ns, attempt, kMaxAttempts, calibration.probe.min_ns,
          calibration.probe.median_ns, calibration.iterations, calibration.calls,
          calibration.verify.min_ns, calibration.verify.median_ns, kCalibrationTolerance * 100.0);
      continue;
    }

    LatencyRecorder recorder(target_ns);
    mode_label = recorder.label();
    for (std::size_t sample = 0; sample < kSamples; ++sample) {
      recorder.record_op([iterations = calibration.iterations] { spin(iterations); });
    }
    p = recorder.percentiles();
    after = measure_windows(calibration.iterations, calibration.calls);
    measured = true;

    const bool p50_ok =
        static_cast<double>(p.p50_ns) >= low && static_cast<double>(p.p50_ns) <= high;
    const bool after_ok = std::abs(after.min_ns - target_ns) <= kCalibrationTolerance * target_ns;
    std::printf(
        "self-test: %s mode %s attempt %d/%d: target ~%.0f ns -> p50 %" PRIu64 " ns, p99 %" PRIu64
        " ns, max %" PRIu64
        " ns (tolerance %.0f..%.0f ns); calibration probe min %.3f ns/iteration (median %.3f), "
        "%" PRIu64 " iterations x %" PRIu64
        " calls after %d pass%s; verify min %.1f / median %.1f / max %.1f ns; "
        "post-check min %.1f ns [%s]\n",
        what, mode_label.c_str(), attempt, kMaxAttempts, target_ns, p.p50_ns, p.p99_ns, p.max_ns,
        low, high, calibration.probe.min_ns, calibration.probe.median_ns, calibration.iterations,
        calibration.calls, calibration.passes, calibration.passes == 1 ? "" : "es",
        calibration.verify.min_ns, calibration.verify.median_ns, calibration.verify.max_ns,
        after.min_ns, (p50_ok && after_ok) ? "ok" : "retrying");

    if (p50_ok && after_ok) {
      return true;
    }
    if (attempt < kMaxAttempts) {
      std::printf(
          "self-test: %s target ~%.0f ns did not land in tolerance; re-calibrating and "
          "retrying (measurement environment changed under load)\n",
          what, target_ns);
    }
  }

  if (!measured) {
    std::fprintf(stderr,
                 "self-test FAILURE: %s mode %s, target ~%.0f ns: no calibrated delay passed the "
                 "raw-counter check in %d attempts (probe min %.3f ns/iteration, median %.3f; "
                 "last calibration %" PRIu64 " iterations x %" PRIu64
                 " calls, verify min %.1f / median %.1f ns, error %+.1f%%, tolerance +-%.0f%%)\n",
                 what, mode_label.c_str(), target_ns, kMaxAttempts, calibration.probe.min_ns,
                 calibration.probe.median_ns, calibration.iterations, calibration.calls,
                 calibration.verify.min_ns, calibration.verify.median_ns,
                 calibration.error_fraction(target_ns) * 100.0, kCalibrationTolerance * 100.0);
    return false;
  }
  std::fprintf(stderr,
               "self-test FAILURE: %s mode %s, target ~%.0f ns -> p50 %" PRIu64
               " ns outside %.0f..%.0f ns; calibration probe min %.3f ns/iteration (median "
               "%.3f), %" PRIu64 " iterations x %" PRIu64
               " calls after %d pass%s, verify min %.1f / median %.1f / max %.1f ns, post-check "
               "min %.1f ns, calibration error %+.1f%% (tolerance +-%.0f%%)\n",
               what, mode_label.c_str(), target_ns, p.p50_ns, low, high, calibration.probe.min_ns,
               calibration.probe.median_ns, calibration.iterations, calibration.calls,
               calibration.passes, calibration.passes == 1 ? "" : "es", calibration.verify.min_ns,
               calibration.verify.median_ns, calibration.verify.max_ns, after.min_ns,
               calibration.error_fraction(target_ns) * 100.0, kCalibrationTolerance * 100.0);
  return false;
}

/// In auto mode the recorder must pick per-op only when the measured
/// granularity is at most `kPerOpMaxGranularityNs`, and otherwise the smallest
/// power-of-two batch of at least `kMinBatchSize` whose calibrated cost
/// estimate spans `kBatchGranularityTarget` timer steps.
bool check_auto_mode(const GranularityMeasurement& granularity) {
  constexpr double kEstimateNs = 200.0;
  set_latency_mode_request(LatencyModeRequest{});
  LatencyRecorder recorder(kEstimateNs);
  const LatencyMode mode = recorder.mode();
  const LatencyMode expected =
      select_latency_mode(LatencyModeRequest{}, granularity.step_ns, kEstimateNs);
  bool ok = mode == expected;
  const double span_target_ns = kBatchGranularityTarget * granularity.step_ns;
  if (granularity.step_ns <= kPerOpMaxGranularityNs) {
    ok = ok && mode.is_per_op();
  } else {
    const int batch_size = mode.batch_size;
    ok = ok && !mode.is_per_op();
    ok = ok && batch_size >= kMinBatchSize;
    ok = ok && (batch_size & (batch_size - 1)) == 0;
    ok = ok && static_cast<double>(batch_size) * kEstimateNs >= span_target_ns;
    ok = ok && (batch_size == kMinBatchSize ||
                static_cast<double>(batch_size / 2) * kEstimateNs < span_target_ns);
  }
  std::printf("self-test: auto mode %s for granularity %.1f ns with a %.0f ns op estimate [%s]\n",
              mode.label().c_str(), granularity.step_ns, kEstimateNs, ok ? "ok" : "FAILED");
  return ok;
}

/// The `--latency-mode` values the harness accepts and rejects.
bool check_parsing() {
  bool ok = true;
  LatencyModeRequest request;
  ok = ok && parse_latency_mode_request("auto", request) && request.label() == "auto";
  ok = ok && parse_latency_mode_request("per_op", request) && request.label() == "per_op";
  ok = ok && parse_latency_mode_request("batch64", request) && request.label() == "batch64";
  ok = ok && !parse_latency_mode_request("batch0", request);
  ok = ok && !parse_latency_mode_request("batch", request);
  ok = ok && !parse_latency_mode_request("perop", request);
  ok = ok && !parse_latency_mode_request("batch65536", request);
  std::printf("self-test: --latency-mode parsing [%s]\n", ok ? "ok" : "FAILED");
  return ok;
}

}  // namespace

int run_self_test() {
  const LatencyModeRequest saved_request = latency_mode_request();
  reset_latency_mode_decision();  // check_auto_mode must decide from its own estimate
  const TimerInfo info = timer_info();
  const GranularityMeasurement granularity = timer_granularity();
  const TimerOverhead overhead = calibrate_overhead();
  std::printf("self-test: counter %s at %" PRIu64 " Hz (measured %" PRIu64
              " Hz against the mach timebase at %" PRIu64 " Hz over %.0f ms: %s)\n",
              info.counter_name, static_cast<std::uint64_t>(std::llround(info.ticks_per_second)),
              static_cast<std::uint64_t>(std::llround(info.measured_ticks_per_second)),
              static_cast<std::uint64_t>(std::llround(info.mach_ticks_per_second)),
              kCrossCheckWindowSeconds * 1e3, info.cross_check_agrees ? "agree" : "DISAGREE");
  std::printf("self-test: timer %s\n", describe_granularity(granularity).c_str());
  std::printf("self-test: timer overhead %" PRIu64 " ticks (%.1f ns)\n", overhead.ticks,
              overhead.ns);

  bool ok = true;
  std::vector<std::string> failures;
  const auto check = [&ok, &failures](bool passed, const std::string& name) {
    ok = passed && ok;
    if (!passed) {
      failures.push_back(name);
    }
  };
  check(check_auto_mode(granularity), "auto mode selection");
  check(check_parsing(), "--latency-mode parsing");
  // Auto mode on the measured granularity, then each forced mode. The
  // per-op check uses a delay well above the timer step so quantization cannot
  // move the p50 outside the tolerance band; the batch check uses a delay far
  // below it to prove that batching resolves sub-step operations.
  check(check_delay(200.0, LatencyModeRequest{}, "auto"), "auto mode, 200 ns target");
  check(check_delay(2000.0, LatencyModeRequest{}, "auto"), "auto mode, 2000 ns target");
  check(check_delay(2000.0, LatencyModeRequest{LatencyModeRequestKind::PerOp, 1}, "forced per-op"),
        "forced per-op, 2000 ns target");
  check(check_delay(200.0, LatencyModeRequest{LatencyModeRequestKind::Batch, 64}, "forced batch64"),
        "forced batch64, 200 ns target");

  set_latency_mode_request(saved_request);
  reset_latency_mode_decision();
  if (!ok) {
    std::fprintf(stderr, "self-test FAILED: %zu check(s) did not pass:", failures.size());
    for (const std::string& failure : failures) {
      std::fprintf(stderr, " [%s]", failure.c_str());
    }
    std::fprintf(stderr,
                 "\nself-test FAILED: reported percentiles do not match the synthetic delays; "
                 "do not trust measurements from this machine right now\n");
    return 1;
  }
  std::printf("self-test PASSED\n");
  return 0;
}

}  // namespace lob::bench
