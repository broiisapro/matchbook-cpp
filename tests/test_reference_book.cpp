// Event vectors in this file are written by hand from the event contract in
// docs/architecture.md section 4. They are not captured from the engine.

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <initializer_list>
#include <span>
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
using Event = lob::test::Event;

constexpr lob::Config kConfig{1, 1000, 1000, 100};

class ReferenceBookTest : public ::testing::Test {
 protected:
  void expect_events(std::initializer_list<Event> expected) {
    EXPECT_EQ(listener.events, std::vector<Event>(expected));
    listener.clear();
  }

  lob::test::RecordingListener listener;
  lob::ref::ReferenceBook book{kConfig, listener};
};

std::uint64_t empty_book_digest() { return lob::DigestBuilder{}.finish(); }

TEST_F(ReferenceBookTest, EmptyBookQueriesAndDigest) {
  EXPECT_EQ(book.best_bid(), std::nullopt);
  EXPECT_EQ(book.best_ask(), std::nullopt);
  EXPECT_EQ(book.level_qty(Side::Buy, 100), 0);
  EXPECT_EQ(book.level_qty(Side::Sell, 100), 0);
  EXPECT_EQ(book.order_count(), 0U);
  EXPECT_EQ(book.digest(), empty_book_digest());

  std::array<lob::LevelInfo, 4> levels{};
  EXPECT_EQ(book.top_levels(Side::Buy, levels), 0U);
  EXPECT_EQ(book.top_levels(Side::Sell, levels), 0U);

  EXPECT_TRUE(listener.events.empty());
}

TEST_F(ReferenceBookTest, SubmitRestsAtBackOfLevel) {
  book.submit(1, Side::Buy, OrderType::Limit, 100, 10);
  expect_events({lob::test::Accept{1}, lob::test::BookUpdate{Side::Buy, 100, 10}});

  book.submit(2, Side::Buy, OrderType::Limit, 100, 5);
  expect_events({lob::test::Accept{2}, lob::test::BookUpdate{Side::Buy, 100, 15}});

  book.submit(3, Side::Sell, OrderType::Limit, 101, 7);
  expect_events({lob::test::Accept{3}, lob::test::BookUpdate{Side::Sell, 101, 7}});

  EXPECT_EQ(book.best_bid(), 100);
  EXPECT_EQ(book.best_ask(), 101);
  EXPECT_EQ(book.level_qty(Side::Buy, 100), 15);
  EXPECT_EQ(book.level_qty(Side::Sell, 101), 7);
  EXPECT_EQ(book.order_count(), 3U);

  std::array<lob::LevelInfo, 4> levels{};
  ASSERT_EQ(book.top_levels(Side::Buy, levels), 1U);
  EXPECT_EQ(levels[0].price, 100);
  EXPECT_EQ(levels[0].qty, 15);
  EXPECT_EQ(levels[0].order_count, 2U);

  lob::DigestBuilder expected;
  expected.level(Side::Buy, 100, 2);
  expected.order(1, 10);
  expected.order(2, 5);
  expected.level(Side::Sell, 101, 1);
  expected.order(3, 7);
  EXPECT_EQ(book.digest(), expected.finish());
}

TEST_F(ReferenceBookTest, TopLevelsOrderedBestFirstAndTruncated) {
  book.submit(1, Side::Sell, OrderType::Limit, 103, 1);
  book.submit(2, Side::Sell, OrderType::Limit, 101, 2);
  book.submit(3, Side::Sell, OrderType::Limit, 102, 3);
  book.submit(4, Side::Buy, OrderType::Limit, 98, 4);
  book.submit(5, Side::Buy, OrderType::Limit, 100, 5);
  book.submit(6, Side::Buy, OrderType::Limit, 99, 6);
  listener.clear();

  std::array<lob::LevelInfo, 3> asks{};
  ASSERT_EQ(book.top_levels(Side::Sell, asks), 3U);
  EXPECT_EQ(asks[0].price, 101);
  EXPECT_EQ(asks[0].qty, 2);
  EXPECT_EQ(asks[1].price, 102);
  EXPECT_EQ(asks[1].qty, 3);
  EXPECT_EQ(asks[2].price, 103);
  EXPECT_EQ(asks[2].qty, 1);

  std::array<lob::LevelInfo, 2> bids{};
  ASSERT_EQ(book.top_levels(Side::Buy, bids), 2U);
  EXPECT_EQ(bids[0].price, 100);
  EXPECT_EQ(bids[1].price, 99);

  std::span<lob::LevelInfo> empty;
  EXPECT_EQ(book.top_levels(Side::Buy, empty), 0U);

  EXPECT_EQ(book.best_bid(), 100);
  EXPECT_EQ(book.best_ask(), 101);
  EXPECT_TRUE(listener.events.empty());
}

TEST_F(ReferenceBookTest, DuplicateLiveIdRejectedAndIdReusableAfterCancel) {
  book.submit(1, Side::Buy, OrderType::Limit, 100, 10);
  listener.clear();

  book.submit(1, Side::Sell, OrderType::Limit, 150, 5);
  expect_events({lob::test::Reject{1, RejectReason::DuplicateId}});
  EXPECT_EQ(book.order_count(), 1U);

  book.cancel(1);
  expect_events({lob::test::Cancel{1, 10}, lob::test::BookUpdate{Side::Buy, 100, 0}});

  book.submit(1, Side::Sell, OrderType::Limit, 150, 5);
  expect_events({lob::test::Accept{1}, lob::test::BookUpdate{Side::Sell, 150, 5}});
}

TEST_F(ReferenceBookTest, CancelRemovesOrderAndReportsRemainingQuantity) {
  book.submit(1, Side::Buy, OrderType::Limit, 100, 10);
  book.submit(2, Side::Buy, OrderType::Limit, 100, 5);
  listener.clear();

  book.cancel(1);
  expect_events({lob::test::Cancel{1, 10}, lob::test::BookUpdate{Side::Buy, 100, 5}});
  EXPECT_EQ(book.level_qty(Side::Buy, 100), 5);

  book.cancel(2);
  expect_events({lob::test::Cancel{2, 5}, lob::test::BookUpdate{Side::Buy, 100, 0}});
  EXPECT_EQ(book.best_bid(), std::nullopt);
  EXPECT_EQ(book.order_count(), 0U);

  book.cancel(99);
  expect_events({lob::test::Reject{99, RejectReason::UnknownOrder}});
}

TEST_F(ReferenceBookTest, CancelAfterPartialFillReportsRemainingNotOriginal) {
  book.submit(1, Side::Buy, OrderType::Limit, 100, 10);
  book.submit(2, Side::Sell, OrderType::Limit, 100, 4);
  listener.clear();

  book.cancel(1);
  expect_events({lob::test::Cancel{1, 6}, lob::test::BookUpdate{Side::Buy, 100, 0}});
}

TEST_F(ReferenceBookTest, SubmitValidationOrderAndPrecedence) {
  book.submit(1, Side::Buy, OrderType::Limit, 100, 0);
  expect_events({lob::test::Reject{1, RejectReason::InvalidQuantity}});

  book.submit(1, Side::Buy, OrderType::Limit, 50, -3);
  expect_events({lob::test::Reject{1, RejectReason::InvalidQuantity}});

  book.submit(1, Side::Buy, OrderType::Limit, 0, 5);
  expect_events({lob::test::Reject{1, RejectReason::InvalidPrice}});

  book.submit(1, Side::Buy, OrderType::IOC, 1001, 5);
  expect_events({lob::test::Reject{1, RejectReason::InvalidPrice}});

  book.submit(7, Side::Buy, OrderType::Limit, 100, 5);
  listener.clear();

  book.submit(7, Side::Buy, OrderType::Limit, 0, 0);
  expect_events({lob::test::Reject{7, RejectReason::InvalidQuantity}});

  book.submit(7, Side::Buy, OrderType::Limit, 0, 5);
  expect_events({lob::test::Reject{7, RejectReason::InvalidPrice}});

  book.submit(7, Side::Buy, OrderType::Limit, 100, 5);
  expect_events({lob::test::Reject{7, RejectReason::DuplicateId}});

  EXPECT_EQ(book.order_count(), 1U);
  EXPECT_EQ(book.level_qty(Side::Buy, 100), 5);
}

TEST_F(ReferenceBookTest, MarketIgnoresPriceIncludingOutOfRange) {
  book.submit(1, Side::Buy, OrderType::Market, 0, 5);
  expect_events({lob::test::Accept{1}, lob::test::Cancel{1, 5}});

  book.submit(2, Side::Buy, OrderType::Market, -1000, 5);
  expect_events({lob::test::Accept{2}, lob::test::Cancel{2, 5}});

  book.submit(3, Side::Sell, OrderType::Market, 999999, 5);
  expect_events({lob::test::Accept{3}, lob::test::Cancel{3, 5}});

  EXPECT_EQ(book.order_count(), 0U);
}

TEST(ReferenceBookValidation, PriceBoundariesAreInclusive) {
  constexpr lob::Config config{100, 200, 10, 10};
  lob::test::RecordingListener listener;
  lob::ref::ReferenceBook book(config, listener);

  book.submit(1, Side::Buy, OrderType::Limit, 100, 1);
  EXPECT_EQ(listener.events,
            (std::vector<Event>{lob::test::Accept{1}, lob::test::BookUpdate{Side::Buy, 100, 1}}));
  listener.clear();

  book.submit(2, Side::Buy, OrderType::Limit, 99, 1);
  EXPECT_EQ(listener.events,
            (std::vector<Event>{lob::test::Reject{2, RejectReason::InvalidPrice}}));
  listener.clear();

  book.submit(3, Side::Sell, OrderType::Limit, 200, 1);
  EXPECT_EQ(listener.events,
            (std::vector<Event>{lob::test::Accept{3}, lob::test::BookUpdate{Side::Sell, 200, 1}}));
  listener.clear();

  book.submit(4, Side::Sell, OrderType::Limit, 201, 1);
  EXPECT_EQ(listener.events,
            (std::vector<Event>{lob::test::Reject{4, RejectReason::InvalidPrice}}));
}

TEST_F(ReferenceBookTest, TradeExecutesAtRestingPrice) {
  book.submit(1, Side::Buy, OrderType::Limit, 100, 10);
  listener.clear();

  book.submit(2, Side::Sell, OrderType::Limit, 99, 4);
  expect_events({lob::test::Accept{2}, lob::test::Trade{1, 2, 100, 4},
                 lob::test::BookUpdate{Side::Buy, 100, 6}});

  EXPECT_EQ(book.best_ask(), std::nullopt);
  EXPECT_EQ(book.level_qty(Side::Sell, 99), 0);
  EXPECT_EQ(book.level_qty(Side::Buy, 100), 6);
}

TEST_F(ReferenceBookTest, PartialFillKeepsQueuePosition) {
  book.submit(1, Side::Buy, OrderType::Limit, 100, 10);
  book.submit(2, Side::Buy, OrderType::Limit, 100, 5);
  listener.clear();

  book.submit(3, Side::Sell, OrderType::Limit, 100, 7);
  expect_events({lob::test::Accept{3}, lob::test::Trade{1, 3, 100, 7},
                 lob::test::BookUpdate{Side::Buy, 100, 8}});

  book.submit(4, Side::Sell, OrderType::Limit, 100, 5);
  expect_events({lob::test::Accept{4}, lob::test::Trade{1, 4, 100, 3},
                 lob::test::BookUpdate{Side::Buy, 100, 5}, lob::test::Trade{2, 4, 100, 2},
                 lob::test::BookUpdate{Side::Buy, 100, 3}});

  book.submit(5, Side::Sell, OrderType::Limit, 100, 3);
  expect_events({lob::test::Accept{5}, lob::test::Trade{2, 5, 100, 3},
                 lob::test::BookUpdate{Side::Buy, 100, 0}});

  EXPECT_EQ(book.order_count(), 0U);
}

TEST_F(ReferenceBookTest, SellSideMakersFillInFifoOrder) {
  book.submit(1, Side::Sell, OrderType::Limit, 150, 10);
  book.submit(2, Side::Sell, OrderType::Limit, 150, 5);
  listener.clear();

  book.submit(3, Side::Buy, OrderType::Limit, 150, 12);
  expect_events({lob::test::Accept{3}, lob::test::Trade{1, 3, 150, 10},
                 lob::test::BookUpdate{Side::Sell, 150, 5}, lob::test::Trade{2, 3, 150, 2},
                 lob::test::BookUpdate{Side::Sell, 150, 3}});

  EXPECT_EQ(book.level_qty(Side::Sell, 150), 3);
  EXPECT_EQ(book.best_ask(), 150);
}

TEST_F(ReferenceBookTest, SweepAcrossLevelsAndRestsTakerRemainder) {
  book.submit(1, Side::Buy, OrderType::Limit, 100, 3);
  book.submit(2, Side::Buy, OrderType::Limit, 100, 2);
  book.submit(3, Side::Buy, OrderType::Limit, 99, 4);
  listener.clear();

  book.submit(4, Side::Sell, OrderType::Limit, 99, 10);
  expect_events({lob::test::Accept{4}, lob::test::Trade{1, 4, 100, 3},
                 lob::test::BookUpdate{Side::Buy, 100, 2}, lob::test::Trade{2, 4, 100, 2},
                 lob::test::BookUpdate{Side::Buy, 100, 0}, lob::test::Trade{3, 4, 99, 4},
                 lob::test::BookUpdate{Side::Buy, 99, 0},
                 lob::test::BookUpdate{Side::Sell, 99, 1}});

  EXPECT_EQ(book.best_bid(), std::nullopt);
  EXPECT_EQ(book.best_ask(), 99);
  EXPECT_EQ(book.order_count(), 1U);
}

TEST_F(ReferenceBookTest, LimitCrossingRestsTheRemainder) {
  book.submit(1, Side::Sell, OrderType::Limit, 101, 4);
  book.submit(2, Side::Sell, OrderType::Limit, 102, 6);
  listener.clear();

  book.submit(3, Side::Buy, OrderType::Limit, 102, 12);
  expect_events({lob::test::Accept{3}, lob::test::Trade{1, 3, 101, 4},
                 lob::test::BookUpdate{Side::Sell, 101, 0}, lob::test::Trade{2, 3, 102, 6},
                 lob::test::BookUpdate{Side::Sell, 102, 0},
                 lob::test::BookUpdate{Side::Buy, 102, 2}});

  EXPECT_EQ(book.best_ask(), std::nullopt);
  EXPECT_EQ(book.best_bid(), 102);
  EXPECT_EQ(book.level_qty(Side::Buy, 102), 2);
  EXPECT_EQ(book.order_count(), 1U);
}

TEST_F(ReferenceBookTest, IocPartialFillThenCancelRemainder) {
  book.submit(1, Side::Buy, OrderType::Limit, 100, 5);
  listener.clear();

  book.submit(2, Side::Sell, OrderType::IOC, 100, 10);
  expect_events({lob::test::Accept{2}, lob::test::Trade{1, 2, 100, 5},
                 lob::test::BookUpdate{Side::Buy, 100, 0}, lob::test::Cancel{2, 5}});

  EXPECT_EQ(book.order_count(), 0U);
  EXPECT_EQ(book.best_bid(), std::nullopt);
}

TEST_F(ReferenceBookTest, IocThatDoesNotCrossIsCancelledNotRested) {
  book.submit(1, Side::Buy, OrderType::IOC, 140, 5);
  expect_events({lob::test::Accept{1}, lob::test::Cancel{1, 5}});

  EXPECT_EQ(book.order_count(), 0U);
  EXPECT_EQ(book.best_bid(), std::nullopt);
}

TEST_F(ReferenceBookTest, IocFullyFilledEmitsNoCancel) {
  book.submit(1, Side::Sell, OrderType::Limit, 120, 3);
  listener.clear();

  book.submit(2, Side::Buy, OrderType::IOC, 125, 3);
  expect_events({lob::test::Accept{2}, lob::test::Trade{1, 2, 120, 3},
                 lob::test::BookUpdate{Side::Sell, 120, 0}});
}

TEST_F(ReferenceBookTest, MarketOrderAgainstEmptyBookIsCancelledNotRested) {
  book.submit(1, Side::Buy, OrderType::Market, 0, 5);
  expect_events({lob::test::Accept{1}, lob::test::Cancel{1, 5}});

  EXPECT_EQ(book.best_bid(), std::nullopt);
  EXPECT_EQ(book.best_ask(), std::nullopt);
  EXPECT_EQ(book.order_count(), 0U);
  EXPECT_EQ(book.digest(), empty_book_digest());
}

TEST_F(ReferenceBookTest, MarketSellSweepsBidsAndCancelsRemainder) {
  book.submit(1, Side::Buy, OrderType::Limit, 100, 2);
  book.submit(2, Side::Buy, OrderType::Limit, 99, 3);
  listener.clear();

  book.submit(3, Side::Sell, OrderType::Market, 0, 6);
  expect_events({lob::test::Accept{3}, lob::test::Trade{1, 3, 100, 2},
                 lob::test::BookUpdate{Side::Buy, 100, 0}, lob::test::Trade{2, 3, 99, 3},
                 lob::test::BookUpdate{Side::Buy, 99, 0}, lob::test::Cancel{3, 1}});

  EXPECT_EQ(book.order_count(), 0U);
}

TEST_F(ReferenceBookTest, MarketBuySweepsAsksInPriceOrder) {
  book.submit(1, Side::Sell, OrderType::Limit, 101, 2);
  book.submit(2, Side::Sell, OrderType::Limit, 102, 3);
  listener.clear();

  book.submit(3, Side::Buy, OrderType::Market, 0, 4);
  expect_events({lob::test::Accept{3}, lob::test::Trade{1, 3, 101, 2},
                 lob::test::BookUpdate{Side::Sell, 101, 0}, lob::test::Trade{2, 3, 102, 2},
                 lob::test::BookUpdate{Side::Sell, 102, 1}});

  EXPECT_EQ(book.best_ask(), 102);
  EXPECT_EQ(book.level_qty(Side::Sell, 102), 1);
}

TEST_F(ReferenceBookTest, ModifyReduceKeepsQueuePriority) {
  book.submit(1, Side::Buy, OrderType::Limit, 100, 10);
  book.submit(2, Side::Buy, OrderType::Limit, 100, 5);
  listener.clear();

  book.modify(1, 100, 4);
  expect_events({lob::test::Modify{1, 100, 4}, lob::test::BookUpdate{Side::Buy, 100, 9}});

  book.submit(3, Side::Sell, OrderType::Limit, 100, 5);
  expect_events({lob::test::Accept{3}, lob::test::Trade{1, 3, 100, 4},
                 lob::test::BookUpdate{Side::Buy, 100, 5}, lob::test::Trade{2, 3, 100, 1},
                 lob::test::BookUpdate{Side::Buy, 100, 4}});

  EXPECT_EQ(book.order_count(), 1U);
  EXPECT_EQ(book.best_bid(), 100);
  EXPECT_EQ(book.level_qty(Side::Buy, 100), 4);
}

TEST_F(ReferenceBookTest, ModifyIncreaseLosesQueuePriority) {
  book.submit(1, Side::Buy, OrderType::Limit, 100, 10);
  book.submit(2, Side::Buy, OrderType::Limit, 100, 5);
  listener.clear();

  book.modify(1, 100, 12);
  expect_events({lob::test::Cancel{1, 10}, lob::test::BookUpdate{Side::Buy, 100, 5},
                 lob::test::Accept{1}, lob::test::BookUpdate{Side::Buy, 100, 17}});

  book.submit(3, Side::Sell, OrderType::Limit, 100, 6);
  expect_events({lob::test::Accept{3}, lob::test::Trade{2, 3, 100, 5},
                 lob::test::BookUpdate{Side::Buy, 100, 12}, lob::test::Trade{1, 3, 100, 1},
                 lob::test::BookUpdate{Side::Buy, 100, 11}});

  EXPECT_EQ(book.level_qty(Side::Buy, 100), 11);
}

TEST_F(ReferenceBookTest, ModifyPriceChangeCancelsAndRestsAtBack) {
  book.submit(1, Side::Buy, OrderType::Limit, 100, 10);
  book.submit(2, Side::Buy, OrderType::Limit, 105, 3);
  listener.clear();

  book.modify(1, 105, 10);
  expect_events({lob::test::Cancel{1, 10}, lob::test::BookUpdate{Side::Buy, 100, 0},
                 lob::test::Accept{1}, lob::test::BookUpdate{Side::Buy, 105, 13}});

  lob::DigestBuilder expected;
  expected.level(Side::Buy, 105, 2);
  expected.order(2, 3);
  expected.order(1, 10);
  EXPECT_EQ(book.digest(), expected.finish());
}

TEST_F(ReferenceBookTest, ModifyNoChangeRejected) {
  book.submit(1, Side::Buy, OrderType::Limit, 100, 10);
  const std::uint64_t before = book.digest();
  listener.clear();

  book.modify(1, 100, 10);
  expect_events({lob::test::Reject{1, RejectReason::ModifyNoChange}});

  EXPECT_EQ(book.digest(), before);
  EXPECT_EQ(book.order_count(), 1U);
}

TEST_F(ReferenceBookTest, ModifyValidationOrderAndPrecedence) {
  book.modify(99, 100, 0);
  expect_events({lob::test::Reject{99, RejectReason::UnknownOrder}});

  book.submit(1, Side::Buy, OrderType::Limit, 100, 10);
  listener.clear();

  book.modify(1, 0, 0);
  expect_events({lob::test::Reject{1, RejectReason::InvalidQuantity}});

  book.modify(1, 0, 10);
  expect_events({lob::test::Reject{1, RejectReason::InvalidPrice}});

  book.modify(1, 0, 10);
  expect_events({lob::test::Reject{1, RejectReason::InvalidPrice}});

  book.modify(1, 100, 10);
  expect_events({lob::test::Reject{1, RejectReason::ModifyNoChange}});

  book.modify(1, 100, 4);
  expect_events({lob::test::Modify{1, 100, 4}, lob::test::BookUpdate{Side::Buy, 100, 4}});

  book.modify(1, 0, 4);
  expect_events({lob::test::Reject{1, RejectReason::InvalidPrice}});

  EXPECT_EQ(book.level_qty(Side::Buy, 100), 4);
}

TEST_F(ReferenceBookTest, ModifyToCrossingPriceMatchesThenRests) {
  book.submit(1, Side::Sell, OrderType::Limit, 120, 4);
  book.submit(2, Side::Buy, OrderType::Limit, 100, 10);
  listener.clear();

  book.modify(2, 125, 10);
  expect_events({lob::test::Cancel{2, 10}, lob::test::BookUpdate{Side::Buy, 100, 0},
                 lob::test::Accept{2}, lob::test::Trade{1, 2, 120, 4},
                 lob::test::BookUpdate{Side::Sell, 120, 0},
                 lob::test::BookUpdate{Side::Buy, 125, 6}});

  EXPECT_EQ(book.best_bid(), 125);
  EXPECT_EQ(book.level_qty(Side::Buy, 125), 6);
}

TEST_F(ReferenceBookTest, ModifyUnknownFilledIdRejected) {
  book.submit(1, Side::Buy, OrderType::Limit, 100, 5);
  book.submit(2, Side::Sell, OrderType::Limit, 100, 5);
  listener.clear();

  book.modify(1, 101, 5);
  expect_events({lob::test::Reject{1, RejectReason::UnknownOrder}});
}

TEST_F(ReferenceBookTest, IdReuseAfterFullFill) {
  book.submit(1, Side::Buy, OrderType::Limit, 100, 5);
  book.submit(2, Side::Sell, OrderType::Limit, 100, 5);
  listener.clear();

  book.submit(1, Side::Sell, OrderType::Limit, 150, 2);
  expect_events({lob::test::Accept{1}, lob::test::BookUpdate{Side::Sell, 150, 2}});

  book.cancel(2);
  expect_events({lob::test::Reject{2, RejectReason::UnknownOrder}});
}

TEST_F(ReferenceBookTest, DigestTracksPartialFillQuantitiesInFifoOrder) {
  book.submit(1, Side::Buy, OrderType::Limit, 100, 10);
  book.submit(2, Side::Buy, OrderType::Limit, 100, 5);
  book.submit(3, Side::Sell, OrderType::Limit, 100, 7);
  listener.clear();

  lob::DigestBuilder expected;
  expected.level(Side::Buy, 100, 2);
  expected.order(1, 3);
  expected.order(2, 5);
  EXPECT_EQ(book.digest(), expected.finish());
}

TEST_F(ReferenceBookTest, DigestMatchesHandBuiltStream) {
  book.submit(1, Side::Buy, OrderType::Limit, 100, 10);
  book.submit(2, Side::Buy, OrderType::Limit, 100, 5);
  book.submit(3, Side::Buy, OrderType::Limit, 99, 7);
  book.submit(4, Side::Sell, OrderType::Limit, 101, 8);
  book.submit(5, Side::Sell, OrderType::Limit, 102, 3);
  listener.clear();

  lob::DigestBuilder expected;
  expected.level(Side::Buy, 100, 2);
  expected.order(1, 10);
  expected.order(2, 5);
  expected.level(Side::Buy, 99, 1);
  expected.order(3, 7);
  expected.level(Side::Sell, 101, 1);
  expected.order(4, 8);
  expected.level(Side::Sell, 102, 1);
  expected.order(5, 3);
  EXPECT_EQ(book.digest(), expected.finish());
}

TEST(ReferenceBookCapacity, MaxOrdersCancelsUnrestableRemainder) {
  constexpr lob::Config config{1, 1000, 2, 100};
  lob::test::RecordingListener listener;
  lob::ref::ReferenceBook book(config, listener);

  book.submit(1, Side::Buy, OrderType::Limit, 100, 10);
  book.submit(2, Side::Buy, OrderType::Limit, 99, 5);
  listener.clear();

  book.submit(3, Side::Buy, OrderType::Limit, 98, 5);
  EXPECT_EQ(listener.events, (std::vector<Event>{lob::test::Accept{3}, lob::test::Cancel{3, 5}}));
  EXPECT_EQ(book.order_count(), 2U);
  EXPECT_EQ(book.level_qty(Side::Buy, 98), 0);
  listener.clear();

  book.submit(4, Side::Sell, OrderType::Limit, 150, 5);
  EXPECT_EQ(listener.events, (std::vector<Event>{lob::test::Accept{4}, lob::test::Cancel{4, 5}}));
  EXPECT_EQ(book.order_count(), 2U);
  listener.clear();

  book.submit(5, Side::Sell, OrderType::Limit, 100, 10);
  EXPECT_EQ(listener.events,
            (std::vector<Event>{lob::test::Accept{5}, lob::test::Trade{1, 5, 100, 10},
                                lob::test::BookUpdate{Side::Buy, 100, 0}}));
  EXPECT_EQ(book.order_count(), 1U);
}

TEST(ReferenceBookCapacity, MaxLevelsIsPerSideAndAllowsExistingLevel) {
  constexpr lob::Config config{1, 1000, 100, 1};
  lob::test::RecordingListener listener;
  lob::ref::ReferenceBook book(config, listener);

  book.submit(1, Side::Buy, OrderType::Limit, 90, 1);
  listener.clear();

  book.submit(2, Side::Buy, OrderType::Limit, 89, 1);
  EXPECT_EQ(listener.events, (std::vector<Event>{lob::test::Accept{2}, lob::test::Cancel{2, 1}}));
  listener.clear();

  book.submit(3, Side::Buy, OrderType::Limit, 90, 2);
  EXPECT_EQ(listener.events,
            (std::vector<Event>{lob::test::Accept{3}, lob::test::BookUpdate{Side::Buy, 90, 3}}));
  listener.clear();

  book.submit(4, Side::Sell, OrderType::Limit, 150, 1);
  EXPECT_EQ(listener.events,
            (std::vector<Event>{lob::test::Accept{4}, lob::test::BookUpdate{Side::Sell, 150, 1}}));
  listener.clear();

  book.submit(5, Side::Sell, OrderType::Limit, 151, 1);
  EXPECT_EQ(listener.events, (std::vector<Event>{lob::test::Accept{5}, lob::test::Cancel{5, 1}}));

  EXPECT_EQ(book.order_count(), 3U);
}

TEST(ReferenceBookCapacity, MaxLevelsCanCancelCrossingRemainder) {
  constexpr lob::Config config{1, 1000, 100, 1};
  lob::test::RecordingListener listener;
  lob::ref::ReferenceBook book(config, listener);

  book.submit(1, Side::Buy, OrderType::Limit, 90, 1);
  book.submit(2, Side::Sell, OrderType::Limit, 100, 1);
  listener.clear();

  book.submit(3, Side::Buy, OrderType::Limit, 200, 3);
  EXPECT_EQ(
      listener.events,
      (std::vector<Event>{lob::test::Accept{3}, lob::test::Trade{2, 3, 100, 1},
                          lob::test::BookUpdate{Side::Sell, 100, 0}, lob::test::Cancel{3, 2}}));

  EXPECT_EQ(book.best_ask(), std::nullopt);
  EXPECT_EQ(book.best_bid(), 90);
  EXPECT_EQ(book.order_count(), 1U);
}

TEST(ReferenceBookCapacity, MaxOrdersOneAllowsTakerButNotRestingOrder) {
  constexpr lob::Config config{1, 1000, 1, 100};
  lob::test::RecordingListener listener;
  lob::ref::ReferenceBook book(config, listener);

  book.submit(1, Side::Buy, OrderType::Limit, 100, 5);
  listener.clear();

  book.submit(2, Side::Buy, OrderType::Limit, 99, 5);
  EXPECT_EQ(listener.events, (std::vector<Event>{lob::test::Accept{2}, lob::test::Cancel{2, 5}}));
  listener.clear();

  book.submit(3, Side::Sell, OrderType::Market, 0, 3);
  EXPECT_EQ(listener.events,
            (std::vector<Event>{lob::test::Accept{3}, lob::test::Trade{1, 3, 100, 3},
                                lob::test::BookUpdate{Side::Buy, 100, 2}}));
  EXPECT_EQ(book.order_count(), 1U);
  listener.clear();

  book.submit(4, Side::Buy, OrderType::Limit, 99, 1);
  EXPECT_EQ(listener.events, (std::vector<Event>{lob::test::Accept{4}, lob::test::Cancel{4, 1}}));
  EXPECT_EQ(book.order_count(), 1U);
  listener.clear();

  book.submit(5, Side::Sell, OrderType::Market, 0, 2);
  EXPECT_EQ(listener.events,
            (std::vector<Event>{lob::test::Accept{5}, lob::test::Trade{1, 5, 100, 2},
                                lob::test::BookUpdate{Side::Buy, 100, 0}}));
  EXPECT_EQ(book.order_count(), 0U);
  listener.clear();

  book.submit(6, Side::Buy, OrderType::Limit, 99, 1);
  EXPECT_EQ(listener.events,
            (std::vector<Event>{lob::test::Accept{6}, lob::test::BookUpdate{Side::Buy, 99, 1}}));
  EXPECT_EQ(book.order_count(), 1U);
}

}  // namespace
