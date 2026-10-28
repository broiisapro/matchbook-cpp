#include "net/loadgen/hdr_latency.hpp"

#include <hdr/hdr_histogram.h>

namespace lob::loadgen {
namespace {

/// 60 s in nanoseconds: a loopback round trip is microseconds, but a stalled
/// gateway thread can legitimately push a sample into the milliseconds and the
/// report should show it rather than clamp at the old 10 ms bench ceiling.
constexpr std::int64_t kHighestTrackableNs = 60'000'000'000;

}  // namespace

HdrLatencySink::HdrLatencySink() {
  if (hdr_init(1, kHighestTrackableNs, 3, &histogram_) != 0) {
    histogram_ = nullptr;
  }
}

HdrLatencySink::~HdrLatencySink() {
  if (histogram_ != nullptr) {
    hdr_close(histogram_);
  }
}

void HdrLatencySink::record(std::uint64_t latency_ns) {
  if (histogram_ == nullptr) {
    return;
  }
  std::int64_t value = static_cast<std::int64_t>(latency_ns);
  if (value < 0) {
    value = 0;
  }
  if (value > kHighestTrackableNs) {
    value = kHighestTrackableNs;
    ++clamped_;
  }
  hdr_record_value(histogram_, value);
}

LatencySummary HdrLatencySink::summary() const {
  LatencySummary summary;
  if (histogram_ == nullptr) {
    return summary;
  }
  summary.count = static_cast<std::uint64_t>(histogram_->total_count);
  if (summary.count == 0) {
    return summary;
  }
  summary.p50_ns = static_cast<std::uint64_t>(hdr_value_at_percentile(histogram_, 50.0));
  summary.p90_ns = static_cast<std::uint64_t>(hdr_value_at_percentile(histogram_, 90.0));
  summary.p99_ns = static_cast<std::uint64_t>(hdr_value_at_percentile(histogram_, 99.0));
  summary.p999_ns = static_cast<std::uint64_t>(hdr_value_at_percentile(histogram_, 99.9));
  summary.max_ns = static_cast<std::uint64_t>(hdr_max(histogram_));
  summary.mean_ns = hdr_mean(histogram_);
  return summary;
}

}  // namespace lob::loadgen
