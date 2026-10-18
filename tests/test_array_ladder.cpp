#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <vector>

#include "lob/array_ladder.hpp"

namespace {

using lob::ArrayPriceLadder;
using lob::kNullIndex;
using lob::Level;
using lob::Price;
using lob::Qty;
using lob::Side;

/// Occupies a level with a single fictional order so that erase_if_empty will
/// remove it again. Fails the test when the ladder refuses the insert.
Level* insert_level(ArrayPriceLadder& ladder, Price price, Qty qty = 1) {
  Level* level = ladder.find_or_insert(price);
  EXPECT_NE(level, nullptr) << "find_or_insert refused price " << price;
  if (level != nullptr) {
    level->head = 0;
    level->tail = 0;
    level->total_qty = qty;
    level->order_count = 1;
  }
  return level;
}

/// Empties a level and lets the ladder drop it.
void remove_level(ArrayPriceLadder& ladder, Price price) {
  Level* level = ladder.find(price);
  ASSERT_NE(level, nullptr) << "no level at price " << price;
  level->head = kNullIndex;
  level->tail = kNullIndex;
  level->total_qty = 0;
  level->order_count = 0;
  ladder.erase_if_empty(price);
}

/// Prices of the levels visited by for_each_best_first.
std::vector<Price> best_first_prices(const ArrayPriceLadder& ladder) {
  std::vector<Price> prices;
  ladder.for_each_best_first([&prices](Price price, const Level&) {
    prices.push_back(price);
    return true;
  });
  return prices;
}

TEST(ArrayLadder, EmptyLadderHasNoBestOnEitherSide) {
  for (const Side side : {Side::Buy, Side::Sell}) {
    ArrayPriceLadder ladder(side, 1, 1000, 16);
    EXPECT_TRUE(ladder.empty());
    EXPECT_EQ(ladder.level_count(), 0U);
    EXPECT_EQ(ladder.best(), nullptr);
    EXPECT_EQ(ladder.best_price(), std::nullopt);
    EXPECT_EQ(ladder.find(1), nullptr);
    EXPECT_EQ(ladder.find(1000), nullptr);
    EXPECT_TRUE(best_first_prices(ladder).empty());
  }
}

TEST(ArrayLadder, BuyWordBoundaryIndicesRoundTrip) {
  // Indices 0, 63, 64, 65 cross the first occupancy word; 4095, 4096, 4097
  // cross the first summary word; 8192 is the last index of the range.
  const std::vector<Price> prices = {1, 64, 65, 66, 4096, 4097, 4098, 8193};
  ArrayPriceLadder ladder(Side::Buy, 1, 8193, 16);

  for (const Price price : prices) {
    ASSERT_NE(insert_level(ladder, price), nullptr);
  }
  EXPECT_EQ(ladder.level_count(), prices.size());
  EXPECT_FALSE(ladder.empty());
  EXPECT_EQ(ladder.best_price(), std::optional<Price>{8193});
  EXPECT_EQ(ladder.find(2), nullptr);
  EXPECT_EQ(ladder.find(63), nullptr);
  EXPECT_EQ(ladder.find(4095), nullptr);

  std::vector<Price> expected = prices;
  std::reverse(expected.begin(), expected.end());
  EXPECT_EQ(best_first_prices(ladder), expected);

  // Removing the best at the far end recomputes through the bitmap.
  remove_level(ladder, 8193);
  EXPECT_EQ(ladder.best_price(), std::optional<Price>{4098});
  remove_level(ladder, 66);
  remove_level(ladder, 4096);
  EXPECT_EQ(ladder.best_price(), std::optional<Price>{4098});
  remove_level(ladder, 4098);
  EXPECT_EQ(ladder.best_price(), std::optional<Price>{4097});
  remove_level(ladder, 4097);
  EXPECT_EQ(ladder.best_price(), std::optional<Price>{65});
  remove_level(ladder, 65);
  remove_level(ladder, 64);
  EXPECT_EQ(ladder.best_price(), std::optional<Price>{1});
  remove_level(ladder, 1);
  EXPECT_TRUE(ladder.empty());
  EXPECT_EQ(ladder.level_count(), 0U);
  EXPECT_EQ(ladder.best_price(), std::nullopt);
}

TEST(ArrayLadder, SellWordBoundaryIndicesRoundTrip) {
  const std::vector<Price> prices = {1, 64, 65, 66, 4096, 4097, 4098, 8193};
  ArrayPriceLadder ladder(Side::Sell, 1, 8193, 16);

  for (const Price price : prices) {
    ASSERT_NE(insert_level(ladder, price), nullptr);
  }
  EXPECT_EQ(ladder.level_count(), prices.size());
  EXPECT_EQ(ladder.best_price(), std::optional<Price>{1});
  EXPECT_EQ(best_first_prices(ladder), prices);

  remove_level(ladder, 1);
  EXPECT_EQ(ladder.best_price(), std::optional<Price>{64});
  remove_level(ladder, 4097);
  remove_level(ladder, 65);
  EXPECT_EQ(ladder.best_price(), std::optional<Price>{64});
  remove_level(ladder, 64);
  EXPECT_EQ(ladder.best_price(), std::optional<Price>{66});
  remove_level(ladder, 66);
  EXPECT_EQ(ladder.best_price(), std::optional<Price>{4096});
  remove_level(ladder, 4096);
  EXPECT_EQ(ladder.best_price(), std::optional<Price>{4098});
  remove_level(ladder, 4098);
  EXPECT_EQ(ladder.best_price(), std::optional<Price>{8193});
  remove_level(ladder, 8193);
  EXPECT_TRUE(ladder.empty());
  EXPECT_EQ(ladder.best_price(), std::nullopt);
}

TEST(ArrayLadder, BestRecomputedAcrossManyWordsForBothSides) {
  // Price 10 is in word 0; 70,000 is in the middle; 1 << 20 is the last tick.
  const Price low = 10;
  const Price middle = 70000;
  const Price high = 1 << 20;

  {
    ArrayPriceLadder ladder(Side::Buy, 1, high, 16);
    insert_level(ladder, low);
    insert_level(ladder, middle);
    insert_level(ladder, high);
    EXPECT_EQ(ladder.best_price(), std::optional<Price>{high});
    remove_level(ladder, high);
    EXPECT_EQ(ladder.best_price(), std::optional<Price>{middle});
    remove_level(ladder, middle);
    EXPECT_EQ(ladder.best_price(), std::optional<Price>{low});
    remove_level(ladder, low);
    EXPECT_EQ(ladder.best_price(), std::nullopt);
  }
  {
    ArrayPriceLadder ladder(Side::Sell, 1, high, 16);
    insert_level(ladder, low);
    insert_level(ladder, middle);
    insert_level(ladder, high);
    EXPECT_EQ(ladder.best_price(), std::optional<Price>{low});
    remove_level(ladder, low);
    EXPECT_EQ(ladder.best_price(), std::optional<Price>{middle});
    remove_level(ladder, middle);
    EXPECT_EQ(ladder.best_price(), std::optional<Price>{high});
    remove_level(ladder, high);
    EXPECT_EQ(ladder.best_price(), std::nullopt);
  }
}

TEST(ArrayLadder, BestTracksBothRangeEnds) {
  {
    ArrayPriceLadder ladder(Side::Buy, 1, 8193, 8);
    insert_level(ladder, 8193);
    insert_level(ladder, 1);
    EXPECT_EQ(ladder.best_price(), std::optional<Price>{8193});
    remove_level(ladder, 8193);
    EXPECT_EQ(ladder.best_price(), std::optional<Price>{1});
    remove_level(ladder, 1);
    EXPECT_EQ(ladder.best_price(), std::nullopt);
  }
  {
    ArrayPriceLadder ladder(Side::Sell, 1, 8193, 8);
    insert_level(ladder, 8193);
    insert_level(ladder, 1);
    EXPECT_EQ(ladder.best_price(), std::optional<Price>{1});
    remove_level(ladder, 1);
    EXPECT_EQ(ladder.best_price(), std::optional<Price>{8193});
    remove_level(ladder, 8193);
    EXPECT_EQ(ladder.best_price(), std::nullopt);
  }
}

TEST(ArrayLadder, InsertExistingLevelKeepsTheSameSlot) {
  ArrayPriceLadder ladder(Side::Buy, 1, 100, 4);
  Level* first = insert_level(ladder, 42, 7);
  ASSERT_NE(first, nullptr);
  Level* again = ladder.find_or_insert(42);
  EXPECT_EQ(again, first);
  EXPECT_EQ(ladder.level_count(), 1U);
}

TEST(ArrayLadder, MaxLevelsIsEnforcedBeforeTheBitIsSet) {
  ArrayPriceLadder ladder(Side::Buy, 1, 1000, 2);
  ASSERT_NE(insert_level(ladder, 100), nullptr);
  ASSERT_NE(insert_level(ladder, 200), nullptr);
  EXPECT_EQ(ladder.find_or_insert(300), nullptr);
  EXPECT_EQ(ladder.level_count(), 2U);
  EXPECT_EQ(ladder.find(300), nullptr);

  remove_level(ladder, 100);
  ASSERT_NE(insert_level(ladder, 300), nullptr);
  EXPECT_EQ(ladder.level_count(), 2U);
  EXPECT_NE(ladder.find(300), nullptr);
}

TEST(ArrayLadder, ReinsertedLevelStartsClean) {
  ArrayPriceLadder ladder(Side::Sell, 1, 256, 4);
  ASSERT_NE(insert_level(ladder, 128, 9), nullptr);
  remove_level(ladder, 128);
  EXPECT_EQ(ladder.find(128), nullptr);

  Level* again = ladder.find_or_insert(128);
  ASSERT_NE(again, nullptr);
  EXPECT_EQ(again->head, kNullIndex);
  EXPECT_EQ(again->tail, kNullIndex);
  EXPECT_EQ(again->total_qty, 0);
  EXPECT_EQ(again->order_count, 0U);
}

TEST(ArrayLadder, OutOfRangePricesAreNotAddressable) {
  ArrayPriceLadder ladder(Side::Buy, 10, 20, 4);
  EXPECT_EQ(ladder.find(9), nullptr);
  EXPECT_EQ(ladder.find(21), nullptr);
  EXPECT_EQ(ladder.find_or_insert(9), nullptr);
  EXPECT_EQ(ladder.find_or_insert(21), nullptr);
  EXPECT_EQ(ladder.level_count(), 0U);

  ASSERT_NE(insert_level(ladder, 10), nullptr);
  ASSERT_NE(insert_level(ladder, 20), nullptr);
  EXPECT_EQ(ladder.level_count(), 2U);
  EXPECT_EQ(ladder.best_price(), std::optional<Price>{20});
}

TEST(ArrayLadder, EraseIfEmptyKeepsOccupiedLevels) {
  ArrayPriceLadder ladder(Side::Buy, 1, 100, 4);
  insert_level(ladder, 50, 5);
  ladder.erase_if_empty(50);
  EXPECT_NE(ladder.find(50), nullptr);
  EXPECT_EQ(ladder.level_count(), 1U);
  EXPECT_EQ(ladder.find(50)->total_qty, 5);
}

TEST(ArrayLadder, ConstructionRejectsInvalidRanges) {
  static_assert(ArrayPriceLadder::kMaxRange == (std::uint64_t{1} << 24));
  EXPECT_THROW((ArrayPriceLadder{Side::Buy, 20, 10, 4}), std::invalid_argument);
  EXPECT_THROW(
      (ArrayPriceLadder{Side::Buy, 1, static_cast<Price>(ArrayPriceLadder::kMaxRange) + 1, 4}),
      std::invalid_argument);
  EXPECT_NO_THROW((ArrayPriceLadder{Side::Sell, 5, 5, 1}));
}

TEST(ArrayLadder, ForEachBestFirstCanStopEarly) {
  ArrayPriceLadder ladder(Side::Buy, 1, 1000, 8);
  insert_level(ladder, 10);
  insert_level(ladder, 20);
  insert_level(ladder, 30);

  std::vector<Price> visited;
  ladder.for_each_best_first([&visited](Price price, const Level&) {
    visited.push_back(price);
    return price > 20;
  });
  EXPECT_EQ(visited, (std::vector<Price>{30, 20}));
}

}  // namespace
