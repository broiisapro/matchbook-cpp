#pragma once

namespace lob::bench {

/// Records busy-wait delays calibrated to ~200 ns and ~2 us and checks that the
/// reported p50 lands within +-25 % of each target. Prints the counter
/// frequency, latency mode and measured timer overhead. Returns 0 on success,
/// 1 on failure.
int run_self_test();

}  // namespace lob::bench
