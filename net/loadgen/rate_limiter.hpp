#pragma once

// Rate pacing for the load generator. The limiter is pure arithmetic: request
// `index` (zero based, counted from the start of the run) is due at
// index / rate seconds. The runner polls the clock and waits for the due time,
// so a run that cannot keep up falls behind instead of sending bursts.

#include <chrono>
#include <cstdint>

namespace lob::loadgen {

class RateLimiter {
 public:
  /// `messages_per_second <= 0` means unlimited. Rates above zero are rounded
  /// through nanosecond due times, so very high rates pace in microseconds.
  explicit RateLimiter(double messages_per_second);

  bool unlimited() const { return !limited_; }

  /// Requests per second, 0 when unlimited.
  double rate() const { return rate_; }

  /// Time from the start of the run at which request `index` may be sent.
  std::chrono::nanoseconds due_after(std::uint64_t index) const;

 private:
  bool limited_ = false;
  double rate_ = 0.0;
};

}  // namespace lob::loadgen
