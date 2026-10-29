#pragma once

// HdrHistogram-backed LatencySink used by lob_loadgen. Only the application
// links the histogram library; the tests drive the runner with their own sink,
// so the load generator's core stays free of the dependency.

#include <cstdint>

#include "net/loadgen/latency.hpp"

struct hdr_histogram;

namespace lob::loadgen {

/// Records whole nanoseconds from 1 ns to 60 s at three significant digits.
/// Samples above the ceiling are clamped at the ceiling and counted separately
/// so a pathological run cannot silently lose them.
class HdrLatencySink final : public LatencySink {
 public:
  HdrLatencySink();
  ~HdrLatencySink() override;
  HdrLatencySink(const HdrLatencySink&) = delete;
  HdrLatencySink& operator=(const HdrLatencySink&) = delete;

  void record(std::uint64_t latency_ns) override;

  LatencySummary summary() const;

  /// True when the histogram allocated successfully.
  bool ok() const { return histogram_ != nullptr; }

  /// Samples above the 60 s ceiling (clamped, never dropped silently).
  std::uint64_t clamped_samples() const { return clamped_; }

 private:
  hdr_histogram* histogram_ = nullptr;
  std::uint64_t clamped_ = 0;
};

}  // namespace lob::loadgen
