// FOK and PostOnly event vectors for the reference engine. Every expected
// vector below is written by hand from docs/architecture.md sections 4.1.1
// (fill or kill) and 4.1.2 (post only), never captured from an engine.

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <vector>

#include "lob/config.hpp"
#include "lob/digest.hpp"
#include "lob/order_book.hpp"
#include "lob/types.hpp"
#include "reference/reference_book.hpp"
#include "support/recording_listener.hpp"

namespace {

using lob::OrderType;
using lob::RejectReason;
using lob::Side;
using lob::test::Accept;
using lob::test::BookUpdate;
using lob::test::Cancel;
using lob::test::Event;
using lob::test::Reject;
using lob::test::Trade;

constexpr lob::Config kConfig{1, 1000, 1000, 100};

class ReferenceTypesTest : public ::testing::Test {
 protected:
  void expect_events(std::initializer_list<Event> expected) {
    EXPECT_EQ(listener.events, std::vector<Event>(expected));
    listener.clear();
  }

  lob::test::RecordingListener listener;
  lob::ref::ReferenceBook book{kConfig, listener};
};

std::uint64_t empty_book_digest() { return lob::DigestBuilder{}.finish(); }

// ---------------------------------------------------------------------------
// FOK
// ---------------------------------------------------------------------------

TEST_F(ReferenceTypesTest, FokExactFillBehavesLikeAFullyFilledIoc) {
  book.submit(1, Side::Sell, OrderType::Limit, 100, 5);
  listener.clear();

  book.submit(2, Side::Buy, OrderType::FOK, 100, 5);
  expect_events({Accept{2}, Trade{1, 2, 100, 5}, BookUpdate{Side::Sell, 100, 0}});

  EXPECT_EQ(book.best_ask(), std::nullopt);
  EXPECT_EQ(book.order_count(), 0U);
  EXPECT_EQ(book.digest(), empty_book_digest());
}

TEST_F(ReferenceTypesTest, FokOneLotShortIsKilledBeforeAnyTrade) {
  book.submit(1, Side::Sell, OrderType::Limit, 100, 9);
  listener.clear();
  const std::uint64_t digest_before = book.digest();

  book.submit(2, Side::Buy, OrderType::FOK, 100, 10);
  expect_events({Accept{2}, Cancel{2, 10}});

  // The maker is untouched: no trade, no book update, same digest.
  EXPECT_EQ(book.digest(), digest_before);
  EXPECT_EQ(book.level_qty(Side::Sell, 100), 9);
  EXPECT_EQ(book.best_ask(), 100);
  EXPECT_EQ(book.order_count(), 1U);

  // The killed order never rested, so its id is free again.
  book.submit(2, Side::Sell, OrderType::Limit, 150, 1);
  expect_events({Accept{2}, BookUpdate{Side::Sell, 150, 1}});
}

TEST_F(ReferenceTypesTest, FokAcrossThreeLevelsFillsInPriceThenTimeOrder) {
  book.submit(1, Side::Sell, OrderType::Limit, 102, 4);
  book.submit(2, Side::Sell, OrderType::Limit, 100, 2);
  book.submit(3, Side::Sell, OrderType::Limit, 101, 3);
  book.submit(4, Side::Sell, OrderType::Limit, 100, 1);
  listener.clear();

  book.submit(5, Side::Buy, OrderType::FOK, 102, 10);
  expect_events({Accept{5},
                 // Best ask first, oldest order first within the 100 level.
                 Trade{2, 5, 100, 2}, BookUpdate{Side::Sell, 100, 1}, Trade{4, 5, 100, 1},
                 BookUpdate{Side::Sell, 100, 0}, Trade{3, 5, 101, 3},
                 BookUpdate{Side::Sell, 101, 0}, Trade{1, 5, 102, 4},
                 BookUpdate{Side::Sell, 102, 0}});

  EXPECT_EQ(book.best_ask(), std::nullopt);
  EXPECT_EQ(book.order_count(), 0U);
}

TEST_F(ReferenceTypesTest, FokOnEmptyBookIsKilled) {
  book.submit(7, Side::Buy, OrderType::FOK, 100, 3);
  expect_events({Accept{7}, Cancel{7, 3}});

  EXPECT_EQ(book.order_count(), 0U);
  EXPECT_EQ(book.best_bid(), std::nullopt);
  EXPECT_EQ(book.digest(), empty_book_digest());
}

TEST_F(ReferenceTypesTest, FokSellSweepsBidsInDescendingPriceOrder) {
  book.submit(1, Side::Buy, OrderType::Limit, 100, 2);
  book.submit(2, Side::Buy, OrderType::Limit, 99, 3);
  book.submit(3, Side::Buy, OrderType::Limit, 100, 1);
  listener.clear();

  book.submit(4, Side::Sell, OrderType::FOK, 99, 6);
  expect_events({Accept{4}, Trade{1, 4, 100, 2}, BookUpdate{Side::Buy, 100, 1}, Trade{3, 4, 100, 1},
                 BookUpdate{Side::Buy, 100, 0}, Trade{2, 4, 99, 3}, BookUpdate{Side::Buy, 99, 0}});

  EXPECT_EQ(book.best_bid(), std::nullopt);
  EXPECT_EQ(book.order_count(), 0U);
}

TEST_F(ReferenceTypesTest, FokKillCountsOnlyQuantityAtCrossingPrices) {
  book.submit(1, Side::Sell, OrderType::Limit, 100, 2);
  book.submit(2, Side::Sell, OrderType::Limit, 102, 10);
  listener.clear();
  const std::uint64_t digest_before = book.digest();

  // Ten lots rest on the ask side, but only two are at or below 101.
  book.submit(3, Side::Buy, OrderType::FOK, 101, 5);
  expect_events({Accept{3}, Cancel{3, 5}});
  EXPECT_EQ(book.digest(), digest_before);

  // At 102 the same order crosses both levels and fills.
  book.submit(4, Side::Buy, OrderType::FOK, 102, 5);
  expect_events({Accept{4}, Trade{1, 4, 100, 2}, BookUpdate{Side::Sell, 100, 0},
                 Trade{2, 4, 102, 3}, BookUpdate{Side::Sell, 102, 7}});

  EXPECT_EQ(book.level_qty(Side::Sell, 102), 7);
  EXPECT_EQ(book.order_count(), 1U);
}

TEST_F(ReferenceTypesTest, FokPartiallyFillsTheLastMakerAndLeavesItResting) {
  book.submit(1, Side::Sell, OrderType::Limit, 100, 10);
  listener.clear();

  book.submit(2, Side::Buy, OrderType::FOK, 100, 4);
  expect_events({Accept{2}, Trade{1, 2, 100, 4}, BookUpdate{Side::Sell, 100, 6}});

  EXPECT_EQ(book.best_ask(), 100);
  EXPECT_EQ(book.level_qty(Side::Sell, 100), 6);
  EXPECT_EQ(book.order_count(), 1U);
}

TEST(ReferenceTypesCapacity, FokNeverHitsTheRestingCapacityLimits) {
  constexpr lob::Config config{1, 1000, /*max_orders=*/1, /*max_levels=*/1};
  lob::test::RecordingListener listener;
  lob::ref::ReferenceBook book(config, listener);

  book.submit(1, Side::Buy, OrderType::Limit, 90, 5);
  listener.clear();

  // The book is at both capacity limits, yet a FOK that fully fills is not a
  // resting order, so capacity is irrelevant to it.
  book.submit(2, Side::Sell, OrderType::FOK, 90, 5);
  EXPECT_EQ(listener.events,
            (std::vector<Event>{Accept{2}, Trade{1, 2, 90, 5}, BookUpdate{Side::Buy, 90, 0}}));
  EXPECT_EQ(book.order_count(), 0U);
  listener.clear();

  book.submit(3, Side::Buy, OrderType::FOK, 90, 1);
  EXPECT_EQ(listener.events, (std::vector<Event>{Accept{3}, Cancel{3, 1}}));
  EXPECT_EQ(book.order_count(), 0U);
}

// ---------------------------------------------------------------------------
// PostOnly
// ---------------------------------------------------------------------------

TEST_F(ReferenceTypesTest, PostOnlyRestsLikeALimitOrderAndCanBeHitLater) {
  book.submit(1, Side::Buy, OrderType::PostOnly, 100, 10);
  expect_events({Accept{1}, BookUpdate{Side::Buy, 100, 10}});

  book.submit(2, Side::Buy, OrderType::PostOnly, 100, 5);
  expect_events({Accept{2}, BookUpdate{Side::Buy, 100, 15}});

  book.submit(3, Side::Sell, OrderType::PostOnly, 101, 7);
  expect_events({Accept{3}, BookUpdate{Side::Sell, 101, 7}});

  EXPECT_EQ(book.best_bid(), 100);
  EXPECT_EQ(book.best_ask(), 101);

  lob::DigestBuilder expected;
  expected.level(Side::Buy, 100, 2);
  expected.order(1, 10);
  expected.order(2, 5);
  expected.level(Side::Sell, 101, 1);
  expected.order(3, 7);
  EXPECT_EQ(book.digest(), expected.finish());

  // A resting post-only order trades against later incoming orders in FIFO
  // order like any other resting order.
  book.submit(4, Side::Sell, OrderType::Limit, 100, 12);
  expect_events({Accept{4}, Trade{1, 4, 100, 10}, BookUpdate{Side::Buy, 100, 5},
                 Trade{2, 4, 100, 2}, BookUpdate{Side::Buy, 100, 3}});
  EXPECT_EQ(book.level_qty(Side::Buy, 100), 3);
}

TEST_F(ReferenceTypesTest, PostOnlyAtTheOppositeBestPriceIsRejectedWithWouldCross) {
  book.submit(1, Side::Sell, OrderType::Limit, 100, 5);
  listener.clear();
  const std::uint64_t digest_before = book.digest();

  // Equal to the best ask is already a crossing price for a buy.
  book.submit(2, Side::Buy, OrderType::PostOnly, 100, 1);
  expect_events({Reject{2, RejectReason::WouldCross}});

  EXPECT_EQ(book.digest(), digest_before);
  EXPECT_EQ(book.best_ask(), 100);
  EXPECT_EQ(book.level_qty(Side::Sell, 100), 5);
  EXPECT_EQ(book.order_count(), 1U);

  // The rejected id is not resting: reusing it does not raise DuplicateId.
  book.submit(2, Side::Buy, OrderType::Limit, 99, 1);
  expect_events({Accept{2}, BookUpdate{Side::Buy, 99, 1}});
}

TEST_F(ReferenceTypesTest, PostOnlyCrossingThroughTheBookIsRejectedOnBothSides) {
  book.submit(1, Side::Sell, OrderType::Limit, 100, 5);
  book.submit(2, Side::Buy, OrderType::Limit, 90, 5);
  listener.clear();

  book.submit(3, Side::Buy, OrderType::PostOnly, 105, 1);
  expect_events({Reject{3, RejectReason::WouldCross}});

  book.submit(4, Side::Sell, OrderType::PostOnly, 85, 1);
  expect_events({Reject{4, RejectReason::WouldCross}});

  // One tick outside the touch on each side does not cross and rests.
  book.submit(5, Side::Buy, OrderType::PostOnly, 99, 1);
  expect_events({Accept{5}, BookUpdate{Side::Buy, 99, 1}});

  book.submit(6, Side::Sell, OrderType::PostOnly, 101, 1);
  expect_events({Accept{6}, BookUpdate{Side::Sell, 101, 1}});

  EXPECT_EQ(book.order_count(), 4U);
  EXPECT_EQ(book.best_bid(), 99);
  EXPECT_EQ(book.best_ask(), 100);
}

TEST_F(ReferenceTypesTest, PostOnlyOnAnEmptySideRests) {
  book.submit(1, Side::Buy, OrderType::PostOnly, 100, 4);
  expect_events({Accept{1}, BookUpdate{Side::Buy, 100, 4}});

  EXPECT_EQ(book.best_bid(), 100);
  EXPECT_EQ(book.order_count(), 1U);
}

TEST(ReferenceTypesCapacity, PostOnlyCapacityRulesMatchLimitOrders) {
  constexpr lob::Config order_config{1, 1000, /*max_orders=*/1, /*max_levels=*/100};
  lob::test::RecordingListener order_listener;
  lob::ref::ReferenceBook order_book(order_config, order_listener);

  order_book.submit(1, Side::Buy, OrderType::Limit, 100, 5);
  order_listener.clear();

  order_book.submit(2, Side::Buy, OrderType::PostOnly, 99, 5);
  EXPECT_EQ(order_listener.events, (std::vector<Event>{Accept{2}, Cancel{2, 5}}));
  EXPECT_EQ(order_book.order_count(), 1U);

  constexpr lob::Config level_config{1, 1000, /*max_orders=*/100, /*max_levels=*/1};
  lob::test::RecordingListener level_listener;
  lob::ref::ReferenceBook level_book(level_config, level_listener);

  level_book.submit(1, Side::Buy, OrderType::Limit, 90, 1);
  level_listener.clear();

  level_book.submit(2, Side::Buy, OrderType::PostOnly, 89, 1);
  EXPECT_EQ(level_listener.events, (std::vector<Event>{Accept{2}, Cancel{2, 1}}));
  level_listener.clear();

  // An existing level is always allowed.
  level_book.submit(3, Side::Buy, OrderType::PostOnly, 90, 2);
  EXPECT_EQ(level_listener.events, (std::vector<Event>{Accept{3}, BookUpdate{Side::Buy, 90, 3}}));
  EXPECT_EQ(level_book.order_count(), 2U);
}

// ---------------------------------------------------------------------------
// Validation precedence
// ---------------------------------------------------------------------------

TEST_F(ReferenceTypesTest, FokAndPostOnlyFollowTheOrdinaryValidationOrder) {
  // Quantity is checked before price, and the type is no longer short-circuited.
  book.submit(1, Side::Buy, OrderType::FOK, 0, 0);
  expect_events({Reject{1, RejectReason::InvalidQuantity}});

  book.submit(1, Side::Buy, OrderType::PostOnly, 0, -3);
  expect_events({Reject{1, RejectReason::InvalidQuantity}});

  book.submit(1, Side::Buy, OrderType::FOK, 0, 5);
  expect_events({Reject{1, RejectReason::InvalidPrice}});

  book.submit(1, Side::Buy, OrderType::PostOnly, 1001, 5);
  expect_events({Reject{1, RejectReason::InvalidPrice}});

  book.submit(1, Side::Buy, OrderType::Limit, 90, 5);
  listener.clear();

  // The price bounds are checked before the duplicate id.
  book.submit(1, Side::Buy, OrderType::FOK, 5000, 5);
  expect_events({Reject{1, RejectReason::InvalidPrice}});

  book.submit(1, Side::Buy, OrderType::PostOnly, 90, 5);
  expect_events({Reject{1, RejectReason::DuplicateId}});

  book.submit(2, Side::Sell, OrderType::Limit, 100, 5);
  listener.clear();

  // Quantity and duplicate id both precede the post-only crossing check.
  book.submit(1, Side::Buy, OrderType::PostOnly, 100, 0);
  expect_events({Reject{1, RejectReason::InvalidQuantity}});

  book.submit(2, Side::Buy, OrderType::PostOnly, 100, 1);
  expect_events({Reject{2, RejectReason::DuplicateId}});

  book.submit(3, Side::Buy, OrderType::PostOnly, 100, 1);
  expect_events({Reject{3, RejectReason::WouldCross}});

  // The same id is free after the reject and a FOK fills exactly.
  book.submit(3, Side::Buy, OrderType::FOK, 100, 5);
  expect_events({Accept{3}, Trade{2, 3, 100, 5}, BookUpdate{Side::Sell, 100, 0}});

  EXPECT_EQ(book.order_count(), 1U);
  EXPECT_EQ(book.best_bid(), 90);
}

// ---------------------------------------------------------------------------
// Interaction with modify (section 4.3)
// ---------------------------------------------------------------------------

TEST_F(ReferenceTypesTest, ModifyOfARestingPostOnlyOrderUsesLimitOrderRules) {
  book.submit(1, Side::Buy, OrderType::PostOnly, 100, 10);
  listener.clear();

  // Reduce in place keeps priority.
  book.modify(1, 100, 4);
  expect_events({lob::test::Modify{1, 100, 4}, BookUpdate{Side::Buy, 100, 4}});

  // A price change is cancel + new, and section 4.3 re-submits as a Limit
  // order; the result is an ordinary resting order.
  book.modify(1, 101, 4);
  expect_events(
      {Cancel{1, 4}, BookUpdate{Side::Buy, 100, 0}, Accept{1}, BookUpdate{Side::Buy, 101, 4}});

  book.submit(2, Side::Sell, OrderType::Limit, 100, 4);
  expect_events({Accept{2}, Trade{1, 2, 101, 4}, BookUpdate{Side::Buy, 101, 0}});

  EXPECT_EQ(book.order_count(), 0U);
}

}  // namespace
