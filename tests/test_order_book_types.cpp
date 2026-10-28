// FOK and PostOnly in the fast engine, per section 4.1.1 and 4.1.2 of
// docs/architecture.md (the interim UnsupportedOrderType reject of decision C47
// is gone). Expected event vectors are written by hand from the contract,
// never captured from the engine.
//
// Both price ladders run this file: the default preset builds the array ladder
// and the release-tree preset builds the tree ladder.

#include <gtest/gtest.h>

#include <cstdint>
#include <initializer_list>
#include <optional>
#include <vector>

#include "lob/config.hpp"
#include "lob/order_book.hpp"
#include "lob/types.hpp"
#include "support/recording_listener.hpp"

namespace {

using lob::Config;
using lob::OrderBook;
using lob::OrderType;
using lob::RejectReason;
using lob::Side;
using lob::test::Accept;
using lob::test::BookUpdate;
using lob::test::Cancel;
using lob::test::Event;
using lob::test::Modify;
using lob::test::Reject;
using lob::test::Trade;

constexpr Config kConfig{1, 1000, 64, 16};

class OrderBookTypesTest : public ::testing::Test {
 protected:
  void expect_events(std::initializer_list<Event> expected) {
    EXPECT_EQ(listener.events, std::vector<Event>(expected));
    listener.clear();
  }

  lob::test::RecordingListener listener;
  OrderBook book{kConfig, listener};
};

// ---------------------------------------------------------------- FOK fills

TEST_F(OrderBookTypesTest, FokFillsExactlyAgainstOneMaker) {
  book.submit(1, Side::Sell, OrderType::Limit, 100, 5);
  this->expect_events({Accept{1}, BookUpdate{Side::Sell, 100, 5}});

  book.submit(2, Side::Buy, OrderType::FOK, 100, 5);

  // Identical to an IOC that fills completely: accept, trade at the maker's
  // resting price, book update for the maker's level, no cancel.
  this->expect_events({Accept{2}, Trade{1, 2, 100, 5}, BookUpdate{Side::Sell, 100, 0}});
  EXPECT_EQ(book.order_count(), 0U);
  EXPECT_EQ(book.best_ask(), std::nullopt);
  EXPECT_EQ(book.level_qty(Side::Sell, 100), 0);
}

TEST_F(OrderBookTypesTest, FokTradesAtTheRestingPriceNotItsLimitPrice) {
  book.submit(1, Side::Sell, OrderType::Limit, 100, 3);
  book.submit(2, Side::Sell, OrderType::Limit, 101, 2);
  listener.clear();

  book.submit(3, Side::Buy, OrderType::FOK, 105, 5);

  this->expect_events({
      Accept{3},
      Trade{1, 3, 100, 3},
      BookUpdate{Side::Sell, 100, 0},
      Trade{2, 3, 101, 2},
      BookUpdate{Side::Sell, 101, 0},
  });
  EXPECT_EQ(book.order_count(), 0U);
}

TEST_F(OrderBookTypesTest, FokFillsAcrossThreeLevelsInPriceTimePriority) {
  book.submit(1, Side::Sell, OrderType::Limit, 100, 2);
  book.submit(2, Side::Sell, OrderType::Limit, 101, 3);
  book.submit(3, Side::Sell, OrderType::Limit, 102, 4);
  listener.clear();

  book.submit(4, Side::Buy, OrderType::FOK, 102, 9);

  this->expect_events({
      Accept{4},
      Trade{1, 4, 100, 2},
      BookUpdate{Side::Sell, 100, 0},
      Trade{2, 4, 101, 3},
      BookUpdate{Side::Sell, 101, 0},
      Trade{3, 4, 102, 4},
      BookUpdate{Side::Sell, 102, 0},
  });
  EXPECT_EQ(book.order_count(), 0U);
  EXPECT_EQ(book.best_ask(), std::nullopt);
}

TEST_F(OrderBookTypesTest, FokKeepsQueuePriorityWithinALevel) {
  book.submit(1, Side::Sell, OrderType::Limit, 100, 2);
  book.submit(2, Side::Sell, OrderType::Limit, 100, 3);
  listener.clear();

  book.submit(3, Side::Buy, OrderType::FOK, 100, 4);

  this->expect_events({
      Accept{3},
      Trade{1, 3, 100, 2},
      BookUpdate{Side::Sell, 100, 3},
      Trade{2, 3, 100, 2},
      BookUpdate{Side::Sell, 100, 1},
  });
  EXPECT_EQ(book.order_count(), 1U);
  EXPECT_EQ(book.level_qty(Side::Sell, 100), 1);
}

TEST_F(OrderBookTypesTest, FokSellFillsAgainstBids) {
  book.submit(1, Side::Buy, OrderType::Limit, 100, 4);
  book.submit(2, Side::Buy, OrderType::Limit, 99, 6);
  listener.clear();

  book.submit(3, Side::Sell, OrderType::FOK, 99, 10);

  this->expect_events({
      Accept{3},
      Trade{1, 3, 100, 4},
      BookUpdate{Side::Buy, 100, 0},
      Trade{2, 3, 99, 6},
      BookUpdate{Side::Buy, 99, 0},
  });
  EXPECT_EQ(book.order_count(), 0U);
}

// ----------------------------------------------------------------- FOK kills

TEST_F(OrderBookTypesTest, FokOneLotShortIsKilledWithoutTrading) {
  book.submit(1, Side::Sell, OrderType::Limit, 100, 5);
  listener.clear();
  const std::uint64_t digest_before = book.digest();

  book.submit(2, Side::Buy, OrderType::FOK, 100, 6);

  // on_accept first, then on_cancel with the full submitted quantity; no
  // trade and no book update, and the book is untouched.
  this->expect_events({Accept{2}, Cancel{2, 6}});
  EXPECT_EQ(book.digest(), digest_before);
  EXPECT_EQ(book.order_count(), 1U);
  EXPECT_EQ(book.level_qty(Side::Sell, 100), 5);
  EXPECT_EQ(book.best_ask(), 100);
}

TEST_F(OrderBookTypesTest, FokCountsOnlyTheLevelsItsLimitPriceCrosses) {
  book.submit(1, Side::Sell, OrderType::Limit, 100, 2);
  book.submit(2, Side::Sell, OrderType::Limit, 200, 10);
  listener.clear();
  const std::uint64_t digest_before = book.digest();

  // 12 lots rest on the ask side but only the 2 at 100 cross a limit of 150.
  book.submit(3, Side::Buy, OrderType::FOK, 150, 5);

  this->expect_events({Accept{3}, Cancel{3, 5}});
  EXPECT_EQ(book.digest(), digest_before);
  EXPECT_EQ(book.level_qty(Side::Sell, 100), 2);
  EXPECT_EQ(book.level_qty(Side::Sell, 200), 10);
}

TEST_F(OrderBookTypesTest, FokOnEmptyBookIsKilled) {
  const std::uint64_t empty_digest = book.digest();

  book.submit(1, Side::Buy, OrderType::FOK, 100, 5);

  this->expect_events({Accept{1}, Cancel{1, 5}});
  EXPECT_EQ(book.digest(), empty_digest);
  EXPECT_EQ(book.order_count(), 0U);
  EXPECT_EQ(book.best_bid(), std::nullopt);
  EXPECT_EQ(book.best_ask(), std::nullopt);
}

TEST_F(OrderBookTypesTest, FokSellOnEmptyBookIsKilled) {
  book.submit(1, Side::Sell, OrderType::FOK, 500, 3);

  this->expect_events({Accept{1}, Cancel{1, 3}});
  EXPECT_EQ(book.order_count(), 0U);
}

TEST_F(OrderBookTypesTest, FokNeverRestsEvenWhenOrderCapacityIsOne) {
  // The FOK may only fill completely, and a fill leaves nothing behind, so
  // the max_orders / max_levels path is never reached (section 4.1.1 step 5).
  const Config tight{1, 1000, 1, 1};
  OrderBook tight_book(tight, listener);

  tight_book.submit(1, Side::Sell, OrderType::Limit, 100, 5);
  this->expect_events({Accept{1}, BookUpdate{Side::Sell, 100, 5}});

  tight_book.submit(2, Side::Buy, OrderType::FOK, 100, 5);
  this->expect_events({Accept{2}, Trade{1, 2, 100, 5}, BookUpdate{Side::Sell, 100, 0}});
  EXPECT_EQ(tight_book.order_count(), 0U);

  // A killed FOK does not rest either, so a full book still kills it instead
  // of cancelling for capacity.
  tight_book.submit(3, Side::Sell, OrderType::Limit, 100, 5);
  listener.clear();
  tight_book.submit(4, Side::Buy, OrderType::FOK, 100, 6);
  this->expect_events({Accept{4}, Cancel{4, 6}});
  EXPECT_EQ(tight_book.order_count(), 1U);
}

// --------------------------------------------------------------- PostOnly

TEST_F(OrderBookTypesTest, PostOnlyRestsLikeALimitOrder) {
  book.submit(1, Side::Sell, OrderType::Limit, 200, 5);
  listener.clear();

  book.submit(2, Side::Buy, OrderType::PostOnly, 100, 7);

  this->expect_events({Accept{2}, BookUpdate{Side::Buy, 100, 7}});
  EXPECT_EQ(book.best_bid(), 100);
  EXPECT_EQ(book.best_ask(), 200);
  EXPECT_EQ(book.order_count(), 2U);
}

TEST_F(OrderBookTypesTest, PostOnlyRestsOnAnEmptyBook) {
  book.submit(1, Side::Sell, OrderType::PostOnly, 100, 5);

  this->expect_events({Accept{1}, BookUpdate{Side::Sell, 100, 5}});
  EXPECT_EQ(book.best_ask(), 100);
  EXPECT_EQ(book.order_count(), 1U);
}

TEST_F(OrderBookTypesTest, PostOnlyDoesNotCrossWhenTheBestPriceIsOutsideItsLimit) {
  book.submit(1, Side::Sell, OrderType::Limit, 101, 5);
  listener.clear();

  // The best ask is 101, so a buy at 100 does not cross and may rest.
  book.submit(2, Side::Buy, OrderType::PostOnly, 100, 5);

  this->expect_events({Accept{2}, BookUpdate{Side::Buy, 100, 5}});
  EXPECT_EQ(book.best_bid(), 100);
  EXPECT_EQ(book.best_ask(), 101);
}

TEST_F(OrderBookTypesTest, PostOnlyAtTheBestOppositePriceIsRejected) {
  book.submit(1, Side::Sell, OrderType::Limit, 100, 5);
  listener.clear();
  const std::uint64_t digest_before = book.digest();

  // Equal price crosses: the ask at 100 would have to trade with a buy at 100.
  book.submit(2, Side::Buy, OrderType::PostOnly, 100, 5);

  this->expect_events({Reject{2, RejectReason::WouldCross}});
  EXPECT_EQ(book.digest(), digest_before);
  EXPECT_EQ(book.order_count(), 1U);
  EXPECT_EQ(book.level_qty(Side::Sell, 100), 5);
  EXPECT_EQ(book.best_bid(), std::nullopt);
}

TEST_F(OrderBookTypesTest, PostOnlyCrossingThroughIsRejected) {
  book.submit(1, Side::Sell, OrderType::Limit, 100, 5);
  book.submit(2, Side::Buy, OrderType::Limit, 90, 4);
  listener.clear();
  const std::uint64_t digest_before = book.digest();

  // A buy through the whole ask side and a sell through the whole bid side.
  book.submit(3, Side::Buy, OrderType::PostOnly, 150, 5);
  this->expect_events({Reject{3, RejectReason::WouldCross}});
  book.submit(4, Side::Sell, OrderType::PostOnly, 80, 5);
  this->expect_events({Reject{4, RejectReason::WouldCross}});
  book.submit(5, Side::Sell, OrderType::PostOnly, 90, 5);
  this->expect_events({Reject{5, RejectReason::WouldCross}});

  EXPECT_EQ(book.digest(), digest_before);
  EXPECT_EQ(book.order_count(), 2U);
  EXPECT_EQ(book.level_qty(Side::Sell, 100), 5);
  EXPECT_EQ(book.level_qty(Side::Buy, 90), 4);
}

TEST_F(OrderBookTypesTest, PostOnlyRejectedIdIsNotLiveAndCanBeReused) {
  book.submit(1, Side::Sell, OrderType::Limit, 100, 5);
  listener.clear();

  book.submit(2, Side::Buy, OrderType::PostOnly, 100, 5);
  this->expect_events({Reject{2, RejectReason::WouldCross}});

  // The id never rested, so it is free for a resting order.
  book.submit(2, Side::Buy, OrderType::PostOnly, 99, 5);
  this->expect_events({Accept{2}, BookUpdate{Side::Buy, 99, 5}});
  EXPECT_EQ(book.order_count(), 2U);
}

TEST_F(OrderBookTypesTest, PostOnlyCancelsWhenTheOrderCapacityIsFull) {
  const Config tight{1, 1000, 1, 4};
  OrderBook tight_book(tight, listener);

  tight_book.submit(1, Side::Buy, OrderType::Limit, 100, 5);
  this->expect_events({Accept{1}, BookUpdate{Side::Buy, 100, 5}});

  // Accepted, then cancelled: the capacity rule of section 4.1 step 6 applies
  // to PostOnly exactly as it does to a limit order.
  tight_book.submit(2, Side::Sell, OrderType::PostOnly, 200, 7);
  this->expect_events({Accept{2}, Cancel{2, 7}});
  EXPECT_EQ(tight_book.order_count(), 1U);
  EXPECT_EQ(tight_book.best_ask(), std::nullopt);
}

TEST_F(OrderBookTypesTest, PostOnlyCancelsWhenTheLevelCapacityIsFull) {
  const Config tight{1, 1000, 8, 1};
  OrderBook tight_book(tight, listener);

  tight_book.submit(1, Side::Buy, OrderType::Limit, 100, 5);
  this->expect_events({Accept{1}, BookUpdate{Side::Buy, 100, 5}});

  // One level already exists on the bid side, so a new bid price cannot rest.
  tight_book.submit(2, Side::Buy, OrderType::PostOnly, 99, 7);
  this->expect_events({Accept{2}, Cancel{2, 7}});
  EXPECT_EQ(tight_book.order_count(), 1U);
}

// --------------------------------------------------------------- validation

TEST_F(OrderBookTypesTest, FokValidationOrderIsUnchanged) {
  book.submit(1, Side::Sell, OrderType::Limit, 100, 5);
  listener.clear();

  // Quantity is checked before price (step 1 before step 2), price before the
  // duplicate check (step 2 before step 3).
  book.submit(1, Side::Buy, OrderType::FOK, 0, 0);
  this->expect_events({Reject{1, RejectReason::InvalidQuantity}});
  book.submit(1, Side::Buy, OrderType::FOK, kConfig.max_price + 1, 5);
  this->expect_events({Reject{1, RejectReason::InvalidPrice}});
  book.submit(1, Side::Buy, OrderType::FOK, kConfig.min_price - 1, 5);
  this->expect_events({Reject{1, RejectReason::InvalidPrice}});

  // The live maker id is a duplicate even though the order would cross it.
  book.submit(1, Side::Buy, OrderType::FOK, 100, 5);
  this->expect_events({Reject{1, RejectReason::DuplicateId}});
  EXPECT_EQ(book.order_count(), 1U);
  EXPECT_EQ(book.level_qty(Side::Sell, 100), 5);
}

TEST_F(OrderBookTypesTest, PostOnlyValidationOrderIsUnchanged) {
  book.submit(1, Side::Sell, OrderType::Limit, 100, 5);
  listener.clear();

  book.submit(2, Side::Buy, OrderType::PostOnly, 150, 0);
  this->expect_events({Reject{2, RejectReason::InvalidQuantity}});
  book.submit(2, Side::Buy, OrderType::PostOnly, kConfig.max_price + 1, 5);
  this->expect_events({Reject{2, RejectReason::InvalidPrice}});

  // The duplicate check runs before the crossing check: id 1 is live and the
  // order would cross, but the reason is DuplicateId.
  book.submit(1, Side::Buy, OrderType::PostOnly, 150, 5);
  this->expect_events({Reject{1, RejectReason::DuplicateId}});

  // Only then does the crossing check reject, before on_accept.
  book.submit(3, Side::Buy, OrderType::PostOnly, 150, 5);
  this->expect_events({Reject{3, RejectReason::WouldCross}});
  EXPECT_EQ(book.order_count(), 1U);
}

// ------------------------------------------------------------------ modify

TEST_F(OrderBookTypesTest, ModifyPriceOfARestingPostOnlyBecomesALimitOrder) {
  book.submit(2, Side::Buy, OrderType::Limit, 100, 3);
  book.submit(1, Side::Sell, OrderType::PostOnly, 101, 5);
  listener.clear();

  // Section 4.3 step 6: cancel + new at the new price as a Limit order, so the
  // moved order may trade instead of resting.
  book.modify(1, 100, 5);

  this->expect_events({
      Cancel{1, 5},
      BookUpdate{Side::Sell, 101, 0},
      Accept{1},
      Trade{2, 1, 100, 3},
      BookUpdate{Side::Buy, 100, 0},
      BookUpdate{Side::Sell, 100, 2},
  });
  EXPECT_EQ(book.order_count(), 1U);
  EXPECT_EQ(book.best_ask(), 100);
  EXPECT_EQ(book.level_qty(Side::Sell, 100), 2);
}

TEST_F(OrderBookTypesTest, ModifyQuantityOfARestingPostOnlyKeepsQueuePosition) {
  book.submit(1, Side::Sell, OrderType::PostOnly, 101, 5);
  listener.clear();

  // Same price and a smaller quantity is an in-place reduce (section 4.3
  // step 5), independent of the order type.
  book.modify(1, 101, 3);

  this->expect_events({Modify{1, 101, 3}, BookUpdate{Side::Sell, 101, 3}});
  EXPECT_EQ(book.level_qty(Side::Sell, 101), 3);
}

}  // namespace
