// Enum stability for the Phase 2 order types and reject reasons. The interim
// contract (decision C47) rejected FOK and PostOnly with UnsupportedOrderType
// in both engines; those cross-engine expectations now live with the engine
// that implements each type (tests/test_order_book_types.cpp and
// tests/test_reference_types.cpp), so this file only pins the wire-visible
// enum values and names.

#include <gtest/gtest.h>

#include <cstdint>

#include "lob/types.hpp"

namespace {

using lob::OrderType;
using lob::RejectReason;

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
