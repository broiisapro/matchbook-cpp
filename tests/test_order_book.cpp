#include <gtest/gtest.h>

#include <cstdint>
#include <initializer_list>
#include <optional>
#include <vector>

#include "lob/digest.hpp"
#include "lob/order_book.hpp"
#include "support/recording_listener.hpp"

namespace {

using lob::Config;
using lob::DigestBuilder;
using lob::OrderBook;
using lob::OrderType;
using lob::Price;
using lob::Qty;
using lob::RejectReason;
using lob::Side;
using lob::test::Accept;
using lob::test::BookUpdate;
using lob::test::Cancel;
using lob::test::Event;
using lob::test::Modify;
using lob::test::RecordingListener;
using lob::test::Reject;
using lob::test::Trade;

Config make_config(std::uint32_t max_orders = 4096, std::uint32_t max_levels = 4096) {
  return Config{1, 1000000, max_orders, max_levels};
}

// Classic bug tests. These live here, under these names, as the canonical
// regression suite.

TEST(OrderBook, TradeExecutesAtRestingPrice) {
  RecordingListener listener;
  OrderBook book(make_config(), listener);
  book.submit(1, Side::Sell, OrderType::Limit, 101, 5);
  listener.clear();

  book.submit(2, Side::Buy, OrderType::Limit, 103, 5);

  EXPECT_EQ(listener.events, (std::vector<Event>{
                                 Accept{2},
                                 Trade{1, 2, 101, 5},
                                 BookUpdate{Side::Sell, 101, 0},
                             }));
  EXPECT_EQ(book.best_bid(), std::nullopt);
  EXPECT_EQ(book.best_ask(), std::nullopt);
  EXPECT_EQ(book.order_count(), 0U);
}

TEST(OrderBook, PartialFillKeepsQueuePosition) {
  RecordingListener listener;
  OrderBook book(make_config(), listener);
  book.submit(1, Side::Sell, OrderType::Limit, 101, 5);
  book.submit(2, Side::Sell, OrderType::Limit, 101, 5);
  listener.clear();

  book.submit(3, Side::Buy, OrderType::Limit, 101, 2);

  EXPECT_EQ(listener.events, (std::vector<Event>{
                                 Accept{3},
                                 Trade{1, 3, 101, 2},
                                 BookUpdate{Side::Sell, 101, 8},
                             }));

  listener.clear();
  book.submit(4, Side::Buy, OrderType::Limit, 101, 1);

  EXPECT_EQ(listener.events, (std::vector<Event>{
                                 Accept{4},
                                 Trade{1, 4, 101, 1},
                                 BookUpdate{Side::Sell, 101, 7},
                             }));
  EXPECT_EQ(book.level_qty(Side::Sell, 101), 7);
  EXPECT_EQ(book.order_count(), 2U);
}

TEST(OrderBook, MarketOrderAgainstEmptyBookIsCancelledNotRested) {
  RecordingListener listener;
  OrderBook book(make_config(), listener);

  book.submit(1, Side::Buy, OrderType::Market, 0, 5);

  EXPECT_EQ(listener.events, (std::vector<Event>{Accept{1}, Cancel{1, 5}}));
  EXPECT_EQ(book.best_bid(), std::nullopt);
  EXPECT_EQ(book.best_ask(), std::nullopt);
  EXPECT_EQ(book.order_count(), 0U);
}

TEST(OrderBook, CancelUnknownIdIsRejectedNotCrash) {
  RecordingListener listener;
  OrderBook book(make_config(), listener);

  book.cancel(99);

  EXPECT_EQ(listener.events, (std::vector<Event>{Reject{99, RejectReason::UnknownOrder}}));
  EXPECT_EQ(book.order_count(), 0U);
  EXPECT_EQ(book.best_bid(), std::nullopt);
}

TEST(OrderBook, DuplicateIdRejected) {
  RecordingListener listener;
  OrderBook book(make_config(), listener);
  book.submit(1, Side::Buy, OrderType::Limit, 100, 5);
  listener.clear();

  book.submit(1, Side::Sell, OrderType::Limit, 200, 9);

  EXPECT_EQ(listener.events, (std::vector<Event>{Reject{1, RejectReason::DuplicateId}}));
  EXPECT_EQ(book.order_count(), 1U);
  EXPECT_EQ(book.best_bid(), std::optional<Price>{100});
  EXPECT_EQ(book.best_ask(), std::nullopt);
}

// Modify.

TEST(OrderBook, ModifyReduceKeepsPriority) {
  RecordingListener listener;
  OrderBook book(make_config(), listener);
  book.submit(1, Side::Sell, OrderType::Limit, 100, 10);
  book.submit(2, Side::Sell, OrderType::Limit, 100, 5);
  listener.clear();

  book.modify(1, 100, 4);

  EXPECT_EQ(listener.events, (std::vector<Event>{
                                 Modify{1, 100, 4},
                                 BookUpdate{Side::Sell, 100, 9},
                             }));
  EXPECT_EQ(book.level_qty(Side::Sell, 100), 9);

  listener.clear();
  book.submit(3, Side::Buy, OrderType::Limit, 100, 4);

  EXPECT_EQ(listener.events, (std::vector<Event>{
                                 Accept{3},
                                 Trade{1, 3, 100, 4},
                                 BookUpdate{Side::Sell, 100, 5},
                             }));
  EXPECT_EQ(book.order_count(), 1U);
}

TEST(OrderBook, ModifyIncreaseLosesPriority) {
  RecordingListener listener;
  OrderBook book(make_config(), listener);
  book.submit(1, Side::Sell, OrderType::Limit, 100, 5);
  book.submit(2, Side::Sell, OrderType::Limit, 100, 5);
  listener.clear();

  book.modify(1, 100, 7);

  EXPECT_EQ(listener.events, (std::vector<Event>{
                                 Cancel{1, 5},
                                 BookUpdate{Side::Sell, 100, 5},
                                 Accept{1},
                                 BookUpdate{Side::Sell, 100, 12},
                             }));

  listener.clear();
  book.submit(3, Side::Buy, OrderType::Limit, 100, 5);

  EXPECT_EQ(listener.events, (std::vector<Event>{
                                 Accept{3},
                                 Trade{2, 3, 100, 5},
                                 BookUpdate{Side::Sell, 100, 7},
                             }));
}

TEST(OrderBook, ModifySamePriceLargerQtyGoesToTheBack) {
  RecordingListener listener;
  OrderBook book(make_config(), listener);
  book.submit(1, Side::Buy, OrderType::Limit, 100, 5);
  book.submit(2, Side::Buy, OrderType::Limit, 100, 5);
  listener.clear();

  book.modify(1, 100, 6);

  EXPECT_EQ(listener.events, (std::vector<Event>{
                                 Cancel{1, 5},
                                 BookUpdate{Side::Buy, 100, 5},
                                 Accept{1},
                                 BookUpdate{Side::Buy, 100, 11},
                             }));

  listener.clear();
  book.submit(3, Side::Sell, OrderType::Limit, 100, 5);

  EXPECT_EQ(listener.events, (std::vector<Event>{
                                 Accept{3},
                                 Trade{2, 3, 100, 5},
                                 BookUpdate{Side::Buy, 100, 6},
                             }));
}

TEST(OrderBook, ModifyPriceChangeThatCrossesTrades) {
  RecordingListener listener;
  OrderBook book(make_config(), listener);
  book.submit(1, Side::Buy, OrderType::Limit, 99, 5);
  book.submit(2, Side::Sell, OrderType::Limit, 100, 3);
  listener.clear();

  book.modify(1, 101, 5);

  EXPECT_EQ(listener.events, (std::vector<Event>{
                                 Cancel{1, 5},
                                 BookUpdate{Side::Buy, 99, 0},
                                 Accept{1},
                                 Trade{2, 1, 100, 3},
                                 BookUpdate{Side::Sell, 100, 0},
                                 BookUpdate{Side::Buy, 101, 2},
                             }));
  EXPECT_EQ(book.best_bid(), std::optional<Price>{101});
  EXPECT_EQ(book.best_ask(), std::nullopt);
  EXPECT_EQ(book.level_qty(Side::Buy, 101), 2);
  EXPECT_EQ(book.order_count(), 1U);
}

TEST(OrderBook, ModifyOnlyOrderAtALevelRemovesThenRecreatesIt) {
  RecordingListener listener;
  OrderBook book(make_config(100, 1), listener);
  book.submit(1, Side::Buy, OrderType::Limit, 100, 5);
  listener.clear();

  book.modify(1, 101, 5);

  EXPECT_EQ(listener.events, (std::vector<Event>{
                                 Cancel{1, 5},
                                 BookUpdate{Side::Buy, 100, 0},
                                 Accept{1},
                                 BookUpdate{Side::Buy, 101, 5},
                             }));
  EXPECT_EQ(book.best_bid(), std::optional<Price>{101});
  EXPECT_EQ(book.best_ask(), std::nullopt);
  EXPECT_EQ(book.order_count(), 1U);
}

TEST(OrderBook, ModifyPartiallyFilledOrderUsesRemainingQty) {
  RecordingListener listener;
  OrderBook book(make_config(), listener);
  book.submit(1, Side::Sell, OrderType::Limit, 100, 10);
  book.submit(2, Side::Buy, OrderType::Limit, 100, 4);
  ASSERT_EQ(book.level_qty(Side::Sell, 100), 6);
  listener.clear();

  book.modify(1, 100, 12);

  EXPECT_EQ(listener.events, (std::vector<Event>{
                                 Cancel{1, 6},
                                 BookUpdate{Side::Sell, 100, 0},
                                 Accept{1},
                                 BookUpdate{Side::Sell, 100, 12},
                             }));

  listener.clear();
  book.modify(1, 100, 2);

  EXPECT_EQ(listener.events, (std::vector<Event>{
                                 Modify{1, 100, 2},
                                 BookUpdate{Side::Sell, 100, 2},
                             }));
  EXPECT_EQ(book.level_qty(Side::Sell, 100), 2);
}

TEST(OrderBook, ModifyValidationPrecedence) {
  RecordingListener listener;
  OrderBook book(make_config(), listener);
  book.submit(1, Side::Buy, OrderType::Limit, 100, 5);
  listener.clear();

  book.modify(42, 0, 0);
  book.modify(1, 0, 0);
  book.modify(1, 0, 5);
  book.modify(1, 100, 5);

  EXPECT_EQ(listener.events, (std::vector<Event>{
                                 Reject{42, RejectReason::UnknownOrder},
                                 Reject{1, RejectReason::InvalidQuantity},
                                 Reject{1, RejectReason::InvalidPrice},
                                 Reject{1, RejectReason::ModifyNoChange},
                             }));
  EXPECT_EQ(book.order_count(), 1U);
  EXPECT_EQ(book.level_qty(Side::Buy, 100), 5);
}

TEST(OrderBook, ModifyNoChangeUsesRemainingQuantityAfterAPartialFill) {
  RecordingListener listener;
  OrderBook book(make_config(), listener);
  book.submit(1, Side::Sell, OrderType::Limit, 100, 10);
  book.submit(2, Side::Buy, OrderType::Limit, 100, 6);
  ASSERT_EQ(book.level_qty(Side::Sell, 100), 4);
  listener.clear();

  book.modify(1, 100, 4);

  EXPECT_EQ(listener.events, (std::vector<Event>{Reject{1, RejectReason::ModifyNoChange}}));
  EXPECT_EQ(book.level_qty(Side::Sell, 100), 4);
}

TEST(OrderBook, ModifyToACrossingPriceConsumesSeveralLevels) {
  RecordingListener listener;
  OrderBook book(make_config(), listener);
  book.submit(1, Side::Buy, OrderType::Limit, 100, 5);
  book.submit(2, Side::Sell, OrderType::Limit, 101, 2);
  book.submit(3, Side::Sell, OrderType::Limit, 102, 2);
  listener.clear();

  book.modify(1, 102, 3);

  EXPECT_EQ(listener.events, (std::vector<Event>{
                                 Cancel{1, 5},
                                 BookUpdate{Side::Buy, 100, 0},
                                 Accept{1},
                                 Trade{2, 1, 101, 2},
                                 BookUpdate{Side::Sell, 101, 0},
                                 Trade{3, 1, 102, 1},
                                 BookUpdate{Side::Sell, 102, 1},
                             }));
  EXPECT_EQ(book.best_ask(), std::optional<Price>{102});
  EXPECT_EQ(book.level_qty(Side::Sell, 102), 1);
  EXPECT_EQ(book.order_count(), 1U);
}

// Event-contract conformance: every event below is written out by hand from
// the architecture document for a scripted sequence; none of it was produced
// by running the engine.

TEST(OrderBook, EventContractConformanceScript) {
  RecordingListener listener;
  OrderBook book(Config{1, 1000, 100, 100}, listener);
  std::vector<Event> expected;
  auto expect = [&expected](std::initializer_list<Event> events) {
    expected.insert(expected.end(), events.begin(), events.end());
  };

  // 1: rest a bid.
  book.submit(1, Side::Buy, OrderType::Limit, 100, 10);
  expect({Accept{1}, BookUpdate{Side::Buy, 100, 10}});
  // 2: rest an out-of-the-way ask.
  book.submit(2, Side::Sell, OrderType::Limit, 200, 5);
  expect({Accept{2}, BookUpdate{Side::Sell, 200, 5}});
  // 3: a second bid level.
  book.submit(3, Side::Buy, OrderType::Limit, 99, 4);
  expect({Accept{3}, BookUpdate{Side::Buy, 99, 4}});
  // 4: an ask level above the bids.
  book.submit(4, Side::Sell, OrderType::Limit, 101, 3);
  expect({Accept{4}, BookUpdate{Side::Sell, 101, 3}});
  // 5: append to the first bid level.
  book.submit(5, Side::Buy, OrderType::Limit, 100, 1);
  expect({Accept{5}, BookUpdate{Side::Buy, 100, 11}});
  // 6: a market buy lifts two lots from the ask at 101.
  book.submit(6, Side::Buy, OrderType::Market, 0, 2);
  expect({Accept{6}, Trade{4, 6, 101, 2}, BookUpdate{Side::Sell, 101, 1}});
  // 7: a sell above the best bid rests.
  book.submit(7, Side::Sell, OrderType::Limit, 101, 1);
  expect({Accept{7}, BookUpdate{Side::Sell, 101, 2}});
  // 8: asking for the same price and quantity is a no-op.
  book.modify(4, 101, 1);
  expect({Reject{4, RejectReason::ModifyNoChange}});
  // 9: increasing moves the ask to the back of its level.
  book.modify(4, 101, 2);
  expect({Cancel{4, 1}, BookUpdate{Side::Sell, 101, 1}, Accept{4}, BookUpdate{Side::Sell, 101, 3}});
  // 10: a crossing buy sweeps both asks at 101 in FIFO order.
  book.submit(8, Side::Buy, OrderType::Limit, 101, 3);
  expect({Accept{8}, Trade{7, 8, 101, 1}, BookUpdate{Side::Sell, 101, 2}, Trade{4, 8, 101, 2},
          BookUpdate{Side::Sell, 101, 0}});
  // 11: reducing keeps priority and adjusts the level.
  book.modify(1, 100, 4);
  expect({Modify{1, 100, 4}, BookUpdate{Side::Buy, 100, 5}});
  // 12: cancel removes the appended bid.
  book.cancel(5);
  expect({Cancel{5, 1}, BookUpdate{Side::Buy, 100, 4}});
  // 13: a sell exactly filling the level-100 bid.
  book.submit(9, Side::Sell, OrderType::Limit, 100, 3);
  expect({Accept{9}, Trade{1, 9, 100, 3}, BookUpdate{Side::Buy, 100, 1}});
  // 14: the just-filled taker is unknown.
  book.modify(9, 99, 3);
  expect({Reject{9, RejectReason::UnknownOrder}});
  // 15: append to the bid at 99.
  book.submit(10, Side::Buy, OrderType::Limit, 99, 2);
  expect({Accept{10}, BookUpdate{Side::Buy, 99, 6}});
  // 16: a price reduction is cancel + new.
  book.modify(3, 98, 6);
  expect({Cancel{3, 4}, BookUpdate{Side::Buy, 99, 2}, Accept{3}, BookUpdate{Side::Buy, 98, 6}});
  // 17: a sell sweeps 100, 99 and 98.
  book.submit(11, Side::Sell, OrderType::Limit, 98, 7);
  expect({Accept{11}, Trade{1, 11, 100, 1}, BookUpdate{Side::Buy, 100, 0}, Trade{10, 11, 99, 2},
          BookUpdate{Side::Buy, 99, 0}, Trade{3, 11, 98, 4}, BookUpdate{Side::Buy, 98, 2}});
  // 18: the filled taker is unknown.
  book.modify(11, 98, 1);
  expect({Reject{11, RejectReason::UnknownOrder}});
  // 19: a buy lifts one lot from the ask at 200.
  book.submit(12, Side::Buy, OrderType::Limit, 500, 1);
  expect({Accept{12}, Trade{2, 12, 200, 1}, BookUpdate{Side::Sell, 200, 4}});
  // 20: an IOC that cannot fill completely is cancelled.
  book.submit(13, Side::Buy, OrderType::IOC, 200, 10);
  expect({Accept{13}, Trade{2, 13, 200, 4}, BookUpdate{Side::Sell, 200, 0}, Cancel{13, 6}});
  // 21: a sell at the price floor crosses the remaining bid.
  book.submit(14, Side::Sell, OrderType::Limit, 1, 1);
  expect({Accept{14}, Trade{3, 14, 98, 1}, BookUpdate{Side::Buy, 98, 1}});
  // 22: a bid at the price floor rests.
  book.submit(15, Side::Buy, OrderType::Limit, 1, 1);
  expect({Accept{15}, BookUpdate{Side::Buy, 1, 1}});
  // 23: a sell clears the remaining two bid levels.
  book.submit(16, Side::Sell, OrderType::Limit, 1, 2);
  expect({Accept{16}, Trade{3, 16, 98, 1}, BookUpdate{Side::Buy, 98, 0}, Trade{15, 16, 1, 1},
          BookUpdate{Side::Buy, 1, 0}});
  // 24: a bid at the price ceiling rests (no asks).
  book.submit(17, Side::Buy, OrderType::Limit, 1000, 3);
  expect({Accept{17}, BookUpdate{Side::Buy, 1000, 3}});
  // 25: a sell at the ceiling sweeps the bid and rests the remainder.
  book.submit(18, Side::Sell, OrderType::Limit, 1000, 5);
  expect({Accept{18}, Trade{17, 18, 1000, 3}, BookUpdate{Side::Buy, 1000, 0},
          BookUpdate{Side::Sell, 1000, 2}});
  // 26: above the ceiling is rejected.
  book.modify(18, 1001, 2);
  expect({Reject{18, RejectReason::InvalidPrice}});
  // 27: same price and quantity is a no-op.
  book.modify(18, 1000, 2);
  expect({Reject{18, RejectReason::ModifyNoChange}});
  // 28: a non-positive quantity is rejected.
  book.modify(18, 1000, 0);
  expect({Reject{18, RejectReason::InvalidQuantity}});
  // 29: an unknown id is rejected before anything else.
  book.modify(999, 1000, -1);
  expect({Reject{999, RejectReason::UnknownOrder}});
  // 30: cancel the resting ask.
  book.cancel(18);
  expect({Cancel{18, 2}, BookUpdate{Side::Sell, 1000, 0}});
  // 31: cancelling it again is unknown.
  book.cancel(18);
  expect({Reject{18, RejectReason::UnknownOrder}});
  // 32: a bid rests.
  book.submit(19, Side::Buy, OrderType::Limit, 1000, 1);
  expect({Accept{19}, BookUpdate{Side::Buy, 1000, 1}});
  // 33: and is cancelled.
  book.cancel(19);
  expect({Cancel{19, 1}, BookUpdate{Side::Buy, 1000, 0}});

  EXPECT_EQ(listener.events, expected);
  EXPECT_EQ(book.best_bid(), std::nullopt);
  EXPECT_EQ(book.best_ask(), std::nullopt);
  EXPECT_EQ(book.order_count(), 0U);

  DigestBuilder empty;
  EXPECT_EQ(book.digest(), empty.finish());
}

}  // namespace
