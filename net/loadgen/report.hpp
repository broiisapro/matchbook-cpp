#pragma once

// JSON report for the load generator. The writer is hand-rolled because the
// report is a flat, fixed schema and the project has no JSON dependency on the
// C++ side; a report is also written when the run fails, which is why the
// result carries its own status instead of relying on the exit code.

#include <cstdint>
#include <string>

#include "net/loadgen/latency.hpp"
#include "net/loadgen/runner.hpp"

namespace lob::loadgen {

struct Report {
  /// Endpoint the run connected to, as given on the command line.
  std::string gateway;
  std::uint32_t sessions = 0;
  std::uint64_t seed = 1;
  /// Target rate in requests per second; 0 when the run was unlimited.
  double rate_limit = 0.0;
  RunStats stats;
  LatencySummary latency;
  /// Samples above the histogram ceiling, clamped instead of dropped.
  std::uint64_t latency_clamped_samples = 0;
  /// Empty unless the runner stopped on an I/O or protocol failure.
  std::string error;
};

/// Renders the report as one JSON object followed by a newline.
std::string to_json(const Report& report);

}  // namespace lob::loadgen
