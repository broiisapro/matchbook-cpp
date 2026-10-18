#include <benchmark/benchmark.h>

#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
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
  lob::bench::GranularityMeasurement granularity;
  lob::bench::TimerOverhead overhead;
  lob::bench::LatencyModeRequest request;
  int qos_status;
};

constexpr std::string_view kLatencyModeFlag = "--latency-mode=";

/// Splits the harness's own flags out of argv before Google Benchmark sees
/// them. Only `--latency-mode=auto|per_op|batchN` is ours.
bool parse_command_line(int argc, char** argv, std::vector<char*>& benchmark_args,
                        lob::bench::LatencyModeRequest& request) {
  benchmark_args.push_back(argv[0]);
  for (int i = 1; i < argc; ++i) {
    const std::string_view argument(argv[i]);
    if (argument.starts_with(kLatencyModeFlag)) {
      if (!lob::bench::parse_latency_mode_request(argument.substr(kLatencyModeFlag.size()),
                                                  request)) {
        std::fprintf(stderr,
                     "lob_bench: invalid --latency-mode value '%.*s'; use auto, per_op or batchN\n",
                     static_cast<int>(argument.size() - kLatencyModeFlag.size()),
                     argv[i] + kLatencyModeFlag.size());
        return false;
      }
      continue;
    }
    benchmark_args.push_back(argv[i]);
  }
  return true;
}

bool has_flag(const std::vector<char*>& args, const char* flag) {
  for (const char* argument : args) {
    if (std::strcmp(argument, flag) == 0) {
      return true;
    }
  }
  return false;
}

bool has_flag_prefix(const std::vector<char*>& args, const char* prefix) {
  const std::size_t length = std::strlen(prefix);
  for (const char* argument : args) {
    if (std::strncmp(argument, prefix, length) == 0) {
      return true;
    }
  }
  return false;
}

std::uint64_t to_integer_hz(double hz) { return static_cast<std::uint64_t>(std::llround(hz)); }

/// Prints the timing environment on every run: the counter and its integer
/// frequency, the mach cross-check, the measured granularity and its delta
/// distribution, the requested latency mode, the measured timer overhead and
/// the QoS hint. Goes to stderr so stdout stays machine-readable for
/// `--benchmark_format=json`.
Prologue print_prologue(const lob::bench::LatencyModeRequest& request) {
  const int qos_status = lob::bench::set_user_interactive_qos();
  const lob::bench::TimerInfo info = lob::bench::timer_info();
  const lob::bench::GranularityMeasurement granularity = lob::bench::timer_granularity();
  const lob::bench::TimerOverhead overhead = lob::bench::calibrate_overhead();

  std::fprintf(stderr, "lob_bench: counter %s at %" PRIu64 " Hz", info.counter_name,
               to_integer_hz(info.ticks_per_second));
  if (std::strcmp(info.counter_name, "cntvct_el0") == 0) {
    std::fprintf(stderr,
                 " (mach timebase %" PRIu64 " Hz; measured %" PRIu64
                 " Hz against it over %.0f ms, %s at %.1f%% tolerance)",
                 to_integer_hz(info.mach_ticks_per_second),
                 to_integer_hz(info.measured_ticks_per_second),
                 lob::bench::kCrossCheckWindowSeconds * 1e3,
                 info.cross_check_agrees ? "cross-check agrees" : "cross-check DISAGREES",
                 lob::bench::kCrossCheckTolerance * 100.0);
  }
  std::fprintf(stderr, "\nlob_bench: timer %s\n",
               lob::bench::describe_granularity(granularity).c_str());
  std::fprintf(stderr,
               "lob_bench: latency mode request %s (per-op only when granularity <= %.0f ns; "
               "otherwise the smallest power-of-two batch spanning >= %d steps, min %d, max %d)\n",
               request.label().c_str(), lob::bench::kPerOpMaxGranularityNs,
               lob::bench::kBatchGranularityTarget, lob::bench::kMinBatchSize,
               lob::bench::kMaxBatchSize);
  std::fprintf(stderr, "lob_bench: timer overhead %" PRIu64 " ticks (%.1f ns)\n", overhead.ticks,
               overhead.ns);
  std::fprintf(
      stderr,
      "lob_bench: qos hint pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0) -> %d "
      "(%s)\n",
      qos_status, qos_status == 0 ? "applied" : "failed");
  std::fflush(stderr);
  return Prologue{info, granularity, overhead, request, qos_status};
}

std::string format_granularity_ns(const lob::bench::GranularityMeasurement& granularity) {
  char buffer[32];
  std::snprintf(buffer, sizeof(buffer), "%.1f", granularity.step_ns);
  return buffer;
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
  timer += std::to_string(to_integer_hz(prologue.timer.ticks_per_second));
  timer += " Hz";

  benchmark::AddCustomContext("compiler", __clang_version__);
  benchmark::AddCustomContext("build_flags", build_flags);
  benchmark::AddCustomContext("cpu", lob::bench::cpu_brand());
  benchmark::AddCustomContext("os", lob::bench::os_version());
  benchmark::AddCustomContext("timer", timer);
  benchmark::AddCustomContext("timer_granularity_ns", format_granularity_ns(prologue.granularity));
  benchmark::AddCustomContext("timer_overhead_ns", std::to_string(prologue.overhead.ns));
  benchmark::AddCustomContext("latency_mode", prologue.request.label());
}

}  // namespace

int main(int argc, char** argv) {
  std::vector<char*> benchmark_args;
  lob::bench::LatencyModeRequest request;
  if (!parse_command_line(argc, argv, benchmark_args, request)) {
    return 2;
  }
  lob::bench::set_latency_mode_request(request);

  const Prologue prologue = print_prologue(request);

  if (has_flag(benchmark_args, "--self-test")) {
    const int status = lob::bench::run_self_test();
    std::fflush(stdout);
    return status;
  }

  // The default output is bench.json next to the executable; an explicit
  // --benchmark_out wins.
  std::vector<std::string> extra_flags;
  if (!has_flag_prefix(benchmark_args, "--benchmark_out")) {
    extra_flags.push_back("--benchmark_out=" + lob::bench::executable_directory() + "/bench.json");
    extra_flags.push_back("--benchmark_out_format=json");
  }
  for (std::string& flag : extra_flags) {
    benchmark_args.push_back(flag.data());
  }
  int adjusted_argc = static_cast<int>(benchmark_args.size());

  benchmark::Initialize(&adjusted_argc, benchmark_args.data());
  if (benchmark::ReportUnrecognizedArguments(adjusted_argc, benchmark_args.data())) {
    return 1;
  }
  add_context(prologue);
  benchmark::RunSpecifiedBenchmarks();
  benchmark::Shutdown();
  return lob::bench::harness_failures() == 0 ? 0 : 1;
}
