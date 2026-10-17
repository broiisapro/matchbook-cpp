#include <benchmark/benchmark.h>

#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "bench/context.hpp"
#include "bench/latency.hpp"
#include "bench/platform.hpp"
#include "bench/self_test.hpp"
#include "bench/support.hpp"
#include "bench/timer.hpp"

namespace {

/// The measured timing environment, captured once at startup and reused for
/// the JSON context.
struct Prologue {
  lob::bench::TimerInfo timer;
  lob::bench::TimerOverhead overhead;
  lob::bench::LatencyMode mode;
  int qos_status;
};

bool has_flag(int argc, char** argv, const char* flag) {
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], flag) == 0) {
      return true;
    }
  }
  return false;
}

bool has_flag_prefix(int argc, char** argv, const char* prefix) {
  const std::size_t length = std::strlen(prefix);
  for (int i = 1; i < argc; ++i) {
    if (std::strncmp(argv[i], prefix, length) == 0) {
      return true;
    }
  }
  return false;
}

/// Prints the timing environment on every run: the counter and its frequency,
/// the mach cross-check, the sample resolution, the measured timer overhead and
/// the QoS hint. Goes to stderr so stdout stays machine-readable for
/// `--benchmark_format=json`.
Prologue print_prologue() {
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
  return Prologue{info, overhead, mode, qos_status};
}

/// Adds the run's environment to the JSON and console context so every
/// reported number carries the machine, compiler, flags and timer that
/// produced it.
void add_context(const Prologue& prologue) {
#ifdef LOB_BENCH_BUILD_FLAGS
  const char* build_flags = LOB_BENCH_BUILD_FLAGS;
#else
  const char* build_flags = "unknown";
#endif
  std::string timer = prologue.timer.counter_name;
  timer += " @ ";
  timer += std::to_string(prologue.timer.ticks_per_second);
  timer += " Hz";

  benchmark::AddCustomContext("compiler", __clang_version__);
  benchmark::AddCustomContext("build_flags", build_flags);
  benchmark::AddCustomContext("cpu", lob::bench::cpu_brand());
  benchmark::AddCustomContext("os", lob::bench::os_version());
  benchmark::AddCustomContext("timer", timer);
  benchmark::AddCustomContext("timer_overhead_ns", std::to_string(prologue.overhead.ns));
  benchmark::AddCustomContext("latency_mode", std::string(lob::bench::to_string(prologue.mode)));
}

}  // namespace

int main(int argc, char** argv) {
  const Prologue prologue = print_prologue();

  if (has_flag(argc, argv, "--self-test")) {
    const int status = lob::bench::run_self_test();
    std::fflush(stdout);
    return status;
  }

  // The default output is bench.json next to the executable; an explicit
  // --benchmark_out wins.
  std::vector<std::string> extra_flags;
  if (!has_flag_prefix(argc, argv, "--benchmark_out")) {
    extra_flags.push_back("--benchmark_out=" + lob::bench::executable_directory() + "/bench.json");
    extra_flags.push_back("--benchmark_out_format=json");
  }
  std::vector<char*> args(argv, argv + argc);
  for (std::string& flag : extra_flags) {
    args.push_back(flag.data());
  }
  int adjusted_argc = static_cast<int>(args.size());

  benchmark::Initialize(&adjusted_argc, args.data());
  if (benchmark::ReportUnrecognizedArguments(adjusted_argc, args.data())) {
    return 1;
  }
  add_context(prologue);
  benchmark::RunSpecifiedBenchmarks();
  benchmark::Shutdown();
  return lob::bench::harness_failures() == 0 ? 0 : 1;
}
