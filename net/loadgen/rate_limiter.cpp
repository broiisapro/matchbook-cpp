#include "net/loadgen/rate_limiter.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace lob::loadgen {

RateLimiter::RateLimiter(double messages_per_second)
    : limited_(messages_per_second > 0.0),
      rate_(messages_per_second > 0.0 ? messages_per_second : 0.0) {}

std::chrono::nanoseconds RateLimiter::due_after(std::uint64_t index) const {
  if (!limited_) {
    return std::chrono::nanoseconds::zero();
  }
  const long double due_ns =
      static_cast<long double>(index) * 1.0e9L / static_cast<long double>(rate_);
  const long double capped =
      std::min(due_ns, static_cast<long double>(std::numeric_limits<std::int64_t>::max()));
  return std::chrono::nanoseconds(static_cast<std::int64_t>(capped));
}

}  // namespace lob::loadgen
