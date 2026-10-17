#include "bench/self_test.hpp"

#include <algorithm>
#include <cinttypes>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <iterator>

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

bool check_delay(double target_ns) {
  constexpr double kTolerance = 0.25;
  constexpr std::size_t kSamples = 2001;
  const std::uint64_t iterations = iterations_for_ns(target_ns);
  LatencyRecorder recorder;
  for (std::size_t sample = 0; sample < kSamples; ++sample) {
    recorder.record_op([iterations] { spin(iterations); });
  }
  const Percentiles p = recorder.percentiles();
  const double low = target_ns * (1.0 - kTolerance);
  const double high = target_ns * (1.0 + kTolerance);
  const bool ok = static_cast<double>(p.p50_ns) >= low && static_cast<double>(p.p50_ns) <= high;
  std::printf("self-test: target ~%.0f ns -> p50 %" PRIu64 " ns, p99 %" PRIu64 " ns, max %" PRIu64
              " ns (tolerance %.0f..%.0f ns) [%s]\n",
              target_ns, p.p50_ns, p.p99_ns, p.max_ns, low, high, ok ? "ok" : "FAILED");
  return ok;
}

const char* mode_name(LatencyMode mode) {
  return mode == LatencyMode::PerOp ? "per_op" : "batch16";
}

}  // namespace

int run_self_test() {
  const TimerInfo info = timer_info();
  const TimerOverhead overhead = calibrate_overhead();
  const LatencyMode mode = latency_mode_for(info.ticks_per_second);
  std::printf(
      "self-test: counter %s at %.0f Hz (measured %.0f Hz against the mach timebase at %.0f Hz: "
      "%s); mode %s; overhead %" PRIu64 " ticks (%.1f ns)\n",
      info.counter_name, info.ticks_per_second, info.measured_ticks_per_second,
      info.mach_ticks_per_second, info.cross_check_agrees ? "agree" : "DISAGREE", mode_name(mode),
      overhead.ticks, overhead.ns);

  const bool small_ok = check_delay(200.0);
  const bool large_ok = check_delay(2000.0);
  if (!small_ok || !large_ok) {
    std::fprintf(stderr,
                 "self-test FAILED: reported percentiles do not match the synthetic delays; "
                 "do not trust measurements from this machine right now\n");
    return 1;
  }
  std::printf("self-test PASSED\n");
  return 0;
}

}  // namespace lob::bench
