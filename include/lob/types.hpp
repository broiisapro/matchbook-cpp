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

/// Order type. FOK and PostOnly are appended in Phase 2; values never
/// renumbered.
enum class OrderType : std::uint8_t { Limit = 0, Market = 1, IOC = 2 };

/// Reason attached to a rejection event. Values are stable.
enum class RejectReason : std::uint8_t {
  DuplicateId = 1,
  UnknownOrder,
  InvalidQuantity,
  InvalidPrice,
  CapacityExceeded,
  ModifyNoChange,
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
  }
  return "unknown";
}

}  // namespace lob
