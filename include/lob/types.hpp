#pragma once

#include <cstdint>
#include <string_view>

namespace lob {

using Price = std::int64_t;
using Qty = std::int64_t;
using OrderId = std::uint64_t;

/// Which side of the book an order belongs to. The numeric values are part of
/// the digest and flow-file byte formats and never renumbered.
enum class Side : std::uint8_t { Buy = 0, Sell = 1 };

/// Order type. The numeric values are part of the flow-file byte format and
/// never renumbered. FOK and PostOnly are part of the Phase 2 contract
/// (docs/architecture.md section 4); until the matching semantics land both
/// engines reject them with RejectReason::UnsupportedOrderType.
enum class OrderType : std::uint8_t {
  Limit = 0,
  Market = 1,
  IOC = 2,
  FOK = 3,
  PostOnly = 4,
};

/// Reason attached to a rejection event. Values are stable and appended,
/// never renumbered.
enum class RejectReason : std::uint8_t {
  DuplicateId = 1,
  UnknownOrder,
  InvalidQuantity,
  InvalidPrice,
  CapacityExceeded,
  ModifyNoChange,
  UnsupportedOrderType,
  WouldCross,
};

constexpr Side opposite(Side side) { return side == Side::Buy ? Side::Sell : Side::Buy; }

constexpr std::string_view to_string(Side side) {
  switch (side) {
    case Side::Buy:
      return "buy";
    case Side::Sell:
      return "sell";
  }
  return "unknown";
}

constexpr std::string_view to_string(OrderType type) {
  switch (type) {
    case OrderType::Limit:
      return "limit";
    case OrderType::Market:
      return "market";
    case OrderType::IOC:
      return "ioc";
    case OrderType::FOK:
      return "fok";
    case OrderType::PostOnly:
      return "post_only";
  }
  return "unknown";
}

constexpr std::string_view to_string(RejectReason reason) {
  switch (reason) {
    case RejectReason::DuplicateId:
      return "duplicate_id";
    case RejectReason::UnknownOrder:
      return "unknown_order";
    case RejectReason::InvalidQuantity:
      return "invalid_quantity";
    case RejectReason::InvalidPrice:
      return "invalid_price";
    case RejectReason::CapacityExceeded:
      return "capacity_exceeded";
    case RejectReason::ModifyNoChange:
      return "modify_no_change";
    case RejectReason::UnsupportedOrderType:
      return "unsupported_order_type";
    case RejectReason::WouldCross:
      return "would_cross";
  }
  return "unknown";
}

}  // namespace lob
