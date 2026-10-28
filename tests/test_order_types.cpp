// Interim FOK / PostOnly contract (decision C47): the enum values and the
// reject reasons are frozen now, and both engines reject the two types at
// validation step 0 with UnsupportedOrderType until the Phase 2 matching
// semantics land. Expected event vectors are written by hand from section 4 of
// docs/architecture.md, never captured from the engines.
//
// The contract tests run once per engine through a typed test suite, so the
// fast engine and the reference engine are held to the same expectations.

#include <gtest/gtest.h>

#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string_view>
#include <vector>

#include "lob/config.hpp"
#include "lob/order_book.hpp"
#include "lob/types.hpp"
#include "reference/reference_book.hpp"
#include "support/recording_listener.hpp"

namespace {

using lob::Config;
using lob::OrderBook;
using lob::OrderType;
using lob::RejectReason;
using lob::Side;
using lob::test::Accept;
using lob::test::BookUpdate;
using lob::test::Event;
using lob::test::Reject;

constexpr Config kConfig{1, 1000, 64, 16};

/// Both engines take the same Config and listener, so one template covers the
/// shared contract.
template <class Engine>
class OrderTypeContractTest : public ::testing::Test {
 protected:
  void expect_events(std::initializer_list<Event> expected) {
    EXPECT_EQ(listener.events, std::vector<Event>(expected));
    listener.clear();
  }

  lob::test::RecordingListener listener;
  Engine book{kConfig, listener};
};

using EngineTypes = ::testing::Types<OrderBook, lob::ref::ReferenceBook>;
TYPED_TEST_SUITE(OrderTypeContractTest, EngineTypes);

TYPED_TEST(OrderTypeContractTest, FokIsRejectedAtValidationStepZero) {
  this->book.submit(1, Side::Buy, OrderType::FOK, 100, 5);

  this->expect_events({Reject{1, RejectReason::UnsupportedOrderType}});
  EXPECT_EQ(this->book.best_bid(), std::nullopt);
  EXPECT_EQ(this->book.best_ask(), std::nullopt);
  EXPECT_EQ(this->book.order_count(), 0U);
}

TYPED_TEST(OrderTypeContractTest, PostOnlyIsRejectedAtValidationStepZero) {
  this->book.submit(1, Side::Sell, OrderType::PostOnly, 100, 5);

  this->expect_events({Reject{1, RejectReason::UnsupportedOrderType}});
  EXPECT_EQ(this->book.best_bid(), std::nullopt);
  EXPECT_EQ(this->book.best_ask(), std::nullopt);
  EXPECT_EQ(this->book.order_count(), 0U);
}

// Step 0 beats every other validation step: the reason is always
// UnsupportedOrderType, never InvalidQuantity, InvalidPrice or DuplicateId.
TYPED_TEST(OrderTypeContractTest, UnsupportedTypePrecedesTheOtherValidationChecks) {
  this->book.submit(1, Side::Sell, OrderType::Limit, 100, 5);
  this->expect_events({Accept{1}, BookUpdate{Side::Sell, 100, 5}});

  // Zero and negative quantities would be InvalidQuantity for a limit order.
  this->book.submit(2, Side::Buy, OrderType::FOK, 100, 0);
  this->expect_events({Reject{2, RejectReason::UnsupportedOrderType}});
  this->book.submit(2, Side::Buy, OrderType::PostOnly, 100, -3);
  this->expect_events({Reject{2, RejectReason::UnsupportedOrderType}});

  // Prices outside [min_price, max_price] would be InvalidPrice.
  this->book.submit(2, Side::Buy, OrderType::FOK, kConfig.min_price - 1, 5);
  this->expect_events({Reject{2, RejectReason::UnsupportedOrderType}});
  this->book.submit(2, Side::Buy, OrderType::PostOnly, kConfig.max_price + 1, 5);
  this->expect_events({Reject{2, RejectReason::UnsupportedOrderType}});

  // A live id would be DuplicateId.
  this->book.submit(1, Side::Buy, OrderType::FOK, 100, 5);
  this->expect_events({Reject{1, RejectReason::UnsupportedOrderType}});
}

// A type whose semantics are not implemented never trades: a crossing FOK is
// not a fill, and a crossing PostOnly is not WouldCross.
TYPED_TEST(OrderTypeContractTest, UnsupportedTypeNeverTradesAndLeavesTheBookUntouched) {
  this->book.submit(1, Side::Sell, OrderType::Limit, 100, 5);
  this->expect_events({Accept{1}, BookUpdate{Side::Sell, 100, 5}});
  const std::uint64_t digest_before = this->book.digest();

  this->book.submit(2, Side::Buy, OrderType::FOK, 100, 5);
  this->expect_events({Reject{2, RejectReason::UnsupportedOrderType}});

  this->book.submit(2, Side::Buy, OrderType::PostOnly, 100, 5);
  this->expect_events({Reject{2, RejectReason::UnsupportedOrderType}});

  this->book.submit(3, Side::Buy, OrderType::FOK, 1000, 5);
  this->expect_events({Reject{3, RejectReason::UnsupportedOrderType}});

  EXPECT_EQ(this->book.digest(), digest_before);
  EXPECT_EQ(this->book.best_ask(), 100);
  EXPECT_EQ(this->book.level_qty(Side::Sell, 100), 5);
  EXPECT_EQ(this->book.order_count(), 1U);
}

// The added enum values are appended and the existing ones keep their numbers:
// OrderType is part of the flow-file byte format and RejectReason must stay
// stable for consumers that log it.
TEST(OrderTypes, EnumValuesAreAppendedNotRenumbered) {
  EXPECT_EQ(static_cast<int>(OrderType::Limit), 0);
  EXPECT_EQ(static_cast<int>(OrderType::Market), 1);
  EXPECT_EQ(static_cast<int>(OrderType::IOC), 2);
  EXPECT_EQ(static_cast<int>(OrderType::FOK), 3);
  EXPECT_EQ(static_cast<int>(OrderType::PostOnly), 4);

  EXPECT_EQ(static_cast<int>(RejectReason::DuplicateId), 1);
  EXPECT_EQ(static_cast<int>(RejectReason::UnknownOrder), 2);
  EXPECT_EQ(static_cast<int>(RejectReason::InvalidQuantity), 3);
  EXPECT_EQ(static_cast<int>(RejectReason::InvalidPrice), 4);
  EXPECT_EQ(static_cast<int>(RejectReason::CapacityExceeded), 5);
  EXPECT_EQ(static_cast<int>(RejectReason::ModifyNoChange), 6);
  EXPECT_EQ(static_cast<int>(RejectReason::UnsupportedOrderType), 7);
  EXPECT_EQ(static_cast<int>(RejectReason::WouldCross), 8);
}

TEST(OrderTypes, ToStringNamesTheNewValues) {
  EXPECT_EQ(lob::to_string(OrderType::FOK), "fok");
  EXPECT_EQ(lob::to_string(OrderType::PostOnly), "post_only");
  EXPECT_EQ(lob::to_string(RejectReason::UnsupportedOrderType), "unsupported_order_type");
  EXPECT_EQ(lob::to_string(RejectReason::WouldCross), "would_cross");
}

}  // namespace
