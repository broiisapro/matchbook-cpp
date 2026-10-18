#pragma once

#include <bit>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>

#include "lob/price_ladder.hpp"
#include "lob/types.hpp"

namespace lob {

/// One side of the book as a flat array indexed by `price - min_price`, with a
/// two-level occupancy bitmap to find the best and next levels.
///
/// The array covers every tick in `[min_price, max_price]`, so a level is
/// addressed in O(1) with no comparisons and no hashing. Occupancy is tracked
/// in 64-bit words (one bit per tick) plus a summary bitmap whose bit i is set
/// exactly when word i is non-zero; the next occupied level is found with
/// `std::countr_zero` (asks, scanning up) or `std::countl_zero` (bids,
/// scanning down), while the summary skips whole empty words. The best price
/// per side is tracked and recomputed from the bitmap when the best level
/// empties. Bits are set and cleared in the same code paths that create and
/// remove levels (find_or_insert and erase_if_empty), so no separate
/// synchronization exists to go stale.
///
/// Every allocation happens in the constructor; the hot path never allocates.
/// The supported range is at most 2^24 ticks per side (see kMaxRange):
/// constructing the ladder with a wider range throws std::invalid_argument.
/// With Level at 24 bytes the limit bounds the level array at ~384 MB per
/// side. Semantics match PriceLadder exactly: the same find/find_or_insert/
/// erase_if_empty/best/for_each_best_first behaviour, the same per-side
/// max_levels limit, and the same order of best-first iteration, so callers
/// and the event contract cannot tell the two ladders apart.
class ArrayPriceLadder {
 public:
  /// Widest supported price range, in ticks. Bounds the level array at
  /// 2^24 * sizeof(Level) bytes (~384 MB) per side.
  static constexpr std::uint64_t kMaxRange = std::uint64_t{1} << 24;

  ArrayPriceLadder(Side side, Price min_price, Price max_price, std::uint32_t max_levels)
      : side_(side),
        min_price_(min_price),
        max_price_(max_price),
        max_levels_(max_levels),
        range_(checked_range(min_price, max_price)),
        word_count_((range_ + 63) / 64),
        summary_count_((word_count_ + 63) / 64),
        levels_(std::make_unique<Level[]>(range_)),
        words_(std::make_unique<std::uint64_t[]>(word_count_)),
        summary_(std::make_unique<std::uint64_t[]>(summary_count_)) {}

  ArrayPriceLadder(const ArrayPriceLadder&) = delete;
  ArrayPriceLadder& operator=(const ArrayPriceLadder&) = delete;
  ArrayPriceLadder(ArrayPriceLadder&&) = delete;
  ArrayPriceLadder& operator=(ArrayPriceLadder&&) = delete;

  Side side() const noexcept { return side_; }

  bool empty() const noexcept { return level_count_ == 0; }

  /// Number of price levels currently present.
  std::uint32_t level_count() const noexcept { return level_count_; }

  std::uint32_t max_levels() const noexcept { return max_levels_; }

  /// Level at `price`, or nullptr when it is not present or outside the
  /// configured range.
  Level* find(Price price) {
    const std::size_t index = index_of(price);
    if (index >= range_ || !occupied(index)) {
      return nullptr;
    }
    return &levels_[index];
  }

  const Level* find(Price price) const {
    const std::size_t index = index_of(price);
    if (index >= range_ || !occupied(index)) {
      return nullptr;
    }
    return &levels_[index];
  }

  /// Level at `price`, inserted empty when absent. Returns nullptr when the
  /// price is outside the configured range or when the level does not exist
  /// and the ladder already holds max_levels levels.
  Level* find_or_insert(Price price) {
    const std::size_t index = index_of(price);
    if (index >= range_) {
      return nullptr;
    }
    if (occupied(index)) {
      return &levels_[index];
    }
    if (level_count_ >= max_levels_) {
      return nullptr;
    }
    levels_[index] = Level{};
    set_occupied(index, true);
    ++level_count_;
    if (!has_best_ || (side_ == Side::Buy ? index > best_index_ : index < best_index_)) {
      best_index_ = index;
      has_best_ = true;
    }
    return &levels_[index];
  }

  /// Removes the level at `price` when it holds no orders.
  void erase_if_empty(Price price) {
    const std::size_t index = index_of(price);
    if (index >= range_ || !occupied(index) || levels_[index].order_count != 0) {
      return;
    }
    set_occupied(index, false);
    --level_count_;
    if (has_best_ && best_index_ == index) {
      recompute_best();
    }
    levels_[index] = Level{};
  }

  /// Best price, or nullopt when this side is empty.
  std::optional<Price> best_price() const {
    if (!has_best_) {
      return std::nullopt;
    }
    return min_price_ + static_cast<Price>(best_index_);
  }

  /// Best level, or nullptr when this side is empty.
  const Level* best() const { return has_best_ ? &levels_[best_index_] : nullptr; }
  Level* best() { return has_best_ ? &levels_[best_index_] : nullptr; }

  /// Visits levels from best to worst. fn(price, level) may return false to
  /// stop the walk.
  template <class F>
  void for_each_best_first(F&& fn) const {
    std::size_t index = has_best_ ? best_index_ : range_;
    while (index != range_) {
      if (!fn(min_price_ + static_cast<Price>(index), levels_[index])) {
        return;
      }
      index = side_ == Side::Buy ? (index == 0 ? range_ : prev_occupied(index - 1))
                                 : next_occupied(index + 1);
    }
  }

 private:
  // The engine test suite's invariant checker inspects the bitmap directly.
  friend struct OrderBookTestAccess;

  /// Number of ticks in [min_price, max_price], validated against kMaxRange.
  static std::size_t checked_range(Price min_price, Price max_price) {
    if (max_price < min_price) {
      throw std::invalid_argument("ArrayPriceLadder: max_price is below min_price");
    }
    const std::uint64_t range =
        static_cast<std::uint64_t>(max_price) - static_cast<std::uint64_t>(min_price) + 1;
    if (range > kMaxRange) {
      throw std::invalid_argument("ArrayPriceLadder: price range exceeds 2^24 ticks");
    }
    return static_cast<std::size_t>(range);
  }

  /// Array index of `price`, or range_ when the price is out of bounds (the
  /// sentinel is past every valid index, so callers can compare once).
  std::size_t index_of(Price price) const noexcept {
    if (price < min_price_ || price > max_price_) {
      return range_;
    }
    return static_cast<std::size_t>(price - min_price_);
  }

  bool occupied(std::size_t index) const noexcept {
    assert(index < range_);
    return (words_[index >> 6] & (std::uint64_t{1} << (index & 63))) != 0;
  }

  void set_occupied(std::size_t index, bool value) noexcept {
    assert(index < range_);
    const std::size_t word = index >> 6;
    const std::uint64_t mask = std::uint64_t{1} << (index & 63);
    if (value) {
      words_[word] |= mask;
      summary_[word >> 6] |= std::uint64_t{1} << (word & 63);
      return;
    }
    words_[word] &= ~mask;
    if (words_[word] == 0) {
      summary_[word >> 6] &= ~(std::uint64_t{1} << (word & 63));
    }
  }

  /// Highest occupied index at or below `start`, or range_ when there is none.
  std::size_t prev_occupied(std::size_t start) const noexcept {
    if (start >= range_) {
      start = range_ - 1;
    }
    std::size_t word = start >> 6;
    std::uint64_t bits = words_[word] & (kAllOnes >> (63 - (start & 63)));
    if (bits != 0) {
      return (word << 6) + static_cast<std::size_t>(63 - std::countl_zero(bits));
    }
    word = prev_occupied_word(word);
    if (word == word_count_) {
      return range_;
    }
    return (word << 6) + static_cast<std::size_t>(63 - std::countl_zero(words_[word]));
  }

  /// Lowest occupied index at or above `start`, or range_ when there is none.
  std::size_t next_occupied(std::size_t start) const noexcept {
    if (start >= range_) {
      return range_;
    }
    std::size_t word = start >> 6;
    std::uint64_t bits = words_[word] & (kAllOnes << (start & 63));
    if (bits != 0) {
      return (word << 6) + static_cast<std::size_t>(std::countr_zero(bits));
    }
    word = next_occupied_word(word);
    if (word == word_count_) {
      return range_;
    }
    return (word << 6) + static_cast<std::size_t>(std::countr_zero(words_[word]));
  }

  /// Highest non-empty occupancy word strictly below `index`, or word_count_
  /// when there is none. The summary bitmap skips whole empty words.
  std::size_t prev_occupied_word(std::size_t index) const noexcept {
    if (index == 0) {
      return word_count_;
    }
    --index;
    std::size_t summary = index >> 6;
    std::uint64_t bits = summary_[summary] & (kAllOnes >> (63 - (index & 63)));
    while (bits == 0) {
      if (summary == 0) {
        return word_count_;
      }
      --summary;
      bits = summary_[summary];
    }
    return (summary << 6) + static_cast<std::size_t>(63 - std::countl_zero(bits));
  }

  /// Lowest non-empty occupancy word strictly above `index`, or word_count_
  /// when there is none. The summary bitmap skips whole empty words.
  std::size_t next_occupied_word(std::size_t index) const noexcept {
    ++index;
    if (index >= word_count_) {
      return word_count_;
    }
    std::size_t summary = index >> 6;
    std::uint64_t bits = summary_[summary] & (kAllOnes << (index & 63));
    while (bits == 0) {
      ++summary;
      if (summary >= summary_count_) {
        return word_count_;
      }
      bits = summary_[summary];
    }
    return (summary << 6) + static_cast<std::size_t>(std::countr_zero(bits));
  }

  /// Recomputes the tracked best from the bitmap after the best level emptied.
  void recompute_best() noexcept {
    const std::size_t found = side_ == Side::Buy
                                  ? (best_index_ == 0 ? range_ : prev_occupied(best_index_ - 1))
                                  : next_occupied(best_index_ + 1);
    best_index_ = found;
    has_best_ = found != range_;
  }

  static constexpr std::uint64_t kAllOnes = ~std::uint64_t{0};

  Side side_;
  Price min_price_;
  Price max_price_;
  std::uint32_t max_levels_;
  std::size_t range_;
  std::size_t word_count_;
  std::size_t summary_count_;
  std::unique_ptr<Level[]> levels_;
  std::unique_ptr<std::uint64_t[]> words_;
  std::unique_ptr<std::uint64_t[]> summary_;
  std::uint32_t level_count_ = 0;
  bool has_best_ = false;
  std::size_t best_index_ = 0;
};

}  // namespace lob
