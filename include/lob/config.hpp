#pragma once

#include <cstdint>

#include "lob/types.hpp"

namespace lob {

/// Engine configuration. Both engines take the same Config and enforce the
/// same limits, so they stay comparable when limits bite.
struct Config {
  Price min_price;
  Price max_price;
  std::uint32_t max_orders;
  std::uint32_t max_levels;
};

/// True when the configuration is usable: a positive price floor that does not
/// exceed the ceiling and non-zero order and level capacities.
constexpr bool valid(const Config& config) {
  return config.min_price >= 1 && config.min_price <= config.max_price && config.max_orders >= 1 &&
         config.max_levels >= 1;
}

}  // namespace lob
