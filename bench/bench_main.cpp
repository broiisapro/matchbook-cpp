#include <benchmark/benchmark.h>

#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <string>

#include "bench/latency.hpp"
#include "bench/platform.hpp"
#include "bench/self_test.hpp"
#include "bench/timer.hpp"

namespace {

bool has_flag(int argc, char** argv, const char* flag) {
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], flag) == 0) {
      return true;
    }
  }
  return false;
}

/// Prints the timing environment on every run: the counter and its frequency,
/// the mach cross-check, the sample resolution, the measured timer overhead and
/// the QoS hint. Goes to stderr so stdout stays machine-readable for
/// `--benchmark_format=json`.
void print_prologue() {
  const int qos_status = lob::bench::set_user_interactive_qos();
  const lob::bench::TimerInfo info = lob::bench::timer_info();
  const lob::bench::TimerOverhead overhead = lob::bench::calibrate_overhead();
  const lob::bench::LatencyMode mode = lob::bench::latency_mode_for(info.ticks_per_second);

  std::fprintf(stderr, "lob_bench: counter %s at %.0f Hz", info.counter_name,
               info.ticks_per_second);
  if (std::strcmp(info.counter_name, "cntvct_el0") == 0) {
    std::fprintf(stderr, " (mach timebase %.0f Hz; measured %.0f Hz against it, %s)",
                 info.mach_ticks_per_second, info.measured_ticks_per_second,
                 info.cross_check_agrees ? "cross-check agrees" : "cross-check DISAGREES");
  }
  const std::string mode_label(lob::bench::to_string(mode));
  std::fprintf(stderr, "\nlob_bench: latency mode %s; timer overhead %" PRIu64 " ticks (%.1f ns)\n",
               mode_label.c_str(), overhead.ticks, overhead.ns);
  std::fprintf(
      stderr,
      "lob_bench: qos hint pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0) -> %d "
      "(%s)\n",
      qos_status, qos_status == 0 ? "applied" : "failed");
  std::fflush(stderr);
}

void BM_placeholder(benchmark::State& state) {
  for (auto _ : state) {
    benchmark::DoNotOptimize(state.iterations());
  }
}
BENCHMARK(BM_placeholder);

}  // namespace

int main(int argc, char** argv) {
  print_prologue();

  if (has_flag(argc, argv, "--self-test")) {
    const int status = lob::bench::run_self_test();
    std::fflush(stdout);
    return status;
  }

  benchmark::Initialize(&argc, argv);
  if (benchmark::ReportUnrecognizedArguments(argc, argv)) {
    return 1;
  }
  benchmark::RunSpecifiedBenchmarks();
  benchmark::Shutdown();
  return 0;
}
