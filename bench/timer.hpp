#pragma once

#include <mach/mach_time.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace lob::bench {

using Ticks = std::uint64_t;

/// Reads the hardware tick counter used by every measurement in the harness.
///
/// On arm64 this is `cntvct_el0`, read after an `isb` so the read cannot be
/// reordered ahead of the code it times. Everywhere else it falls back to
/// `mach_absolute_time()`. Both counters run at a fixed frequency that does not
/// change with CPU frequency scaling, which is why they, and not the wall
/// clock, are the timing source.
inline Ticks now_ticks() {
#if defined(__aarch64__)
  Ticks value = 0;
  __asm__ __volatile__("isb\n\tmrs %0, cntvct_el0" : "=r"(value));
  return value;
#else
  return mach_absolute_time();
#endif
}

/// Frequency of the macOS timebase (`mach_absolute_time()` units per second).
inline double mach_ticks_per_second() {
  mach_timebase_info_data_t info{};
  mach_timebase_info(&info);
  if (info.numer == 0) {
    return 0.0;
  }
  return 1e9 * static_cast<double>(info.denom) / static_cast<double>(info.numer);
}

/// Converts mach ticks to nanoseconds using the timebase.
inline double mach_ticks_to_ns(Ticks ticks) {
  mach_timebase_info_data_t info{};
  mach_timebase_info(&info);
  if (info.numer == 0) {
    return 0.0;
  }
  return static_cast<double>(ticks) * static_cast<double>(info.numer) /
         static_cast<double>(info.denom);
}

/// Frequency reported by `cntfrq_el0` (the `cntvct_el0` rate) on arm64; the
/// timebase rate elsewhere. This is the counter `now_ticks()` actually reads.
/// Note that on some Apple Silicon generations this is 1 GHz while the mach
/// timebase counts at 24 MHz: the two counters tick at different rates but
/// measure the same time, so the cross-check below compares measured elapsed
/// time, not raw frequency.
inline double counter_ticks_per_second() {
#if defined(__aarch64__)
  std::uint64_t frequency = 0;
  __asm__ __volatile__("mrs %0, cntfrq_el0" : "=r"(frequency));
  return static_cast<double>(frequency);
#else
  return mach_ticks_per_second();
#endif
}

/// Everything the harness knows about its clock. `measured_ticks_per_second`
/// is the `cntvct_el0` rate measured against the mach clock over ~2 ms;
/// `cross_check_agrees` is true when it is within 0.1 % of `cntfrq_el0`, which
/// proves the declared frequency and the timebase describe the same clock.
struct TimerInfo {
  const char* counter_name;
  double ticks_per_second;
  double mach_ticks_per_second;
  double measured_ticks_per_second;
  bool cross_check_agrees;
};

inline TimerInfo timer_info() {
  const double declared_hz = counter_ticks_per_second();
  const double mach_hz = mach_ticks_per_second();
#if defined(__aarch64__)
  if (declared_hz <= 0) {
    return TimerInfo{"cntvct_el0", 0.0, mach_hz, 0.0, false};
  }
  // Measure the counter rate against the mach clock over about 2 ms.
  const Ticks begin = now_ticks();
  const Ticks mach_begin = mach_absolute_time();
  const Ticks target = static_cast<Ticks>(declared_hz * 0.002);
  Ticks end = begin;
  while (end - begin < target) {
    end = now_ticks();
  }
  const double elapsed_ns = mach_ticks_to_ns(mach_absolute_time() - mach_begin);
  const double measured_hz =
      elapsed_ns > 0 ? static_cast<double>(end - begin) / elapsed_ns * 1e9 : 0.0;
  const bool agrees = measured_hz > 0 && std::abs(measured_hz - declared_hz) <= 0.001 * declared_hz;
  return TimerInfo{"cntvct_el0", declared_hz, mach_hz, measured_hz, agrees};
#else
  return TimerInfo{"mach_absolute_time", mach_hz, mach_hz, mach_hz, true};
#endif
}

/// Frequency of `now_ticks()` in Hz, cross-checked once against the mach clock;
/// a disagreement is reported on stderr because it means measurements on this
/// machine cannot be trusted.
inline double ticks_per_second() {
  static const TimerInfo info = [] {
    const TimerInfo checked = timer_info();
    if (!checked.cross_check_agrees) {
      std::fprintf(stderr,
                   "timer: cntfrq_el0 declares %.0f Hz but the mach clock measures %.0f Hz; "
                   "the two disagree, measurements are suspect\n",
                   checked.ticks_per_second, checked.measured_ticks_per_second);
    }
    return checked;
  }();
  return info.ticks_per_second;
}

inline double ns_per_tick() { return 1e9 / ticks_per_second(); }

inline double ticks_to_ns(double ticks) { return ticks * ns_per_tick(); }

inline double ticks_to_ns(Ticks ticks) { return ticks_to_ns(static_cast<double>(ticks)); }

/// Cost of reading the counter once, as the median interval between two
/// back-to-back `now_ticks()` calls. A timed region pays this once (for the
/// closing read), so per-operation measurements subtract it.
struct TimerOverhead {
  Ticks ticks;  ///< median interval in counter ticks
  double ns;    ///< the same value in nanoseconds
};

inline TimerOverhead calibrate_overhead(std::size_t samples = 10001) {
  std::vector<Ticks> deltas;
  deltas.reserve(samples);
  for (std::size_t i = 0; i < 64; ++i) {  // warm up caches and the branch predictor
    const Ticks begin = now_ticks();
    const Ticks end = now_ticks();
    (void)begin;
    (void)end;
  }
  for (std::size_t i = 0; i < samples; ++i) {
    const Ticks begin = now_ticks();
    const Ticks end = now_ticks();
    deltas.push_back(end - begin);
  }
  std::sort(deltas.begin(), deltas.end());
  const Ticks median = deltas[deltas.size() / 2];
  return TimerOverhead{median, ticks_to_ns(median)};
}

}  // namespace lob::bench
