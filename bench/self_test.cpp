#include "bench/self_test.hpp"

#include <algorithm>
#include <cinttypes>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <iterator>
#include <string>

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

/// Number of `spin` iterations whose measured cost is closest to target_ns.
/// A long probe gives the starting guess; three short refinement passes then
/// measure the actual loop and scale it, which absorbs frequency and branch
/// predictor state differences between the probe and the timed loop.
std::uint64_t iterations_for_ns(double target_ns) {
  constexpr std::uint64_t kProbeIterations = 1'000'000;
  spin(kProbeIterations);  // warm up
  const Ticks probe_begin = now_ticks();
  spin(kProbeIterations);
  const Ticks probe_end = now_ticks();
  const double per_iteration_ns =
      ticks_to_ns(probe_end - probe_begin) / static_cast<double>(kProbeIterations);

  auto iterations = static_cast<std::uint64_t>(std::llround(target_ns / per_iteration_ns));
  if (iterations == 0) {
    iterations = 1;
  }
  for (int pass = 0; pass < 3; ++pass) {
    Ticks samples[5];
    for (Ticks& sample : samples) {
      const Ticks begin = now_ticks();
      spin(iterations);
      sample = now_ticks() - begin;
    }
    std::sort(std::begin(samples), std::end(samples));
    const double measured_ns = ticks_to_ns(samples[2]);
    if (measured_ns <= 0.0) {
      break;
    }
    const auto adjusted = static_cast<std::uint64_t>(
        std::llround(static_cast<double>(iterations) * target_ns / measured_ns));
    iterations = adjusted == 0 ? 1 : adjusted;
  }
  return iterations;
}

/// Runs a calibrated ~target_ns delay through the recorder with `request`
/// forced (auto resolves against the measured granularity) and checks that the
/// reported p50 lands within +-25 % of the target.
bool check_delay(double target_ns, LatencyModeRequest request, const char* what) {
  constexpr double kTolerance = 0.25;
  constexpr std::size_t kSamples = 2001;
  set_latency_mode_request(request);
  const std::uint64_t iterations = iterations_for_ns(target_ns);
  LatencyRecorder recorder(target_ns);  // the synthetic delay is calibrated by construction
  for (std::size_t sample = 0; sample < kSamples; ++sample) {
    recorder.record_op([iterations] { spin(iterations); });
  }
  const Percentiles p = recorder.percentiles();
  const double low = target_ns * (1.0 - kTolerance);
  const double high = target_ns * (1.0 + kTolerance);
  const bool ok = static_cast<double>(p.p50_ns) >= low && static_cast<double>(p.p50_ns) <= high;
  std::printf("self-test: %s mode %s: target ~%.0f ns -> p50 %" PRIu64 " ns, p99 %" PRIu64
              " ns, max %" PRIu64 " ns (tolerance %.0f..%.0f ns) [%s]\n",
              what, recorder.label().c_str(), target_ns, p.p50_ns, p.p99_ns, p.max_ns, low, high,
              ok ? "ok" : "FAILED");
  return ok;
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
  ok = check_auto_mode(granularity) && ok;
  ok = check_parsing() && ok;
  // Auto mode on the measured granularity, then each forced mode. The
  // per-op check uses a delay well above the timer step so quantization cannot
  // move the p50 outside the tolerance band; the batch check uses a delay far
  // below it to prove that batching resolves sub-step operations.
  ok = check_delay(200.0, LatencyModeRequest{}, "auto") && ok;
  ok = check_delay(2000.0, LatencyModeRequest{}, "auto") && ok;
  ok = check_delay(2000.0, LatencyModeRequest{LatencyModeRequestKind::PerOp, 1}, "forced per-op") &&
       ok;
  ok =
      check_delay(200.0, LatencyModeRequest{LatencyModeRequestKind::Batch, 64}, "forced batch64") &&
      ok;

  set_latency_mode_request(saved_request);
  reset_latency_mode_decision();
  if (!ok) {
    std::fprintf(stderr,
                 "self-test FAILED: reported percentiles do not match the synthetic delays; "
                 "do not trust measurements from this machine right now\n");
    return 1;
  }
  std::printf("self-test PASSED\n");
  return 0;
}

}  // namespace lob::bench
