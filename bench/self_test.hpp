#pragma once

namespace lob::bench {

/// Records busy-wait delays calibrated to ~200 ns and ~2 us and checks that the
/// reported p50 lands within +-25 % of each target. Each mode is calibrated
/// immediately before it is measured, from the minimum of nine long trials
/// (preemption only adds time), and the calibrated delay is verified against
/// the raw counter over long windows before and after recording; a changed
/// machine speed triggers a fresh calibration and a retry. A failure prints the
/// mode, target and calibration numbers. Prints the counter frequency, latency
/// mode and measured timer overhead. Returns 0 on success, 1 on failure.
int run_self_test();

}  // namespace lob::bench
