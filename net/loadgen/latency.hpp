#pragma once

// Latency recording for the load generator. The runner measures a
// standard-library steady_clock duration and hands whole nanoseconds to a
// LatencySink. Keeping the histogram behind this interface lets the tests use
// a plain sample collector while the lob_loadgen binary records into an
// HdrHistogram.

#include <cstdint>

namespace lob::loadgen {

/// One recorded latency distribution. Percentiles are nearest-rank values of
/// the observed samples; zero samples leave every field at zero.
struct LatencySummary {
  std::uint64_t count = 0;
  std::uint64_t p50_ns = 0;
  std::uint64_t p90_ns = 0;
  std::uint64_t p99_ns = 0;
  std::uint64_t p999_ns = 0;
  std::uint64_t max_ns = 0;
  double mean_ns = 0.0;
};

class LatencySink {
 public:
  virtual ~LatencySink() = default;
  virtual void record(std::uint64_t latency_ns) = 0;
};

}  // namespace lob::loadgen
