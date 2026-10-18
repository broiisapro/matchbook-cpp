#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>

#include "lob/pool.hpp"
#include "lob/types.hpp"

namespace lob {

/// One price level: the resting queue's head and tail (pool indices, linked by
/// the order nodes themselves) plus the level aggregates.
struct Level {
  std::uint32_t head = kNullIndex;
  std::uint32_t tail = kNullIndex;
  Qty total_qty = 0;
  std::uint32_t order_count = 0;
};

/// Orders prices so that the best level is the first level of the map: bids
/// descend from best to worst, asks ascend. The side is fixed at construction.
struct PriceOrder {
  Side side = Side::Buy;

  bool operator()(Price lhs, Price rhs) const noexcept {
    return side == Side::Buy ? lhs > rhs : lhs < rhs;
  }
};

/// One side of the book: a map of price levels ordered best first, backed by a
/// fixed-capacity node allocator, plus the per-side level limit.
///
/// find() and find_or_insert() are the operations the matching code uses;
/// iteration is always best -> worst. The class hides the map so a future
/// implementation (an array-indexed price ladder) can replace it behind the
/// same interface without touching callers.
class PriceLadder {
 public:
  using Map = std::map<Price, Level, PriceOrder, PoolAllocator<std::pair<const Price, Level>>>;

  PriceLadder(Side side, std::uint32_t max_levels)
      : side_(side),
        max_levels_(max_levels),
        pool_(detail::make_node_pool_for_map<Map>(max_levels)),
        levels_(PriceOrder{side}, Map::allocator_type{pool_.get()}) {}

  /// Constructor that also takes the price range, mirroring ArrayPriceLadder's
  /// signature so the engine can initialize whichever ladder it was compiled
  /// against with one expression. The tree ladder does not need the bounds.
  PriceLadder(Side side, [[maybe_unused]] Price min_price, [[maybe_unused]] Price max_price,
              std::uint32_t max_levels)
      : PriceLadder(side, max_levels) {}

  PriceLadder(const PriceLadder&) = delete;
  PriceLadder& operator=(const PriceLadder&) = delete;
  PriceLadder(PriceLadder&&) = delete;
  PriceLadder& operator=(PriceLadder&&) = delete;

  Side side() const noexcept { return side_; }

  bool empty() const noexcept { return levels_.empty(); }

  /// Number of price levels currently present.
  std::uint32_t level_count() const noexcept { return static_cast<std::uint32_t>(levels_.size()); }

  std::uint32_t max_levels() const noexcept { return max_levels_; }

  /// Level at `price`, or nullptr when it is not present.
  Level* find(Price price) {
    const auto it = levels_.find(price);
    return it == levels_.end() ? nullptr : &it->second;
  }

  const Level* find(Price price) const {
    const auto it = levels_.find(price);
    return it == levels_.end() ? nullptr : &it->second;
  }

  /// Level at `price`, inserted empty when absent. Returns nullptr when the
  /// level does not exist and the ladder already holds max_levels levels.
  Level* find_or_insert(Price price) {
    const auto it = levels_.find(price);
    if (it != levels_.end()) {
      return &it->second;
    }
    if (levels_.size() >= max_levels_) {
      return nullptr;
    }
    return &levels_.try_emplace(price).first->second;
  }

  /// Removes the level at `price` when it holds no orders.
  void erase_if_empty(Price price) {
    const auto it = levels_.find(price);
    if (it != levels_.end() && it->second.order_count == 0) {
      levels_.erase(it);
    }
  }

  /// Best price, or nullopt when this side is empty.
  std::optional<Price> best_price() const {
    if (levels_.empty()) {
      return std::nullopt;
    }
    return levels_.begin()->first;
  }

  /// Best level, or nullptr when this side is empty.
  const Level* best() const { return levels_.empty() ? nullptr : &levels_.begin()->second; }
  Level* best() { return levels_.empty() ? nullptr : &levels_.begin()->second; }

  /// Visits levels from best to worst. fn(price, level) may return false to
  /// stop the walk.
  template <class F>
  void for_each_best_first(F&& fn) const {
    for (const auto& [price, level] : levels_) {
      if (!fn(price, level)) {
        return;
      }
    }
  }

 private:
  Side side_;
  std::uint32_t max_levels_;
  std::unique_ptr<detail::NodePool> pool_;
  Map levels_;
};

}  // namespace lob
