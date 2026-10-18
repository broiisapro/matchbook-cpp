#pragma once

#include <mach/mach_time.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
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

/// Tolerance band of the frequency cross-check in `timer_info()`: the counter
/// rate measured over `kCrossCheckWindowSeconds` must land within
/// `kCrossCheckTolerance` of the frequency the counter declares. The original
/// 2 ms window with a 0.1 % band flapped between "agrees" and "DISAGREES" run
/// to run on the shared M5 (preemption and read quantization eat into a short
/// window) while a genuinely different clock, like the 24 MHz timebase against
/// a 1 GHz counter, stays far outside 0.5 % either way.
inline constexpr double kCrossCheckTolerance = 0.005;
inline constexpr double kCrossCheckWindowSeconds = 0.05;

/// Everything the harness knows about its clock. `measured_ticks_per_second`
/// is the `cntvct_el0` rate measured against the mach clock over
/// `kCrossCheckWindowSeconds`; `cross_check_agrees` is true when it is within
/// `kCrossCheckTolerance` of `cntfrq_el0`, which proves the declared frequency
/// and the timebase describe the same clock.
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
  // Measure the counter rate against the mach clock over a long enough window
  // that preemption and loop-exit quantization cannot move the result out of
  // the tolerance band (decision C32 fix for issue #57).
  const Ticks begin = now_ticks();
  const Ticks mach_begin = mach_absolute_time();
  const Ticks target = static_cast<Ticks>(declared_hz * kCrossCheckWindowSeconds);
  Ticks end = begin;
  while (end - begin < target) {
    end = now_ticks();
  }
  const double elapsed_ns = mach_ticks_to_ns(mach_absolute_time() - mach_begin);
  const double measured_hz =
      elapsed_ns > 0 ? static_cast<double>(end - begin) / elapsed_ns * 1e9 : 0.0;
  const bool agrees =
      measured_hz > 0 && std::abs(measured_hz - declared_hz) <= kCrossCheckTolerance * declared_hz;
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
                   "timer: cntfrq_el0 declares %.0f Hz but the mach clock measures %.0f Hz over "
                   "%.0f ms; the two disagree by more than %.1f%%, measurements are suspect\n",
                   checked.ticks_per_second, checked.measured_ticks_per_second,
                   kCrossCheckWindowSeconds * 1e3, kCrossCheckTolerance * 100.0);
    }
    return checked;
  }();
  return info.ticks_per_second;
}

inline double ns_per_tick() { return 1e9 / ticks_per_second(); }

inline double ticks_to_ns(double ticks) { return ticks * ns_per_tick(); }

inline double ticks_to_ns(Ticks ticks) { return ticks_to_ns(static_cast<double>(ticks)); }

/// Distribution of the deltas between back-to-back counter reads, used to
/// decide whether per-operation timestamps carry information at all: `cntvct_el0`
/// on the M5 is scaled to nanoseconds but updates once per ~41.7 ns, so
/// sub-step operations all report as exact multiples of the update step
/// (decision C32).
struct GranularityMeasurement {
  static constexpr std::size_t kHistogramBuckets = 8;

  /// A non-zero delta must occur at least this often (as a fraction of the
  /// read intervals, and at least `kMinStepOccurrences` times) to be taken as
  /// the counter's step. Rare deltas of 17/18 ns sit next to the dominant
  /// 41/42 ns step on the M5 (about 0.01 % of reads each): they are update
  /// races, not the step, and the literal minimum alone would misreport the
  /// quantization that per-op measurements actually show.
  static constexpr double kMinStepFraction = 0.0005;
  static constexpr std::uint64_t kMinStepOccurrences = 8;

  /// Number of read pairs the measurement is based on.
  std::uint64_t intervals = 0;
  /// Literal smallest non-zero delta between consecutive reads, in counter
  /// ticks; reported for transparency but not used for decisions.
  Ticks smallest_nonzero = 0;
  /// The same value in nanoseconds.
  double smallest_nonzero_ns = 0.0;
  /// The counter's step: the smallest non-zero delta frequent enough to be a
  /// real update (see `kMinStepFraction`). This is the quantization a per-op
  /// measurement shows, in counter ticks and nanoseconds.
  Ticks step = 0;
  double step_ns = 0.0;
  /// Median delta over every read pair, zero deltas included.
  double median_ns = 0.0;
  /// Mean delta over every read pair, zero deltas included.
  double mean_ns = 0.0;
  /// Read pairs where the counter did not advance at all.
  std::uint64_t zero_deltas = 0;
  /// Counts deltas whose quotient by `step` is the bucket index; bucket 0
  /// holds the zero deltas and every delta below one step, and the last bucket
  /// absorbs every larger delta. Bucket i therefore covers
  /// `[i * step, (i + 1) * step)` with bucket 0 starting at zero.
  std::array<std::uint64_t, kHistogramBuckets> histogram{};
};

/// Measures the counter's update step by timing back-to-back reads over
/// `intervals` pairs: the smallest non-zero delta between consecutive reads
/// (kept verbatim), the smallest delta frequent enough to be the real step
/// (see `GranularityMeasurement::kMinStepFraction`), and the full delta
/// distribution for the log. One million reads take a few milliseconds and are
/// enough to see the step even when most pairs land inside the same one.
inline GranularityMeasurement measure_timer_granularity(std::uint64_t intervals = 1'000'000) {
  GranularityMeasurement result;
  result.intervals = intervals;
  if (intervals == 0) {
    return result;
  }

  std::vector<std::uint32_t> deltas(static_cast<std::size_t>(intervals));
  for (int i = 0; i < 64; ++i) {
    (void)now_ticks();  // warm up the counter read
  }
  Ticks previous = now_ticks();
  double sum = 0.0;
  for (std::uint64_t i = 0; i < intervals; ++i) {
    const Ticks current = now_ticks();
    const Ticks raw = current >= previous ? current - previous : 0;
    const Ticks delta = std::min<Ticks>(raw, 0xFFFFFFFFu);
    deltas[static_cast<std::size_t>(i)] = static_cast<std::uint32_t>(delta);
    sum += static_cast<double>(delta);
    previous = current;
  }

  for (const std::uint32_t delta : deltas) {
    if (delta != 0 && (result.smallest_nonzero == 0 || delta < result.smallest_nonzero)) {
      result.smallest_nonzero = delta;
    }
  }
  result.smallest_nonzero_ns = ticks_to_ns(result.smallest_nonzero);
  result.mean_ns = ticks_to_ns(sum / static_cast<double>(intervals));

  std::sort(deltas.begin(), deltas.end());
  result.median_ns = ticks_to_ns(static_cast<double>(deltas[deltas.size() / 2]));

  // The step is the smallest non-zero delta that occurs often enough to be a
  // real update rather than an update race; fall back to the literal minimum
  // when no delta clears the bar (a very noisy or very short measurement).
  const std::uint64_t min_occurrences =
      std::max<std::uint64_t>(GranularityMeasurement::kMinStepOccurrences,
                              static_cast<std::uint64_t>(static_cast<double>(intervals) *
                                                         GranularityMeasurement::kMinStepFraction));
  for (std::size_t i = 0; i < deltas.size();) {
    const std::uint32_t delta = deltas[i];
    std::size_t run = i;
    while (run < deltas.size() && deltas[run] == delta) {
      ++run;
    }
    if (delta != 0 && run - i >= min_occurrences) {
      result.step = delta;
      break;
    }
    i = run;
  }
  if (result.step == 0) {
    result.step = result.smallest_nonzero;
  }
  result.step_ns = ticks_to_ns(result.step);

  if (result.step != 0) {
    for (const std::uint32_t delta : deltas) {
      if (delta == 0) {
        ++result.zero_deltas;
      }
      std::uint64_t bucket = static_cast<std::uint64_t>(delta) / result.step;
      if (bucket >= GranularityMeasurement::kHistogramBuckets) {
        bucket = GranularityMeasurement::kHistogramBuckets - 1;
      }
      ++result.histogram[static_cast<std::size_t>(bucket)];
    }
  }
  return result;
}

/// The granularity measured once per process.
inline const GranularityMeasurement& timer_granularity() {
  static const GranularityMeasurement measurement = measure_timer_granularity();
  return measurement;
}

/// The counter's effective step in nanoseconds; per-op timing is only
/// meaningful when this is small relative to an operation (issue #57).
inline double measure_granularity_ns() { return timer_granularity().step_ns; }

/// One-line description of a measurement for logs: the step, the literal
/// minimum, the median and mean delta, and the delta distribution as
/// percentages over the buckets.
inline std::string describe_granularity(const GranularityMeasurement& measurement) {
  char buffer[192];
  std::snprintf(buffer, sizeof(buffer),
                "granularity %.1f ns (step; literal smallest non-zero delta %.1f ns; over %llu "
                "back-to-back reads; median %.1f ns, mean %.1f ns); delta distribution:",
                measurement.step_ns, measurement.smallest_nonzero_ns,
                static_cast<unsigned long long>(measurement.intervals), measurement.median_ns,
                measurement.mean_ns);
  std::string text(buffer);
  if (measurement.intervals == 0) {
    return text;
  }
  const double hundredths = static_cast<double>(measurement.intervals) * 0.01;
  for (std::size_t bucket = 0; bucket < GranularityMeasurement::kHistogramBuckets; ++bucket) {
    const double percent = static_cast<double>(measurement.histogram[bucket]) / hundredths;
    char label[16];
    if (bucket == 0) {
      std::snprintf(label, sizeof(label), "<1 step");
    } else if (bucket + 1 == GranularityMeasurement::kHistogramBuckets) {
      std::snprintf(label, sizeof(label), "%zu+ steps", bucket);
    } else {
      std::snprintf(label, sizeof(label), "%zu step%s", bucket, bucket == 1 ? "" : "s");
    }
    char cell[48];
    std::snprintf(cell, sizeof(cell), " %s %.1f%%", label, percent);
    text += cell;
  }
  return text;
}

/// Cost of reading the counter once, as the median interval between two
/// back-to-back `now_ticks()` calls. A timed region pays this once (for the
/// closing read), so measurements subtract it once per timed region: once per
/// operation in per-op mode, once per batch in batch mode.
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
