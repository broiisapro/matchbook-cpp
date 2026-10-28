#pragma once

// Wire protocol messages for Phase 3 (decision C50).
//
// Every message is a fixed-size little-endian frame: a 16-byte common header
// followed by a fixed body. The authoritative byte layouts are in
// docs/architecture.md §14; this header holds the in-memory form, the type
// table and the compile-time sizes. Encoding and decoding live in codec.hpp
// and never copy a struct as a block of memory.

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "lob/types.hpp"

namespace lob::net {

/// Monotonic per-connection message number carried by every header.
using Seq = std::uint64_t;

/// Header layout: type u16, length u16 (body bytes, header excluded), seq u64,
/// reserved u32 (must be zero).
inline constexpr std::size_t kHeaderSize = 16;
inline constexpr std::size_t kHeaderTypeOffset = 0;
inline constexpr std::size_t kHeaderLengthOffset = 2;
inline constexpr std::size_t kHeaderSeqOffset = 4;
inline constexpr std::size_t kHeaderReservedOffset = 12;

/// Largest body in the table (Fill and Trade), so a frame never exceeds
/// kMaxWireSize bytes and a framer can hold a whole frame in a fixed buffer.
inline constexpr std::size_t kMaxBodySize = 40;
inline constexpr std::size_t kMaxWireSize = kHeaderSize + kMaxBodySize;

/// Wire message ids. Appended only; values are part of the protocol.
enum class MessageType : std::uint16_t {
  NewOrder = 1,
  Cancel = 2,
  Modify = 3,
  Heartbeat = 4,
  Ack = 5,
  Reject = 6,
  Fill = 7,
  Cancelled = 8,
  BookUpdate = 9,
  Trade = 10,
  SequenceReset = 11,
  RecoveryRequest = 12,
};

/// True when id names a message this version understands.
constexpr bool is_known_type(std::uint16_t id) {
  switch (static_cast<MessageType>(id)) {
    case MessageType::NewOrder:
    case MessageType::Cancel:
    case MessageType::Modify:
    case MessageType::Heartbeat:
    case MessageType::Ack:
    case MessageType::Reject:
    case MessageType::Fill:
    case MessageType::Cancelled:
    case MessageType::BookUpdate:
    case MessageType::Trade:
    case MessageType::SequenceReset:
    case MessageType::RecoveryRequest:
      return true;
  }
  return false;
}

/// Body size of a known message, 0 for an unknown id.
constexpr std::size_t body_size(MessageType type) {
  switch (type) {
    case MessageType::NewOrder:
      return 32;
    case MessageType::Cancel:
      return 16;
    case MessageType::Modify:
      return 24;
    case MessageType::Heartbeat:
      return 8;
    case MessageType::Ack:
      return 16;
    case MessageType::Reject:
      return 16;
    case MessageType::Fill:
      return 40;
    case MessageType::Cancelled:
      return 16;
    case MessageType::BookUpdate:
      return 24;
    case MessageType::Trade:
      return 40;
    case MessageType::SequenceReset:
      return 16;
    case MessageType::RecoveryRequest:
      return 16;
  }
  return 0;
}

/// Header plus body, the number of bytes a frame occupies on the wire.
constexpr std::size_t wire_size(MessageType type) { return kHeaderSize + body_size(type); }

/// Name of a message id, "unknown" for ids outside the table.
std::string_view to_string(MessageType type);

// Wire values of the enum fields. The in-memory enums in lob/types.hpp carry
// the same values; FOK and PostOnly are reserved by decision C47 and only gain
// matching behaviour in Epic 06. A decoder accepts every value in the range so
// a later engine can implement them without a protocol change.
inline constexpr std::uint8_t kWireOrderTypeLimit = 0;
inline constexpr std::uint8_t kWireOrderTypeMarket = 1;
inline constexpr std::uint8_t kWireOrderTypeIoc = 2;
inline constexpr std::uint8_t kWireOrderTypeFok = 3;
inline constexpr std::uint8_t kWireOrderTypePostOnly = 4;
inline constexpr std::uint8_t kMaxWireOrderType = kWireOrderTypePostOnly;

inline constexpr std::uint8_t kWireSideBuy = 0;
inline constexpr std::uint8_t kWireSideSell = 1;
inline constexpr std::uint8_t kMaxWireSide = kWireSideSell;

/// Reject reasons are opaque to the gateway: the book owns their meaning. The
/// range covers the values in RejectReason plus UnsupportedOrderType (C47).
inline constexpr std::uint8_t kMinWireRejectReason = 1;
inline constexpr std::uint8_t kMaxWireRejectReason = 7;

static_assert(static_cast<std::uint8_t>(Side::Buy) == kWireSideBuy);
static_assert(static_cast<std::uint8_t>(Side::Sell) == kWireSideSell);
static_assert(static_cast<std::uint8_t>(OrderType::Limit) == kWireOrderTypeLimit);
static_assert(static_cast<std::uint8_t>(OrderType::Market) == kWireOrderTypeMarket);
static_assert(static_cast<std::uint8_t>(OrderType::IOC) == kWireOrderTypeIoc);
static_assert(static_cast<std::uint8_t>(RejectReason::DuplicateId) == 1);
static_assert(static_cast<std::uint8_t>(RejectReason::ModifyNoChange) == 6);
static_assert(kMaxWireRejectReason >= static_cast<std::uint8_t>(RejectReason::ModifyNoChange));

// Client -> gateway: place a new order. price is ignored for a market order
// (callers send 0); qty is in lots.
struct NewOrder {
  static constexpr MessageType kType = MessageType::NewOrder;
  static constexpr std::size_t kBodySize = 32;
  static constexpr std::size_t kWireSize = kHeaderSize + kBodySize;

  static constexpr std::size_t kOrderIdOffset = 0;
  static constexpr std::size_t kPriceOffset = 8;
  static constexpr std::size_t kQtyOffset = 16;
  static constexpr std::size_t kSideOffset = 24;
  static constexpr std::size_t kTypeOffset = 25;
  static constexpr std::size_t kReservedOffset = 26;
  static constexpr std::size_t kReservedSize = 6;

  OrderId order_id = 0;
  Price price = 0;
  Qty qty = 0;
  Side side = Side::Buy;
  OrderType type = OrderType::Limit;

  friend bool operator==(const NewOrder&, const NewOrder&) = default;
};

static_assert(NewOrder::kPriceOffset == NewOrder::kOrderIdOffset + 8);
static_assert(NewOrder::kQtyOffset == NewOrder::kPriceOffset + 8);
static_assert(NewOrder::kSideOffset == NewOrder::kQtyOffset + 8);
static_assert(NewOrder::kTypeOffset == NewOrder::kSideOffset + 1);
static_assert(NewOrder::kReservedOffset == NewOrder::kTypeOffset + 1);
static_assert(NewOrder::kReservedOffset + NewOrder::kReservedSize == NewOrder::kBodySize);

// Client -> gateway: cancel the remainder of a resting order.
struct Cancel {
  static constexpr MessageType kType = MessageType::Cancel;
  static constexpr std::size_t kBodySize = 16;
  static constexpr std::size_t kWireSize = kHeaderSize + kBodySize;

  static constexpr std::size_t kOrderIdOffset = 0;
  static constexpr std::size_t kReservedOffset = 8;
  static constexpr std::size_t kReservedSize = 8;

  OrderId order_id = 0;

  friend bool operator==(const Cancel&, const Cancel&) = default;
};

static_assert(Cancel::kReservedOffset + Cancel::kReservedSize == Cancel::kBodySize);

// Client -> gateway: replace the remaining quantity (and optionally the price)
// of a resting order. qty is the new remaining quantity, not a delta.
struct Modify {
  static constexpr MessageType kType = MessageType::Modify;
  static constexpr std::size_t kBodySize = 24;
  static constexpr std::size_t kWireSize = kHeaderSize + kBodySize;

  static constexpr std::size_t kOrderIdOffset = 0;
  static constexpr std::size_t kPriceOffset = 8;
  static constexpr std::size_t kQtyOffset = 16;

  OrderId order_id = 0;
  Price price = 0;
  Qty qty = 0;

  friend bool operator==(const Modify&, const Modify&) = default;
};

static_assert(Modify::kPriceOffset == Modify::kOrderIdOffset + 8);
static_assert(Modify::kQtyOffset + 8 == Modify::kBodySize);

// Client -> gateway keepalive; the gateway echoes the nonce in its own
// heartbeat so the client can measure round-trip time.
struct Heartbeat {
  static constexpr MessageType kType = MessageType::Heartbeat;
  static constexpr std::size_t kBodySize = 8;
  static constexpr std::size_t kWireSize = kHeaderSize + kBodySize;

  static constexpr std::size_t kNonceOffset = 0;

  std::uint64_t nonce = 0;

  friend bool operator==(const Heartbeat&, const Heartbeat&) = default;
};

static_assert(Heartbeat::kNonceOffset + 8 == Heartbeat::kBodySize);

// Gateway -> client: the order was accepted.
struct Ack {
  static constexpr MessageType kType = MessageType::Ack;
  static constexpr std::size_t kBodySize = 16;
  static constexpr std::size_t kWireSize = kHeaderSize + kBodySize;

  static constexpr std::size_t kOrderIdOffset = 0;
  static constexpr std::size_t kReservedOffset = 8;
  static constexpr std::size_t kReservedSize = 8;

  OrderId order_id = 0;

  friend bool operator==(const Ack&, const Ack&) = default;
};

static_assert(Ack::kReservedOffset + Ack::kReservedSize == Ack::kBodySize);

// Gateway -> client: the order was rejected and never entered the book.
struct Reject {
  static constexpr MessageType kType = MessageType::Reject;
  static constexpr std::size_t kBodySize = 16;
  static constexpr std::size_t kWireSize = kHeaderSize + kBodySize;

  static constexpr std::size_t kOrderIdOffset = 0;
  static constexpr std::size_t kReasonOffset = 8;
  static constexpr std::size_t kReservedOffset = 9;
  static constexpr std::size_t kReservedSize = 7;

  OrderId order_id = 0;
  RejectReason reason = RejectReason::InvalidQuantity;

  friend bool operator==(const Reject&, const Reject&) = default;
};

static_assert(Reject::kReservedOffset + Reject::kReservedSize == Reject::kBodySize);

// Gateway -> client: two orders traded. maker_id rested in the book, taker_id
// was the incoming order, price is the maker's resting price.
struct Fill {
  static constexpr MessageType kType = MessageType::Fill;
  static constexpr std::size_t kBodySize = 40;
  static constexpr std::size_t kWireSize = kHeaderSize + kBodySize;

  static constexpr std::size_t kMakerIdOffset = 0;
  static constexpr std::size_t kTakerIdOffset = 8;
  static constexpr std::size_t kPriceOffset = 16;
  static constexpr std::size_t kQtyOffset = 24;
  static constexpr std::size_t kReservedOffset = 32;
  static constexpr std::size_t kReservedSize = 8;

  OrderId maker_id = 0;
  OrderId taker_id = 0;
  Price price = 0;
  Qty qty = 0;

  friend bool operator==(const Fill&, const Fill&) = default;
};

static_assert(Fill::kTakerIdOffset == Fill::kMakerIdOffset + 8);
static_assert(Fill::kPriceOffset == Fill::kTakerIdOffset + 8);
static_assert(Fill::kQtyOffset == Fill::kPriceOffset + 8);
static_assert(Fill::kReservedOffset + Fill::kReservedSize == Fill::kBodySize);

// Gateway -> client: quantity left the book without trading (an explicit
// cancel, an IOC or market remainder, or a remainder that could not rest).
struct Cancelled {
  static constexpr MessageType kType = MessageType::Cancelled;
  static constexpr std::size_t kBodySize = 16;
  static constexpr std::size_t kWireSize = kHeaderSize + kBodySize;

  static constexpr std::size_t kOrderIdOffset = 0;
  static constexpr std::size_t kQtyOffset = 8;

  OrderId order_id = 0;
  Qty qty = 0;

  friend bool operator==(const Cancelled&, const Cancelled&) = default;
};

static_assert(Cancelled::kQtyOffset + 8 == Cancelled::kBodySize);

// Publisher -> subscriber: the total quantity resting at one price level after
// a change. qty 0 means the level is gone.
struct BookUpdate {
  static constexpr MessageType kType = MessageType::BookUpdate;
  static constexpr std::size_t kBodySize = 24;
  static constexpr std::size_t kWireSize = kHeaderSize + kBodySize;

  static constexpr std::size_t kPriceOffset = 0;
  static constexpr std::size_t kQtyOffset = 8;
  static constexpr std::size_t kSideOffset = 16;
  static constexpr std::size_t kReservedOffset = 17;
  static constexpr std::size_t kReservedSize = 7;

  Price price = 0;
  Qty qty = 0;
  Side side = Side::Buy;

  friend bool operator==(const BookUpdate&, const BookUpdate&) = default;
};

static_assert(BookUpdate::kQtyOffset == BookUpdate::kPriceOffset + 8);
static_assert(BookUpdate::kSideOffset == BookUpdate::kQtyOffset + 8);
static_assert(BookUpdate::kReservedOffset + BookUpdate::kReservedSize == BookUpdate::kBodySize);

// Publisher -> subscriber: a trade, mirroring the Fill body.
struct Trade {
  static constexpr MessageType kType = MessageType::Trade;
  static constexpr std::size_t kBodySize = 40;
  static constexpr std::size_t kWireSize = kHeaderSize + kBodySize;

  static constexpr std::size_t kMakerIdOffset = 0;
  static constexpr std::size_t kTakerIdOffset = 8;
  static constexpr std::size_t kPriceOffset = 16;
  static constexpr std::size_t kQtyOffset = 24;
  static constexpr std::size_t kReservedOffset = 32;
  static constexpr std::size_t kReservedSize = 8;

  OrderId maker_id = 0;
  OrderId taker_id = 0;
  Price price = 0;
  Qty qty = 0;

  friend bool operator==(const Trade&, const Trade&) = default;
};

static_assert(Trade::kReservedOffset + Trade::kReservedSize == Trade::kBodySize);

// Publisher -> subscriber: a sequencing gap was closed by a snapshot or the
// publisher restarted. next_seq is the first sequence number the subscriber
// should expect next.
struct SequenceReset {
  static constexpr MessageType kType = MessageType::SequenceReset;
  static constexpr std::size_t kBodySize = 16;
  static constexpr std::size_t kWireSize = kHeaderSize + kBodySize;

  static constexpr std::size_t kNextSeqOffset = 0;
  static constexpr std::size_t kReservedOffset = 8;
  static constexpr std::size_t kReservedSize = 8;

  Seq next_seq = 0;

  friend bool operator==(const SequenceReset&, const SequenceReset&) = default;
};

static_assert(SequenceReset::kReservedOffset + SequenceReset::kReservedSize ==
              SequenceReset::kBodySize);

// Subscriber -> recovery service: send the messages in [from_seq, to_seq].
struct RecoveryRequest {
  static constexpr MessageType kType = MessageType::RecoveryRequest;
  static constexpr std::size_t kBodySize = 16;
  static constexpr std::size_t kWireSize = kHeaderSize + kBodySize;

  static constexpr std::size_t kFromSeqOffset = 0;
  static constexpr std::size_t kToSeqOffset = 8;

  Seq from_seq = 0;
  Seq to_seq = 0;

  friend bool operator==(const RecoveryRequest&, const RecoveryRequest&) = default;
};

static_assert(RecoveryRequest::kToSeqOffset + 8 == RecoveryRequest::kBodySize);

// The table and the struct constants have to agree; a message whose static
// size drifts from messages::body_size() would desynchronise every framer.
static_assert(body_size(MessageType::NewOrder) == NewOrder::kBodySize);
static_assert(body_size(MessageType::Cancel) == Cancel::kBodySize);
static_assert(body_size(MessageType::Modify) == Modify::kBodySize);
static_assert(body_size(MessageType::Heartbeat) == Heartbeat::kBodySize);
static_assert(body_size(MessageType::Ack) == Ack::kBodySize);
static_assert(body_size(MessageType::Reject) == Reject::kBodySize);
static_assert(body_size(MessageType::Fill) == Fill::kBodySize);
static_assert(body_size(MessageType::Cancelled) == Cancelled::kBodySize);
static_assert(body_size(MessageType::BookUpdate) == BookUpdate::kBodySize);
static_assert(body_size(MessageType::Trade) == Trade::kBodySize);
static_assert(body_size(MessageType::SequenceReset) == SequenceReset::kBodySize);
static_assert(body_size(MessageType::RecoveryRequest) == RecoveryRequest::kBodySize);
static_assert(wire_size(MessageType::Fill) == kMaxWireSize);
static_assert(body_size(MessageType::Fill) == kMaxBodySize);

}  // namespace lob::net
