#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

#include "lob/digest.hpp"
#include "lob/order_book.hpp"
#include "support/recording_listener.hpp"

namespace {

using lob::Config;
using lob::DigestBuilder;
using lob::LevelInfo;
using lob::OrderBook;
using lob::OrderId;
using lob::OrderType;
using lob::Price;
using lob::Qty;
using lob::RejectReason;
using lob::Side;
using lob::test::Accept;
using lob::test::BookUpdate;
using lob::test::Cancel;
using lob::test::Event;
using lob::test::RecordingListener;
using lob::test::Reject;

Config make_config(std::uint32_t max_orders = 4096, std::uint32_t max_levels = 4096) {
  return Config{1, 1000000, max_orders, max_levels};
}

TEST(OrderBookResting, SubmitLimitOrderRestsAtBackOfLevel) {
  RecordingListener listener;
  OrderBook book(make_config(), listener);

  book.submit(1, Side::Buy, OrderType::Limit, 100, 5);

  EXPECT_EQ(listener.events, (std::vector<Event>{Accept{1}, BookUpdate{Side::Buy, 100, 5}}));
  EXPECT_EQ(book.best_bid(), std::optional<Price>{100});
  EXPECT_EQ(book.best_ask(), std::nullopt);
  EXPECT_EQ(book.level_qty(Side::Buy, 100), 5);
  EXPECT_EQ(book.level_qty(Side::Buy, 99), 0);
  EXPECT_EQ(book.order_count(), 1U);
}

TEST(OrderBookResting, SecondOrderAtSameLevelAppendsToFifoAndAggregates) {
  RecordingListener listener;
  OrderBook book(make_config(), listener);
  book.submit(1, Side::Buy, OrderType::Limit, 100, 5);
  listener.clear();

  book.submit(2, Side::Buy, OrderType::Limit, 100, 7);

  EXPECT_EQ(listener.events, (std::vector<Event>{Accept{2}, BookUpdate{Side::Buy, 100, 12}}));
  DigestBuilder builder;
  builder.level(Side::Buy, 100, 2);
  builder.order(1, 5);
  builder.order(2, 7);
  EXPECT_EQ(book.digest(), builder.finish());
}

TEST(OrderBookResting, CancelRemovesOrderAndUpdatesLevel) {
  RecordingListener listener;
  OrderBook book(make_config(), listener);
  book.submit(1, Side::Buy, OrderType::Limit, 100, 5);
  book.submit(2, Side::Buy, OrderType::Limit, 100, 7);
  listener.clear();

  book.cancel(1);

  EXPECT_EQ(listener.events, (std::vector<Event>{Cancel{1, 5}, BookUpdate{Side::Buy, 100, 7}}));
  EXPECT_EQ(book.level_qty(Side::Buy, 100), 7);
  EXPECT_EQ(book.order_count(), 1U);

  listener.clear();
  book.cancel(2);

  EXPECT_EQ(listener.events, (std::vector<Event>{Cancel{2, 7}, BookUpdate{Side::Buy, 100, 0}}));
  EXPECT_EQ(book.best_bid(), std::nullopt);
  EXPECT_EQ(book.level_qty(Side::Buy, 100), 0);
  EXPECT_EQ(book.order_count(), 0U);
}

TEST(OrderBookResting, CancelMiddleOfFifoKeepsOrder) {
  RecordingListener listener;
  OrderBook book(make_config(), listener);
  book.submit(1, Side::Buy, OrderType::Limit, 100, 1);
  book.submit(2, Side::Buy, OrderType::Limit, 100, 1);
  book.submit(3, Side::Buy, OrderType::Limit, 100, 1);
  listener.clear();

  book.cancel(2);

  EXPECT_EQ(listener.events, (std::vector<Event>{Cancel{2, 1}, BookUpdate{Side::Buy, 100, 2}}));
  DigestBuilder builder;
  builder.level(Side::Buy, 100, 2);
  builder.order(1, 1);
  builder.order(3, 1);
  EXPECT_EQ(book.digest(), builder.finish());
}

TEST(OrderBookResting, CancelUnknownIdIsRejectedNotCrash) {
  RecordingListener listener;
  OrderBook book(make_config(), listener);

  book.cancel(99);

  EXPECT_EQ(listener.events, (std::vector<Event>{Reject{99, RejectReason::UnknownOrder}}));
  EXPECT_EQ(book.order_count(), 0U);
  EXPECT_EQ(book.best_bid(), std::nullopt);
}

TEST(OrderBookResting, DuplicateIdRejected) {
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

TEST(OrderBookResting, InvalidQuantityIsRejected) {
  RecordingListener listener;
  OrderBook book(make_config(), listener);

  book.submit(1, Side::Buy, OrderType::Limit, 100, 0);
  book.submit(2, Side::Buy, OrderType::Limit, 100, -5);
  book.submit(3, Side::Buy, OrderType::Market, 0, 0);

  EXPECT_EQ(listener.events, (std::vector<Event>{
                                 Reject{1, RejectReason::InvalidQuantity},
                                 Reject{2, RejectReason::InvalidQuantity},
                                 Reject{3, RejectReason::InvalidQuantity},
                             }));
  EXPECT_EQ(book.order_count(), 0U);
}

TEST(OrderBookResting, ValidationPrecedenceStopsAtFirstFailure) {
  RecordingListener listener;
  OrderBook book(make_config(), listener);
  book.submit(1, Side::Buy, OrderType::Limit, 100, 5);
  listener.clear();

  book.submit(1, Side::Buy, OrderType::Limit, 0, 0);
  book.submit(1, Side::Buy, OrderType::Limit, 0, 5);
  book.submit(1, Side::Buy, OrderType::Limit, 100, 5);

  EXPECT_EQ(listener.events, (std::vector<Event>{
                                 Reject{1, RejectReason::InvalidQuantity},
                                 Reject{1, RejectReason::InvalidPrice},
                                 Reject{1, RejectReason::DuplicateId},
                             }));
  EXPECT_EQ(book.order_count(), 1U);
}

TEST(OrderBookResting, InvalidPriceIsRejectedForLimitAndIoc) {
  RecordingListener listener;
  OrderBook book(make_config(), listener);

  book.submit(1, Side::Buy, OrderType::Limit, 0, 5);
  book.submit(2, Side::Buy, OrderType::Limit, 1000001, 5);
  book.submit(3, Side::Sell, OrderType::IOC, -1, 5);
  book.submit(4, Side::Sell, OrderType::IOC, 1000001, 5);

  EXPECT_EQ(listener.events, (std::vector<Event>{
                                 Reject{1, RejectReason::InvalidPrice},
                                 Reject{2, RejectReason::InvalidPrice},
                                 Reject{3, RejectReason::InvalidPrice},
                                 Reject{4, RejectReason::InvalidPrice},
                             }));
  EXPECT_EQ(book.order_count(), 0U);
}

TEST(OrderBookResting, MarketOrderIgnoresPrice) {
  RecordingListener listener;
  OrderBook book(make_config(), listener);

  book.submit(1, Side::Buy, OrderType::Market, 0, 5);
  book.submit(2, Side::Sell, OrderType::Market, -12345, 7);

  EXPECT_EQ(listener.events, (std::vector<Event>{
                                 Accept{1},
                                 Cancel{1, 5},
                                 Accept{2},
                                 Cancel{2, 7},
                             }));
  EXPECT_EQ(book.order_count(), 0U);
  EXPECT_EQ(book.best_bid(), std::nullopt);
  EXPECT_EQ(book.best_ask(), std::nullopt);
}

TEST(OrderBookResting, OrderCapacityExhaustionCancelsTheRemainder) {
  RecordingListener listener;
  OrderBook book(make_config(1, 10), listener);
  book.submit(1, Side::Buy, OrderType::Limit, 100, 5);
  listener.clear();

  book.submit(2, Side::Buy, OrderType::Limit, 100, 5);

  EXPECT_EQ(listener.events, (std::vector<Event>{Accept{2}, Cancel{2, 5}}));
  EXPECT_EQ(book.order_count(), 1U);
  EXPECT_EQ(book.level_qty(Side::Buy, 100), 5);

  book.cancel(1);
  listener.clear();
  book.submit(2, Side::Buy, OrderType::Limit, 100, 5);
  EXPECT_EQ(listener.events, (std::vector<Event>{Accept{2}, BookUpdate{Side::Buy, 100, 5}}));
  EXPECT_EQ(book.order_count(), 1U);
}

TEST(OrderBookResting, LevelCapacityExhaustionCancelsTheRemainder) {
  RecordingListener listener;
  OrderBook book(make_config(100, 1), listener);
  book.submit(1, Side::Buy, OrderType::Limit, 100, 5);
  listener.clear();

  book.submit(2, Side::Buy, OrderType::Limit, 101, 5);

  EXPECT_EQ(listener.events, (std::vector<Event>{Accept{2}, Cancel{2, 5}}));
  EXPECT_EQ(book.order_count(), 1U);
  EXPECT_EQ(book.level_qty(Side::Buy, 101), 0);

  book.submit(3, Side::Buy, OrderType::Limit, 100, 5);
  EXPECT_EQ(listener.events, (std::vector<Event>{Accept{2}, Cancel{2, 5}, Accept{3},
                                                 BookUpdate{Side::Buy, 100, 10}}));
  EXPECT_EQ(book.order_count(), 2U);

  book.submit(4, Side::Sell, OrderType::Limit, 200, 1);
  EXPECT_EQ(book.level_qty(Side::Sell, 200), 1);
  EXPECT_EQ(book.order_count(), 3U);
}

TEST(OrderBookResting, QueriesOnEmptyBook) {
  RecordingListener listener;
  OrderBook book(make_config(), listener);

  EXPECT_EQ(book.best_bid(), std::nullopt);
  EXPECT_EQ(book.best_ask(), std::nullopt);
  EXPECT_EQ(book.level_qty(Side::Buy, 100), 0);
  EXPECT_EQ(book.level_qty(Side::Sell, 100), 0);
  EXPECT_EQ(book.order_count(), 0U);

  std::array<LevelInfo, 4> levels{};
  EXPECT_EQ(book.top_levels(Side::Buy, levels), 0U);
  EXPECT_EQ(book.top_levels(Side::Sell, levels), 0U);

  DigestBuilder empty;
  EXPECT_EQ(book.digest(), empty.finish());
}

TEST(OrderBookResting, BestBidAndAskFollowSideOrdering) {
  RecordingListener listener;
  OrderBook book(make_config(), listener);
  book.submit(1, Side::Buy, OrderType::Limit, 100, 1);
  book.submit(2, Side::Buy, OrderType::Limit, 101, 2);
  book.submit(3, Side::Sell, OrderType::Limit, 103, 3);
  book.submit(4, Side::Sell, OrderType::Limit, 102, 4);

  EXPECT_EQ(book.best_bid(), std::optional<Price>{101});
  EXPECT_EQ(book.best_ask(), std::optional<Price>{102});

  std::array<LevelInfo, 4> levels{};
  ASSERT_EQ(book.top_levels(Side::Buy, levels), 2U);
  EXPECT_EQ(levels[0].price, 101);
  EXPECT_EQ(levels[0].qty, 2);
  EXPECT_EQ(levels[0].order_count, 1U);
  EXPECT_EQ(levels[1].price, 100);
  EXPECT_EQ(levels[1].qty, 1);

  ASSERT_EQ(book.top_levels(Side::Sell, levels), 2U);
  EXPECT_EQ(levels[0].price, 102);
  EXPECT_EQ(levels[0].qty, 4);
  EXPECT_EQ(levels[1].price, 103);
  EXPECT_EQ(levels[1].qty, 3);
}

TEST(OrderBookResting, TopLevelsRespectsOutputSize) {
  RecordingListener listener;
  OrderBook book(make_config(), listener);
  book.submit(1, Side::Buy, OrderType::Limit, 100, 1);
  book.submit(2, Side::Buy, OrderType::Limit, 101, 1);
  book.submit(3, Side::Buy, OrderType::Limit, 102, 1);

  std::array<LevelInfo, 1> out{};
  ASSERT_EQ(book.top_levels(Side::Buy, out), 1U);
  EXPECT_EQ(out[0].price, 102);
  EXPECT_EQ(out[0].qty, 1);
}

TEST(OrderBookResting, PricesAtConfiguredBoundariesAreAccepted) {
  RecordingListener listener;
  OrderBook book(make_config(), listener);

  book.submit(1, Side::Buy, OrderType::Limit, 1, 1);
  book.submit(2, Side::Sell, OrderType::Limit, 1000000, 1);

  EXPECT_EQ(book.best_bid(), std::optional<Price>{1});
  EXPECT_EQ(book.best_ask(), std::optional<Price>{1000000});
  EXPECT_EQ(book.order_count(), 2U);
}

TEST(OrderBookResting, DigestMatchesHandBuiltStream) {
  RecordingListener listener;
  OrderBook book(make_config(), listener);
  book.submit(1, Side::Buy, OrderType::Limit, 100, 7);
  book.submit(2, Side::Buy, OrderType::Limit, 100, 4);
  book.submit(3, Side::Buy, OrderType::Limit, 99, 5);
  book.submit(4, Side::Sell, OrderType::Limit, 101, 6);
  book.submit(5, Side::Sell, OrderType::Limit, 101, 2);

  DigestBuilder builder;
  builder.level(Side::Buy, 100, 2);
  builder.order(1, 7);
  builder.order(2, 4);
  builder.level(Side::Buy, 99, 1);
  builder.order(3, 5);
  builder.level(Side::Sell, 101, 2);
  builder.order(4, 6);
  builder.order(5, 2);

  EXPECT_EQ(book.digest(), builder.finish());
}

TEST(OrderBookResting, IdCanBeReusedAfterCancel) {
  RecordingListener listener;
  OrderBook book(make_config(), listener);
  book.submit(1, Side::Buy, OrderType::Limit, 100, 5);
  book.cancel(1);
  listener.clear();

  book.submit(1, Side::Buy, OrderType::Limit, 100, 5);

  EXPECT_EQ(listener.events, (std::vector<Event>{Accept{1}, BookUpdate{Side::Buy, 100, 5}}));
  EXPECT_EQ(book.order_count(), 1U);
}

}  // namespace
