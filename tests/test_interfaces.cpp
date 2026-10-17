#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <optional>
#include <type_traits>
#include <vector>

#include "lob/config.hpp"
#include "lob/digest.hpp"
#include "lob/listener.hpp"
#include "lob/order_book.hpp"
#include "lob/types.hpp"
#include "reference/reference_book.hpp"
#include "support/recording_listener.hpp"

namespace {

constexpr lob::Config kConfig{100, 200, 1024, 128};

TEST(Config, ValidCases) {
  EXPECT_TRUE(lob::valid(kConfig));
  EXPECT_TRUE(lob::valid(lob::Config{1, 1, 1, 1}));
  EXPECT_TRUE(lob::valid(lob::Config{100, 100, 7, 3}));
}

TEST(Config, InvalidCases) {
  EXPECT_FALSE(lob::valid(lob::Config{0, 200, 1, 1}));
  EXPECT_FALSE(lob::valid(lob::Config{-5, 200, 1, 1}));
  EXPECT_FALSE(lob::valid(lob::Config{200, 100, 1, 1}));
  EXPECT_FALSE(lob::valid(lob::Config{100, 200, 0, 1}));
  EXPECT_FALSE(lob::valid(lob::Config{100, 200, 1, 0}));
}

TEST(Types, Opposite) {
  EXPECT_EQ(lob::opposite(lob::Side::Buy), lob::Side::Sell);
  EXPECT_EQ(lob::opposite(lob::Side::Sell), lob::Side::Buy);
}

TEST(Types, ToString) {
  EXPECT_EQ(lob::to_string(lob::Side::Buy), "buy");
  EXPECT_EQ(lob::to_string(lob::Side::Sell), "sell");
  EXPECT_EQ(lob::to_string(lob::OrderType::Limit), "limit");
  EXPECT_EQ(lob::to_string(lob::OrderType::Market), "market");
  EXPECT_EQ(lob::to_string(lob::OrderType::IOC), "ioc");
  EXPECT_EQ(lob::to_string(lob::RejectReason::DuplicateId), "duplicate_id");
  EXPECT_EQ(lob::to_string(lob::RejectReason::UnknownOrder), "unknown_order");
  EXPECT_EQ(lob::to_string(lob::RejectReason::InvalidQuantity), "invalid_quantity");
  EXPECT_EQ(lob::to_string(lob::RejectReason::InvalidPrice), "invalid_price");
  EXPECT_EQ(lob::to_string(lob::RejectReason::CapacityExceeded), "capacity_exceeded");
  EXPECT_EQ(lob::to_string(lob::RejectReason::ModifyNoChange), "modify_no_change");
}

// The reference engine mirrors the OrderBook API so the differential harness
// can be a template over the engine type (decision C19). Stripping the class
// from each member pointer leaves plain function types to compare.
template <typename MemberPointer>
struct Signature;
template <typename R, typename C, typename... Args>
struct Signature<R (C::*)(Args...)> {
  using Type = R(Args...);
};
template <typename R, typename C, typename... Args>
struct Signature<R (C::*)(Args...) const> {
  using Type = R(Args...) const;
};

#define LOB_SAME_API(name)                                                      \
  static_assert(                                                                \
      std::is_same_v<typename Signature<decltype(&lob::OrderBook::name)>::Type, \
                     typename Signature<decltype(&lob::ref::ReferenceBook::name)>::Type>)

LOB_SAME_API(submit);
LOB_SAME_API(cancel);
LOB_SAME_API(modify);
LOB_SAME_API(best_bid);
LOB_SAME_API(best_ask);
LOB_SAME_API(level_qty);
LOB_SAME_API(top_levels);
LOB_SAME_API(order_count);
LOB_SAME_API(digest);

#undef LOB_SAME_API

TEST(Interfaces, EnginesAreNonCopyableAndNonMovable) {
  static_assert(!std::is_copy_constructible_v<lob::OrderBook>);
  static_assert(!std::is_copy_assignable_v<lob::OrderBook>);
  static_assert(!std::is_move_constructible_v<lob::OrderBook>);
  static_assert(!std::is_move_assignable_v<lob::OrderBook>);
  static_assert(!std::is_copy_constructible_v<lob::ref::ReferenceBook>);
  static_assert(!std::is_copy_assignable_v<lob::ref::ReferenceBook>);
  static_assert(!std::is_move_constructible_v<lob::ref::ReferenceBook>);
  static_assert(!std::is_move_assignable_v<lob::ref::ReferenceBook>);
}

// Exercises the stub API of either engine: calls are accepted and every query
// reports an empty book.
template <typename Book>
void expect_empty_stub() {
  lob::NullListener listener;
  Book book(kConfig, listener);

  book.submit(1, lob::Side::Buy, lob::OrderType::Limit, 100, 10);
  book.submit(2, lob::Side::Sell, lob::OrderType::Market, 0, 5);
  book.submit(3, lob::Side::Sell, lob::OrderType::IOC, 200, 1);
  book.cancel(4);
  book.modify(5, 101, 7);

  EXPECT_EQ(book.best_bid(), std::nullopt);
  EXPECT_EQ(book.best_ask(), std::nullopt);
  EXPECT_EQ(book.level_qty(lob::Side::Buy, 100), 0);
  EXPECT_EQ(book.level_qty(lob::Side::Sell, 101), 0);
  EXPECT_EQ(book.order_count(), 0U);

  std::array<lob::LevelInfo, 4> levels{};
  EXPECT_EQ(book.top_levels(lob::Side::Buy, levels), 0U);
  EXPECT_EQ(book.top_levels(lob::Side::Sell, levels), 0U);
  EXPECT_EQ(levels[0].price, 0);

  EXPECT_EQ(book.digest(), lob::DigestBuilder{}.finish());
}

TEST(OrderBook, EmptyStubAcceptsCalls) { expect_empty_stub<lob::OrderBook>(); }

TEST(ReferenceBook, EmptyStubAcceptsCalls) { expect_empty_stub<lob::ref::ReferenceBook>(); }

TEST(RecordingListener, RecordsEventsInOrder) {
  lob::test::RecordingListener listener;

  listener.on_accept(7);
  listener.on_reject(8, lob::RejectReason::DuplicateId);
  listener.on_trade(1, 2, 100, 3);
  listener.on_cancel(4, 5);
  listener.on_modify(6, 101, 9);
  listener.on_book_update(lob::Side::Sell, 102, 0);

  const std::vector<lob::test::Event> expected{
      lob::test::Accept{7},           lob::test::Reject{8, lob::RejectReason::DuplicateId},
      lob::test::Trade{1, 2, 100, 3}, lob::test::Cancel{4, 5},
      lob::test::Modify{6, 101, 9},   lob::test::BookUpdate{lob::Side::Sell, 102, 0},
  };
  EXPECT_EQ(listener.events, expected);

  listener.clear();
  EXPECT_TRUE(listener.events.empty());
}

TEST(RecordingListener, EventsAreComparableFieldByField) {
  EXPECT_EQ((lob::test::Accept{7}), (lob::test::Accept{7}));
  EXPECT_NE((lob::test::Accept{7}), (lob::test::Accept{8}));
  EXPECT_EQ((lob::test::Trade{1, 2, 100, 3}), (lob::test::Trade{1, 2, 100, 3}));
  EXPECT_NE((lob::test::Trade{1, 2, 100, 3}), (lob::test::Trade{1, 2, 101, 3}));
  EXPECT_EQ((lob::test::BookUpdate{lob::Side::Buy, 99, 0}),
            (lob::test::BookUpdate{lob::Side::Buy, 99, 0}));
  EXPECT_NE((lob::test::BookUpdate{lob::Side::Buy, 99, 0}),
            (lob::test::BookUpdate{lob::Side::Sell, 99, 0}));
}

}  // namespace
