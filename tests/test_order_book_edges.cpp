#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <vector>

#include "lob/digest.hpp"
#include "lob/order_book.hpp"
#include "support/recording_listener.hpp"

namespace {

using lob::Config;
using lob::DigestBuilder;
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
using lob::test::Modify;
using lob::test::RecordingListener;
using lob::test::Reject;
using lob::test::Trade;

constexpr OrderId kMaxId = std::numeric_limits<std::uint64_t>::max();

Config config_with(Price min_price, Price max_price, std::uint32_t max_orders,
                   std::uint32_t max_levels) {
  return Config{min_price, max_price, max_orders, max_levels};
}

TEST(OrderBookEdges, IdsZeroAndMaxAreUsableEndToEnd) {
  RecordingListener listener;
  OrderBook book(config_with(1, 1000, 16, 16), listener);

  book.submit(0, Side::Buy, OrderType::Limit, 100, 5);
  EXPECT_EQ(listener.events, (std::vector<Event>{Accept{0}, BookUpdate{Side::Buy, 100, 5}}));

  listener.clear();
  book.submit(kMaxId, Side::Sell, OrderType::Limit, 100, 3);
  EXPECT_EQ(listener.events, (std::vector<Event>{
                                 Accept{kMaxId},
                                 Trade{0, kMaxId, 100, 3},
                                 BookUpdate{Side::Buy, 100, 2},
                             }));

  listener.clear();
  book.cancel(0);
  EXPECT_EQ(listener.events, (std::vector<Event>{Cancel{0, 2}, BookUpdate{Side::Buy, 100, 0}}));

  listener.clear();
  book.submit(kMaxId, Side::Buy, OrderType::Limit, 100, 1);
  EXPECT_EQ(listener.events, (std::vector<Event>{Accept{kMaxId}, BookUpdate{Side::Buy, 100, 1}}));

  listener.clear();
  book.modify(kMaxId, 100, 2);
  EXPECT_EQ(listener.events, (std::vector<Event>{
                                 Cancel{kMaxId, 1},
                                 BookUpdate{Side::Buy, 100, 0},
                                 Accept{kMaxId},
                                 BookUpdate{Side::Buy, 100, 2},
                             }));

  listener.clear();
  book.cancel(0);
  EXPECT_EQ(listener.events, (std::vector<Event>{Reject{0, RejectReason::UnknownOrder}}));

  listener.clear();
  book.submit(0, Side::Sell, OrderType::Limit, 200, 7);
  EXPECT_EQ(listener.events, (std::vector<Event>{Accept{0}, BookUpdate{Side::Sell, 200, 7}}));
  EXPECT_EQ(book.order_count(), 2U);
}

TEST(OrderBookEdges, PricesAtTheConfiguredBoundaries) {
  RecordingListener listener;
  OrderBook book(config_with(50, 150, 16, 16), listener);

  book.submit(1, Side::Buy, OrderType::Limit, 50, 1);
  book.submit(2, Side::Sell, OrderType::Limit, 150, 1);
  book.submit(3, Side::Buy, OrderType::Limit, 49, 1);
  book.submit(4, Side::Sell, OrderType::Limit, 151, 1);
  book.submit(5, Side::Buy, OrderType::IOC, 49, 1);

  EXPECT_EQ(listener.events, (std::vector<Event>{
                                 Accept{1},
                                 BookUpdate{Side::Buy, 50, 1},
                                 Accept{2},
                                 BookUpdate{Side::Sell, 150, 1},
                                 Reject{3, RejectReason::InvalidPrice},
                                 Reject{4, RejectReason::InvalidPrice},
                                 Reject{5, RejectReason::InvalidPrice},
                             }));

  listener.clear();
  book.modify(1, 49, 1);
  book.modify(1, 151, 1);
  book.modify(1, 50, 1);

  EXPECT_EQ(listener.events, (std::vector<Event>{
                                 Reject{1, RejectReason::InvalidPrice},
                                 Reject{1, RejectReason::InvalidPrice},
                                 Reject{1, RejectReason::ModifyNoChange},
                             }));

  listener.clear();
  book.modify(1, 150, 2);

  EXPECT_EQ(listener.events, (std::vector<Event>{
                                 Cancel{1, 1},
                                 BookUpdate{Side::Buy, 50, 0},
                                 Accept{1},
                                 Trade{2, 1, 150, 1},
                                 BookUpdate{Side::Sell, 150, 0},
                                 BookUpdate{Side::Buy, 150, 1},
                             }));
  EXPECT_EQ(book.best_bid(), std::optional<Price>{150});
  EXPECT_EQ(book.best_ask(), std::nullopt);
}

TEST(OrderBookEdges, MaxOrdersOneLifecycle) {
  RecordingListener listener;
  OrderBook book(config_with(1, 1000, 1, 10), listener);

  book.submit(1, Side::Buy, OrderType::Limit, 100, 5);
  book.submit(2, Side::Buy, OrderType::Limit, 100, 5);
  EXPECT_EQ(listener.events, (std::vector<Event>{
                                 Accept{1},
                                 BookUpdate{Side::Buy, 100, 5},
                                 Accept{2},
                                 Cancel{2, 5},
                             }));

  listener.clear();
  book.submit(3, Side::Sell, OrderType::Limit, 100, 3);
  EXPECT_EQ(listener.events, (std::vector<Event>{
                                 Accept{3},
                                 Trade{1, 3, 100, 3},
                                 BookUpdate{Side::Buy, 100, 2},
                             }));

  listener.clear();
  book.submit(4, Side::Sell, OrderType::Limit, 100, 5);
  EXPECT_EQ(listener.events, (std::vector<Event>{
                                 Accept{4},
                                 Trade{1, 4, 100, 2},
                                 BookUpdate{Side::Buy, 100, 0},
                                 BookUpdate{Side::Sell, 100, 3},
                             }));
  EXPECT_EQ(book.order_count(), 1U);
  EXPECT_EQ(book.best_ask(), std::optional<Price>{100});
}

TEST(OrderBookEdges, MaxLevelsOnePerSideIsIndependent) {
  RecordingListener listener;
  OrderBook book(config_with(1, 1000, 100, 1), listener);

  book.submit(1, Side::Buy, OrderType::Limit, 100, 1);
  book.submit(2, Side::Buy, OrderType::Limit, 101, 1);
  book.submit(3, Side::Sell, OrderType::Limit, 200, 1);
  book.submit(4, Side::Sell, OrderType::Limit, 201, 1);
  EXPECT_EQ(listener.events, (std::vector<Event>{
                                 Accept{1},
                                 BookUpdate{Side::Buy, 100, 1},
                                 Accept{2},
                                 Cancel{2, 1},
                                 Accept{3},
                                 BookUpdate{Side::Sell, 200, 1},
                                 Accept{4},
                                 Cancel{4, 1},
                             }));

  listener.clear();
  book.submit(5, Side::Buy, OrderType::Limit, 100, 2);
  book.submit(6, Side::Sell, OrderType::Limit, 200, 2);
  EXPECT_EQ(listener.events, (std::vector<Event>{
                                 Accept{5},
                                 BookUpdate{Side::Buy, 100, 3},
                                 Accept{6},
                                 BookUpdate{Side::Sell, 200, 3},
                             }));

  book.cancel(1);
  listener.clear();
  book.submit(7, Side::Buy, OrderType::Limit, 101, 1);
  EXPECT_EQ(listener.events, (std::vector<Event>{Accept{7}, Cancel{7, 1}}));

  book.cancel(5);
  listener.clear();
  book.submit(7, Side::Buy, OrderType::Limit, 101, 1);
  EXPECT_EQ(listener.events, (std::vector<Event>{Accept{7}, BookUpdate{Side::Buy, 101, 1}}));
  EXPECT_EQ(book.order_count(), 3U);
}

TEST(OrderBookEdges, MarketOrderLargerThanTheWholeOppositeSide) {
  RecordingListener listener;
  OrderBook book(config_with(1, 1000, 100, 100), listener);
  book.submit(1, Side::Sell, OrderType::Limit, 100, 2);
  book.submit(2, Side::Sell, OrderType::Limit, 101, 3);
  book.submit(3, Side::Sell, OrderType::Limit, 102, 4);
  listener.clear();

  book.submit(4, Side::Buy, OrderType::Market, 0, 20);

  EXPECT_EQ(listener.events, (std::vector<Event>{
                                 Accept{4},
                                 Trade{1, 4, 100, 2},
                                 BookUpdate{Side::Sell, 100, 0},
                                 Trade{2, 4, 101, 3},
                                 BookUpdate{Side::Sell, 101, 0},
                                 Trade{3, 4, 102, 4},
                                 BookUpdate{Side::Sell, 102, 0},
                                 Cancel{4, 11},
                             }));
  EXPECT_EQ(book.best_ask(), std::nullopt);
  EXPECT_EQ(book.order_count(), 0U);

  book.submit(5, Side::Buy, OrderType::Limit, 99, 1);
  book.submit(6, Side::Buy, OrderType::Limit, 98, 1);
  listener.clear();
  book.submit(7, Side::Sell, OrderType::Market, 0, 10);
  EXPECT_EQ(listener.events, (std::vector<Event>{
                                 Accept{7},
                                 Trade{5, 7, 99, 1},
                                 BookUpdate{Side::Buy, 99, 0},
                                 Trade{6, 7, 98, 1},
                                 BookUpdate{Side::Buy, 98, 0},
                                 Cancel{7, 8},
                             }));
  EXPECT_EQ(book.order_count(), 0U);
}

TEST(OrderBookEdges, CancelEveryOrderAtALevelInReverseOrder) {
  RecordingListener listener;
  OrderBook book(config_with(1, 1000, 100, 100), listener);
  book.submit(1, Side::Sell, OrderType::Limit, 100, 1);
  book.submit(2, Side::Sell, OrderType::Limit, 100, 2);
  book.submit(3, Side::Sell, OrderType::Limit, 100, 3);
  book.submit(4, Side::Sell, OrderType::Limit, 100, 4);
  book.submit(5, Side::Sell, OrderType::Limit, 100, 5);
  listener.clear();

  book.cancel(5);
  book.cancel(4);
  book.cancel(3);
  book.cancel(2);
  book.cancel(1);

  EXPECT_EQ(listener.events, (std::vector<Event>{
                                 Cancel{5, 5},
                                 BookUpdate{Side::Sell, 100, 10},
                                 Cancel{4, 4},
                                 BookUpdate{Side::Sell, 100, 6},
                                 Cancel{3, 3},
                                 BookUpdate{Side::Sell, 100, 3},
                                 Cancel{2, 2},
                                 BookUpdate{Side::Sell, 100, 1},
                                 Cancel{1, 1},
                                 BookUpdate{Side::Sell, 100, 0},
                             }));
  EXPECT_EQ(book.best_ask(), std::nullopt);
  EXPECT_EQ(book.order_count(), 0U);

  DigestBuilder empty;
  EXPECT_EQ(book.digest(), empty.finish());
}

TEST(OrderBookEdges, ModifyJustFilledIdIsUnknown) {
  RecordingListener listener;
  OrderBook book(config_with(1, 1000, 16, 16), listener);
  book.submit(1, Side::Sell, OrderType::Limit, 100, 5);
  book.submit(2, Side::Buy, OrderType::Limit, 100, 5);
  listener.clear();

  book.modify(1, 100, 5);
  book.cancel(1);

  EXPECT_EQ(listener.events, (std::vector<Event>{
                                 Reject{1, RejectReason::UnknownOrder},
                                 Reject{1, RejectReason::UnknownOrder},
                             }));

  listener.clear();
  book.submit(1, Side::Buy, OrderType::Limit, 99, 1);
  EXPECT_EQ(listener.events, (std::vector<Event>{Accept{1}, BookUpdate{Side::Buy, 99, 1}}));
  EXPECT_EQ(book.order_count(), 1U);
}

TEST(OrderBookEdges, LevelsAtBothRangeEndsOfALargeRange) {
  RecordingListener listener;
  OrderBook book(config_with(1, 65536, 16, 16), listener);

  book.submit(1, Side::Buy, OrderType::Limit, 1, 3);
  book.submit(2, Side::Sell, OrderType::Limit, 65536, 7);
  EXPECT_EQ(listener.events, (std::vector<Event>{
                                 Accept{1},
                                 BookUpdate{Side::Buy, 1, 3},
                                 Accept{2},
                                 BookUpdate{Side::Sell, 65536, 7},
                             }));
  EXPECT_EQ(book.best_bid(), std::optional<Price>{1});
  EXPECT_EQ(book.best_ask(), std::optional<Price>{65536});

  listener.clear();
  book.submit(3, Side::Sell, OrderType::Limit, 1, 5);
  EXPECT_EQ(listener.events, (std::vector<Event>{
                                 Accept{3},
                                 Trade{1, 3, 1, 3},
                                 BookUpdate{Side::Buy, 1, 0},
                                 BookUpdate{Side::Sell, 1, 2},
                             }));
  EXPECT_EQ(book.best_ask(), std::optional<Price>{1});
  EXPECT_EQ(book.best_bid(), std::nullopt);

  listener.clear();
  book.cancel(3);
  EXPECT_EQ(listener.events, (std::vector<Event>{Cancel{3, 2}, BookUpdate{Side::Sell, 1, 0}}));
  EXPECT_EQ(book.best_ask(), std::optional<Price>{65536});
  EXPECT_EQ(book.order_count(), 1U);
}

TEST(OrderBookEdges, MarketSweepAcrossWidePriceGaps) {
  RecordingListener listener;
  OrderBook book(config_with(1, 10000, 64, 64), listener);
  book.submit(1, Side::Sell, OrderType::Limit, 100, 2);
  book.submit(2, Side::Sell, OrderType::Limit, 200, 3);
  book.submit(3, Side::Sell, OrderType::Limit, 4400, 4);
  book.submit(4, Side::Sell, OrderType::Limit, 9950, 5);
  book.submit(5, Side::Sell, OrderType::Limit, 10000, 6);
  listener.clear();

  book.submit(6, Side::Buy, OrderType::Market, 0, 100);

  EXPECT_EQ(listener.events, (std::vector<Event>{
                                 Accept{6},
                                 Trade{1, 6, 100, 2},
                                 BookUpdate{Side::Sell, 100, 0},
                                 Trade{2, 6, 200, 3},
                                 BookUpdate{Side::Sell, 200, 0},
                                 Trade{3, 6, 4400, 4},
                                 BookUpdate{Side::Sell, 4400, 0},
                                 Trade{4, 6, 9950, 5},
                                 BookUpdate{Side::Sell, 9950, 0},
                                 Trade{5, 6, 10000, 6},
                                 BookUpdate{Side::Sell, 10000, 0},
                                 Cancel{6, 80},
                             }));
  EXPECT_EQ(book.best_ask(), std::nullopt);
  EXPECT_EQ(book.order_count(), 0U);

  book.submit(7, Side::Buy, OrderType::Limit, 9900, 1);
  book.submit(8, Side::Buy, OrderType::Limit, 500, 1);
  book.submit(9, Side::Buy, OrderType::Limit, 100, 1);
  listener.clear();
  book.submit(10, Side::Sell, OrderType::Limit, 100, 10);
  EXPECT_EQ(listener.events, (std::vector<Event>{
                                 Accept{10},
                                 Trade{7, 10, 9900, 1},
                                 BookUpdate{Side::Buy, 9900, 0},
                                 Trade{8, 10, 500, 1},
                                 BookUpdate{Side::Buy, 500, 0},
                                 Trade{9, 10, 100, 1},
                                 BookUpdate{Side::Buy, 100, 0},
                                 BookUpdate{Side::Sell, 100, 7},
                             }));
  EXPECT_EQ(book.best_bid(), std::nullopt);
  EXPECT_EQ(book.best_ask(), std::optional<Price>{100});
  EXPECT_EQ(book.order_count(), 1U);
}

TEST(OrderBookEdges, CancelBestWhenTheNextLevelIsManyWordsAway) {
  RecordingListener listener;
  OrderBook book(config_with(1, 10000, 16, 16), listener);
  book.submit(1, Side::Buy, OrderType::Limit, 10, 4);
  book.submit(2, Side::Buy, OrderType::Limit, 4500, 5);
  book.submit(3, Side::Sell, OrderType::Limit, 5000, 2);
  book.submit(4, Side::Sell, OrderType::Limit, 9500, 3);
  listener.clear();

  book.cancel(2);
  EXPECT_EQ(listener.events, (std::vector<Event>{Cancel{2, 5}, BookUpdate{Side::Buy, 4500, 0}}));
  EXPECT_EQ(book.best_bid(), std::optional<Price>{10});

  listener.clear();
  book.cancel(3);
  EXPECT_EQ(listener.events, (std::vector<Event>{Cancel{3, 2}, BookUpdate{Side::Sell, 5000, 0}}));
  EXPECT_EQ(book.best_ask(), std::optional<Price>{9500});
  EXPECT_EQ(book.best_bid(), std::optional<Price>{10});

  listener.clear();
  book.cancel(4);
  book.cancel(1);
  EXPECT_EQ(listener.events, (std::vector<Event>{
                                 Cancel{4, 3},
                                 BookUpdate{Side::Sell, 9500, 0},
                                 Cancel{1, 4},
                                 BookUpdate{Side::Buy, 10, 0},
                             }));
  EXPECT_EQ(book.best_bid(), std::nullopt);
  EXPECT_EQ(book.best_ask(), std::nullopt);
  EXPECT_EQ(book.order_count(), 0U);
}

TEST(OrderBookEdges, ModifyAtTheBoundariesWithRestingNeighbour) {
  RecordingListener listener;
  OrderBook book(config_with(10, 20, 16, 16), listener);
  book.submit(1, Side::Buy, OrderType::Limit, 10, 5);
  book.submit(2, Side::Sell, OrderType::Limit, 20, 5);
  listener.clear();

  book.modify(2, 20, 5);
  EXPECT_EQ(listener.events, (std::vector<Event>{Reject{2, RejectReason::ModifyNoChange}}));

  listener.clear();
  book.modify(2, 20, 3);
  EXPECT_EQ(listener.events, (std::vector<Event>{
                                 Modify{2, 20, 3},
                                 BookUpdate{Side::Sell, 20, 3},
                             }));

  listener.clear();
  book.submit(3, Side::Buy, OrderType::Limit, 20, 3);
  EXPECT_EQ(listener.events, (std::vector<Event>{
                                 Accept{3},
                                 Trade{2, 3, 20, 3},
                                 BookUpdate{Side::Sell, 20, 0},
                             }));
  EXPECT_EQ(book.best_bid(), std::optional<Price>{10});
  EXPECT_EQ(book.best_ask(), std::nullopt);
}

}  // namespace
