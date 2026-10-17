#include "bench/latency.hpp"

#include <hdr/hdr_histogram.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace lob::bench {
namespace {

/// 10 ms in nanoseconds: the ceiling of the HDR range (1 ns .. 10 ms).
constexpr std::int64_t kHighestTrackableNs = 10'000'000;

/// Records one whole-nanosecond sample, clamped at zero.
void record_ns(hdr_histogram* histogram, double ns) {
  std::int64_t value = static_cast<std::int64_t>(std::llround(ns));
  if (value < 0) {
    value = 0;
  }
  hdr_record_value(histogram, value);
}

}  // namespace

LatencyRecorder::LatencyRecorder()
    : mode_(latency_mode_for(ticks_per_second())), overhead_(calibrate_overhead()) {
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
