#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <vector>

#include "lob/order_book.hpp"
#include "support/recording_listener.hpp"

namespace {

using lob::Config;
using lob::OrderBook;
using lob::OrderType;
using lob::Price;
using lob::Qty;
using lob::Side;
using lob::test::Accept;
using lob::test::BookUpdate;
using lob::test::Cancel;
using lob::test::Event;
using lob::test::RecordingListener;
using lob::test::Trade;

Config make_config(std::uint32_t max_orders = 4096, std::uint32_t max_levels = 4096) {
  return Config{1, 1000000, max_orders, max_levels};
}

TEST(OrderBookMatching, TradeExecutesAtRestingPrice) {
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

TEST(OrderBookMatching, PartialFillKeepsQueuePosition) {
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

TEST(OrderBookMatching, TwoMakersAtOneLevelFillInFifoOrder) {
  RecordingListener listener;
  OrderBook book(make_config(), listener);
  book.submit(1, Side::Sell, OrderType::Limit, 100, 1);
  book.submit(2, Side::Sell, OrderType::Limit, 100, 1);
  listener.clear();

  book.submit(3, Side::Buy, OrderType::Limit, 100, 2);

  EXPECT_EQ(listener.events, (std::vector<Event>{
                                 Accept{3},
                                 Trade{1, 3, 100, 1},
                                 BookUpdate{Side::Sell, 100, 1},
                                 Trade{2, 3, 100, 1},
                                 BookUpdate{Side::Sell, 100, 0},
                             }));
  EXPECT_EQ(book.order_count(), 0U);
}

TEST(OrderBookMatching, MakerFullyFilledExactlyLeavesSilently) {
  RecordingListener listener;
  OrderBook book(make_config(), listener);
  book.submit(1, Side::Buy, OrderType::Limit, 101, 7);
  listener.clear();

  book.submit(3, Side::Sell, OrderType::Limit, 100, 7);

  EXPECT_EQ(listener.events, (std::vector<Event>{
                                 Accept{3},
                                 Trade{1, 3, 101, 7},
                                 BookUpdate{Side::Buy, 101, 0},
                             }));
  EXPECT_EQ(book.best_bid(), std::nullopt);
  EXPECT_EQ(book.order_count(), 0U);
}

TEST(OrderBookMatching, SweepThreeLevelsWithPartialOnTheLast) {
  RecordingListener listener;
  OrderBook book(make_config(), listener);
  book.submit(1, Side::Sell, OrderType::Limit, 100, 2);
  book.submit(2, Side::Sell, OrderType::Limit, 101, 3);
  book.submit(3, Side::Sell, OrderType::Limit, 102, 4);
  listener.clear();

  book.submit(4, Side::Buy, OrderType::Limit, 102, 8);

  EXPECT_EQ(listener.events, (std::vector<Event>{
                                 Accept{4},
                                 Trade{1, 4, 100, 2},
                                 BookUpdate{Side::Sell, 100, 0},
                                 Trade{2, 4, 101, 3},
                                 BookUpdate{Side::Sell, 101, 0},
                                 Trade{3, 4, 102, 3},
                                 BookUpdate{Side::Sell, 102, 1},
                             }));
  EXPECT_EQ(book.best_ask(), std::optional<Price>{102});
  EXPECT_EQ(book.level_qty(Side::Sell, 102), 1);
  EXPECT_EQ(book.order_count(), 1U);
}

TEST(OrderBookMatching, SellTakerSwipesBidsBestFirst) {
  RecordingListener listener;
  OrderBook book(make_config(), listener);
  book.submit(1, Side::Buy, OrderType::Limit, 101, 2);
  book.submit(2, Side::Buy, OrderType::Limit, 100, 3);
  listener.clear();

  book.submit(3, Side::Sell, OrderType::Limit, 100, 5);

  EXPECT_EQ(listener.events, (std::vector<Event>{
                                 Accept{3},
                                 Trade{1, 3, 101, 2},
                                 BookUpdate{Side::Buy, 101, 0},
                                 Trade{2, 3, 100, 3},
                                 BookUpdate{Side::Buy, 100, 0},
                             }));
  EXPECT_EQ(book.order_count(), 0U);
}

TEST(OrderBookMatching, MarketOrderAgainstEmptyBookIsCancelledNotRested) {
  RecordingListener listener;
  OrderBook book(make_config(), listener);

  book.submit(1, Side::Buy, OrderType::Market, 0, 5);

  EXPECT_EQ(listener.events, (std::vector<Event>{Accept{1}, Cancel{1, 5}}));
  EXPECT_EQ(book.best_bid(), std::nullopt);
  EXPECT_EQ(book.best_ask(), std::nullopt);
  EXPECT_EQ(book.order_count(), 0U);
}

TEST(OrderBookMatching, MarketOrderSweepsThinBookAndCancelsRemainder) {
  RecordingListener listener;
  OrderBook book(make_config(), listener);
  book.submit(1, Side::Sell, OrderType::Limit, 100, 2);
  book.submit(2, Side::Sell, OrderType::Limit, 101, 3);
  listener.clear();

  book.submit(3, Side::Buy, OrderType::Market, 0, 10);

  EXPECT_EQ(listener.events, (std::vector<Event>{
                                 Accept{3},
                                 Trade{1, 3, 100, 2},
                                 BookUpdate{Side::Sell, 100, 0},
                                 Trade{2, 3, 101, 3},
                                 BookUpdate{Side::Sell, 101, 0},
                                 Cancel{3, 5},
                             }));
  EXPECT_EQ(book.order_count(), 0U);
}

TEST(OrderBookMatching, IocPartialThenCancel) {
  RecordingListener listener;
  OrderBook book(make_config(), listener);
  book.submit(1, Side::Sell, OrderType::Limit, 100, 3);
  listener.clear();

  book.submit(2, Side::Buy, OrderType::IOC, 100, 7);

  EXPECT_EQ(listener.events, (std::vector<Event>{
                                 Accept{2},
                                 Trade{1, 2, 100, 3},
                                 BookUpdate{Side::Sell, 100, 0},
                                 Cancel{2, 4},
                             }));
  EXPECT_EQ(book.order_count(), 0U);
  EXPECT_EQ(book.best_ask(), std::nullopt);
}

TEST(OrderBookMatching, IocThatDoesNotCrossIsCancelledNeverRests) {
  RecordingListener listener;
  OrderBook book(make_config(), listener);
  book.submit(1, Side::Sell, OrderType::Limit, 101, 5);
  listener.clear();

  book.submit(2, Side::Buy, OrderType::IOC, 100, 5);

  EXPECT_EQ(listener.events, (std::vector<Event>{Accept{2}, Cancel{2, 5}}));
  EXPECT_EQ(book.best_bid(), std::nullopt);
  EXPECT_EQ(book.best_ask(), std::optional<Price>{101});
  EXPECT_EQ(book.order_count(), 1U);
}

TEST(OrderBookMatching, LimitThatCrossesThenRestsTheRemainder) {
  RecordingListener listener;
  OrderBook book(make_config(), listener);
  book.submit(1, Side::Sell, OrderType::Limit, 100, 3);
  listener.clear();

  book.submit(2, Side::Buy, OrderType::Limit, 100, 5);

  EXPECT_EQ(listener.events, (std::vector<Event>{
                                 Accept{2},
                                 Trade{1, 2, 100, 3},
                                 BookUpdate{Side::Sell, 100, 0},
                                 BookUpdate{Side::Buy, 100, 2},
                             }));
  EXPECT_EQ(book.best_bid(), std::optional<Price>{100});
  EXPECT_EQ(book.best_ask(), std::nullopt);
  EXPECT_EQ(book.level_qty(Side::Buy, 100), 2);
  EXPECT_EQ(book.order_count(), 1U);
}

TEST(OrderBookMatching, TakerIdEqualToJustFilledMakerIdIsAcceptedLater) {
  RecordingListener listener;
  OrderBook book(make_config(), listener);
  book.submit(1, Side::Sell, OrderType::Limit, 100, 5);
  listener.clear();
  book.submit(2, Side::Buy, OrderType::Limit, 100, 5);
  EXPECT_EQ(listener.events, (std::vector<Event>{
                                 Accept{2},
                                 Trade{1, 2, 100, 5},
                                 BookUpdate{Side::Sell, 100, 0},
                             }));
  listener.clear();

  book.submit(1, Side::Buy, OrderType::Limit, 99, 5);

  EXPECT_EQ(listener.events, (std::vector<Event>{Accept{1}, BookUpdate{Side::Buy, 99, 5}}));
  EXPECT_EQ(book.order_count(), 1U);
}

TEST(OrderBookMatching, CrossingLimitRemainderCancelledWhenLevelBudgetIsFull) {
  RecordingListener listener;
  OrderBook book(make_config(100, 1), listener);
  book.submit(1, Side::Buy, OrderType::Limit, 99, 1);
  book.submit(2, Side::Sell, OrderType::Limit, 100, 2);
  listener.clear();

  book.submit(3, Side::Buy, OrderType::Limit, 101, 5);

  EXPECT_EQ(listener.events, (std::vector<Event>{
                                 Accept{3},
                                 Trade{2, 3, 100, 2},
                                 BookUpdate{Side::Sell, 100, 0},
                                 Cancel{3, 3},
                             }));
  EXPECT_EQ(book.best_bid(), std::optional<Price>{99});
  EXPECT_EQ(book.best_ask(), std::nullopt);
  EXPECT_EQ(book.order_count(), 1U);
}

TEST(OrderBookMatching, BestLevelRecomputedAfterEachDrain) {
  RecordingListener listener;
  OrderBook book(make_config(), listener);
  book.submit(1, Side::Sell, OrderType::Limit, 100, 2);
  book.submit(2, Side::Sell, OrderType::Limit, 101, 2);
  book.submit(3, Side::Sell, OrderType::Limit, 102, 2);
  listener.clear();

  book.submit(4, Side::Buy, OrderType::Market, 0, 3);

  EXPECT_EQ(listener.events, (std::vector<Event>{
                                 Accept{4},
                                 Trade{1, 4, 100, 2},
                                 BookUpdate{Side::Sell, 100, 0},
                                 Trade{2, 4, 101, 1},
                                 BookUpdate{Side::Sell, 101, 1},
                             }));
  EXPECT_EQ(book.best_ask(), std::optional<Price>{101});
  EXPECT_EQ(book.level_qty(Side::Sell, 101), 1);
  EXPECT_EQ(book.best_bid(), std::nullopt);
}

TEST(OrderBookMatching, MarketOrderDoesNotRestEvenWhenItCannotFill) {
  RecordingListener listener;
  OrderBook book(make_config(), listener);
  book.submit(1, Side::Sell, OrderType::Limit, 500, 1);
  listener.clear();

  book.submit(2, Side::Buy, OrderType::Market, 0, 2);

  EXPECT_EQ(listener.events, (std::vector<Event>{
                                 Accept{2},
                                 Trade{1, 2, 500, 1},
                                 BookUpdate{Side::Sell, 500, 0},
                                 Cancel{2, 1},
                             }));
  EXPECT_EQ(book.order_count(), 0U);
}

}  // namespace
