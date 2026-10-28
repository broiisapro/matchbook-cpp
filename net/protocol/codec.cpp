#include "net/protocol/codec.hpp"

#include <algorithm>

namespace lob::net {
namespace {

// Field-by-field little-endian access. Every value is assembled from bytes so
// the wire format is independent of host byte order, alignment and padding.
void store_u8(std::byte* out, std::uint8_t value) { out[0] = static_cast<std::byte>(value); }

void store_u16(std::byte* out, std::uint16_t value) {
  for (std::size_t i = 0; i < 2; ++i) {
    out[i] = static_cast<std::byte>((value >> (8 * i)) & 0xffU);
  }
}

void store_u32(std::byte* out, std::uint32_t value) {
  for (std::size_t i = 0; i < 4; ++i) {
    out[i] = static_cast<std::byte>((value >> (8 * i)) & 0xffU);
  }
}

void store_u64(std::byte* out, std::uint64_t value) {
  for (std::size_t i = 0; i < 8; ++i) {
    out[i] = static_cast<std::byte>((value >> (8 * i)) & 0xffU);
  }
}

void store_i64(std::byte* out, std::int64_t value) {
  store_u64(out, static_cast<std::uint64_t>(value));
}

std::uint8_t load_u8(const std::byte* in) { return std::to_integer<std::uint8_t>(in[0]); }

std::uint16_t load_u16(const std::byte* in) {
  std::uint16_t value = 0;
  for (std::size_t i = 0; i < 2; ++i) {
    value |= static_cast<std::uint16_t>(std::to_integer<std::uint16_t>(in[i]) << (8 * i));
  }
  return value;
}

std::uint32_t load_u32(const std::byte* in) {
  std::uint32_t value = 0;
  for (std::size_t i = 0; i < 4; ++i) {
    value |= std::to_integer<std::uint32_t>(in[i]) << (8 * i);
  }
  return value;
}

std::uint64_t load_u64(const std::byte* in) {
  std::uint64_t value = 0;
  for (std::size_t i = 0; i < 8; ++i) {
    value |= std::to_integer<std::uint64_t>(in[i]) << (8 * i);
  }
  return value;
}

std::int64_t load_i64(const std::byte* in) { return static_cast<std::int64_t>(load_u64(in)); }

void zero_bytes(std::byte* out, std::size_t count) { std::fill_n(out, count, std::byte{0}); }

bool reserved_zero(const std::byte* in, std::size_t count) {
  return std::all_of(in, in + count, [](std::byte value) { return value == std::byte{0}; });
}

/// Writes the common header and zeroes the whole body, so encoding into a
/// recycled buffer cannot leave stale bytes in a reserved field. Returns the
/// first body byte.
std::byte* begin_frame(std::span<std::byte> out, MessageType type, std::size_t body_size, Seq seq) {
  std::byte* data = out.data();
  store_u16(data + kHeaderTypeOffset, static_cast<std::uint16_t>(type));
  store_u16(data + kHeaderLengthOffset, static_cast<std::uint16_t>(body_size));
  store_u64(data + kHeaderSeqOffset, seq);
  store_u32(data + kHeaderReservedOffset, 0);
  zero_bytes(data + kHeaderSize, body_size);
  return data + kHeaderSize;
}

/// Shared front half of every decoder: checks the frame is fully present, that
/// its header is valid, and that the type matches the requested message.
DecodeStatus check_frame(std::span<const std::byte> in, MessageType type, std::size_t body_size,
                         Seq& seq) {
  if (in.size() < kHeaderSize + body_size) {
    return DecodeStatus::Truncated;
  }
  Header header;
  const DecodeStatus status = decode_header(in.first(kHeaderSize), header);
  if (status != DecodeStatus::Ok) {
    return status;
  }
  if (header.type != type) {
    return DecodeStatus::WrongType;
  }
  seq = header.seq;
  return DecodeStatus::Ok;
}

}  // namespace

std::string_view to_string(MessageType type) {
  switch (type) {
    case MessageType::NewOrder:
      return "new_order";
    case MessageType::Cancel:
      return "cancel";
    case MessageType::Modify:
      return "modify";
    case MessageType::Heartbeat:
      return "heartbeat";
    case MessageType::Ack:
      return "ack";
    case MessageType::Reject:
      return "reject";
    case MessageType::Fill:
      return "fill";
    case MessageType::Cancelled:
      return "cancelled";
    case MessageType::BookUpdate:
      return "book_update";
    case MessageType::Trade:
      return "trade";
    case MessageType::SequenceReset:
      return "sequence_reset";
    case MessageType::RecoveryRequest:
      return "recovery_request";
  }
  return "unknown";
}

std::string_view to_string(DecodeStatus status) {
  switch (status) {
    case DecodeStatus::Ok:
      return "ok";
    case DecodeStatus::Truncated:
      return "truncated";
    case DecodeStatus::WrongType:
      return "wrong_type";
    case DecodeStatus::UnknownType:
      return "unknown_type";
    case DecodeStatus::BadLength:
      return "bad_length";
    case DecodeStatus::Oversize:
      return "oversize";
    case DecodeStatus::BadReserved:
      return "bad_reserved";
    case DecodeStatus::InvalidField:
      return "invalid_field";
  }
  return "unknown";
}

DecodeStatus decode_header(std::span<const std::byte> in, Header& out) {
  if (in.size() < kHeaderSize) {
    return DecodeStatus::Truncated;
  }
  const std::byte* data = in.data();
  if (load_u32(data + kHeaderReservedOffset) != 0) {
    return DecodeStatus::BadReserved;
  }
  const std::uint16_t length = load_u16(data + kHeaderLengthOffset);
  if (length > kMaxBodySize) {
    return DecodeStatus::Oversize;
  }
  const std::uint16_t id = load_u16(data + kHeaderTypeOffset);
  if (!is_known_type(id)) {
    return DecodeStatus::UnknownType;
  }
  const MessageType type = static_cast<MessageType>(id);
  if (length != body_size(type)) {
    return DecodeStatus::BadLength;
  }
  out = Header{type, length, load_u64(data + kHeaderSeqOffset)};
  return DecodeStatus::Ok;
}

std::size_t encode(const NewOrder& message, Seq seq, std::span<std::byte> out) {
  if (out.size() < NewOrder::kWireSize) {
    return 0;
  }
  std::byte* body = begin_frame(out, NewOrder::kType, NewOrder::kBodySize, seq);
  store_u64(body + NewOrder::kOrderIdOffset, message.order_id);
  store_i64(body + NewOrder::kPriceOffset, message.price);
  store_i64(body + NewOrder::kQtyOffset, message.qty);
  store_u8(body + NewOrder::kSideOffset, static_cast<std::uint8_t>(message.side));
  store_u8(body + NewOrder::kTypeOffset, static_cast<std::uint8_t>(message.type));
  return NewOrder::kWireSize;
}

std::size_t encode(const Cancel& message, Seq seq, std::span<std::byte> out) {
  if (out.size() < Cancel::kWireSize) {
    return 0;
  }
  std::byte* body = begin_frame(out, Cancel::kType, Cancel::kBodySize, seq);
  store_u64(body + Cancel::kOrderIdOffset, message.order_id);
  return Cancel::kWireSize;
}

std::size_t encode(const Modify& message, Seq seq, std::span<std::byte> out) {
  if (out.size() < Modify::kWireSize) {
    return 0;
  }
  std::byte* body = begin_frame(out, Modify::kType, Modify::kBodySize, seq);
  store_u64(body + Modify::kOrderIdOffset, message.order_id);
  store_i64(body + Modify::kPriceOffset, message.price);
  store_i64(body + Modify::kQtyOffset, message.qty);
  return Modify::kWireSize;
}

std::size_t encode(const Heartbeat& message, Seq seq, std::span<std::byte> out) {
  if (out.size() < Heartbeat::kWireSize) {
    return 0;
  }
  std::byte* body = begin_frame(out, Heartbeat::kType, Heartbeat::kBodySize, seq);
  store_u64(body + Heartbeat::kNonceOffset, message.nonce);
  return Heartbeat::kWireSize;
}

std::size_t encode(const Ack& message, Seq seq, std::span<std::byte> out) {
  if (out.size() < Ack::kWireSize) {
    return 0;
  }
  std::byte* body = begin_frame(out, Ack::kType, Ack::kBodySize, seq);
  store_u64(body + Ack::kOrderIdOffset, message.order_id);
  return Ack::kWireSize;
}

std::size_t encode(const Reject& message, Seq seq, std::span<std::byte> out) {
  if (out.size() < Reject::kWireSize) {
    return 0;
  }
  std::byte* body = begin_frame(out, Reject::kType, Reject::kBodySize, seq);
  store_u64(body + Reject::kOrderIdOffset, message.order_id);
  store_u8(body + Reject::kReasonOffset, static_cast<std::uint8_t>(message.reason));
  return Reject::kWireSize;
}

std::size_t encode(const Fill& message, Seq seq, std::span<std::byte> out) {
  if (out.size() < Fill::kWireSize) {
    return 0;
  }
  std::byte* body = begin_frame(out, Fill::kType, Fill::kBodySize, seq);
  store_u64(body + Fill::kMakerIdOffset, message.maker_id);
  store_u64(body + Fill::kTakerIdOffset, message.taker_id);
  store_i64(body + Fill::kPriceOffset, message.price);
  store_i64(body + Fill::kQtyOffset, message.qty);
  return Fill::kWireSize;
}

std::size_t encode(const Cancelled& message, Seq seq, std::span<std::byte> out) {
  if (out.size() < Cancelled::kWireSize) {
    return 0;
  }
  std::byte* body = begin_frame(out, Cancelled::kType, Cancelled::kBodySize, seq);
  store_u64(body + Cancelled::kOrderIdOffset, message.order_id);
  store_i64(body + Cancelled::kQtyOffset, message.qty);
  return Cancelled::kWireSize;
}

std::size_t encode(const BookUpdate& message, Seq seq, std::span<std::byte> out) {
  if (out.size() < BookUpdate::kWireSize) {
    return 0;
  }
  std::byte* body = begin_frame(out, BookUpdate::kType, BookUpdate::kBodySize, seq);
  store_i64(body + BookUpdate::kPriceOffset, message.price);
  store_i64(body + BookUpdate::kQtyOffset, message.qty);
  store_u8(body + BookUpdate::kSideOffset, static_cast<std::uint8_t>(message.side));
  return BookUpdate::kWireSize;
}

std::size_t encode(const Trade& message, Seq seq, std::span<std::byte> out) {
  if (out.size() < Trade::kWireSize) {
    return 0;
  }
  std::byte* body = begin_frame(out, Trade::kType, Trade::kBodySize, seq);
  store_u64(body + Trade::kMakerIdOffset, message.maker_id);
  store_u64(body + Trade::kTakerIdOffset, message.taker_id);
  store_i64(body + Trade::kPriceOffset, message.price);
  store_i64(body + Trade::kQtyOffset, message.qty);
  return Trade::kWireSize;
}

std::size_t encode(const SequenceReset& message, Seq seq, std::span<std::byte> out) {
  if (out.size() < SequenceReset::kWireSize) {
    return 0;
  }
  std::byte* body = begin_frame(out, SequenceReset::kType, SequenceReset::kBodySize, seq);
  store_u64(body + SequenceReset::kNextSeqOffset, message.next_seq);
  return SequenceReset::kWireSize;
}

std::size_t encode(const RecoveryRequest& message, Seq seq, std::span<std::byte> out) {
  if (out.size() < RecoveryRequest::kWireSize) {
    return 0;
  }
  std::byte* body = begin_frame(out, RecoveryRequest::kType, RecoveryRequest::kBodySize, seq);
  store_u64(body + RecoveryRequest::kFromSeqOffset, message.from_seq);
  store_u64(body + RecoveryRequest::kToSeqOffset, message.to_seq);
  return RecoveryRequest::kWireSize;
}

DecodeStatus decode(std::span<const std::byte> in, NewOrder& out, Seq& seq) {
  Seq header_seq = 0;
  const DecodeStatus status = check_frame(in, NewOrder::kType, NewOrder::kBodySize, header_seq);
  if (status != DecodeStatus::Ok) {
    return status;
  }
  const std::byte* body = in.data() + kHeaderSize;
  if (!reserved_zero(body + NewOrder::kReservedOffset, NewOrder::kReservedSize)) {
    return DecodeStatus::BadReserved;
  }
  const std::uint8_t side = load_u8(body + NewOrder::kSideOffset);
  const std::uint8_t type = load_u8(body + NewOrder::kTypeOffset);
  if (side > kMaxWireSide || type > kMaxWireOrderType) {
    return DecodeStatus::InvalidField;
  }
  out = NewOrder{load_u64(body + NewOrder::kOrderIdOffset), load_i64(body + NewOrder::kPriceOffset),
                 load_i64(body + NewOrder::kQtyOffset), static_cast<Side>(side),
                 static_cast<OrderType>(type)};
  seq = header_seq;
  return DecodeStatus::Ok;
}

DecodeStatus decode(std::span<const std::byte> in, Cancel& out, Seq& seq) {
  Seq header_seq = 0;
  const DecodeStatus status = check_frame(in, Cancel::kType, Cancel::kBodySize, header_seq);
  if (status != DecodeStatus::Ok) {
    return status;
  }
  const std::byte* body = in.data() + kHeaderSize;
  if (!reserved_zero(body + Cancel::kReservedOffset, Cancel::kReservedSize)) {
    return DecodeStatus::BadReserved;
  }
  out = Cancel{load_u64(body + Cancel::kOrderIdOffset)};
  seq = header_seq;
  return DecodeStatus::Ok;
}

DecodeStatus decode(std::span<const std::byte> in, Modify& out, Seq& seq) {
  Seq header_seq = 0;
  const DecodeStatus status = check_frame(in, Modify::kType, Modify::kBodySize, header_seq);
  if (status != DecodeStatus::Ok) {
    return status;
  }
  const std::byte* body = in.data() + kHeaderSize;
  out = Modify{load_u64(body + Modify::kOrderIdOffset), load_i64(body + Modify::kPriceOffset),
               load_i64(body + Modify::kQtyOffset)};
  seq = header_seq;
  return DecodeStatus::Ok;
}

DecodeStatus decode(std::span<const std::byte> in, Heartbeat& out, Seq& seq) {
  Seq header_seq = 0;
  const DecodeStatus status = check_frame(in, Heartbeat::kType, Heartbeat::kBodySize, header_seq);
  if (status != DecodeStatus::Ok) {
    return status;
  }
  const std::byte* body = in.data() + kHeaderSize;
  out = Heartbeat{load_u64(body + Heartbeat::kNonceOffset)};
  seq = header_seq;
  return DecodeStatus::Ok;
}

DecodeStatus decode(std::span<const std::byte> in, Ack& out, Seq& seq) {
  Seq header_seq = 0;
  const DecodeStatus status = check_frame(in, Ack::kType, Ack::kBodySize, header_seq);
  if (status != DecodeStatus::Ok) {
    return status;
  }
  const std::byte* body = in.data() + kHeaderSize;
  if (!reserved_zero(body + Ack::kReservedOffset, Ack::kReservedSize)) {
    return DecodeStatus::BadReserved;
  }
  out = Ack{load_u64(body + Ack::kOrderIdOffset)};
  seq = header_seq;
  return DecodeStatus::Ok;
}

DecodeStatus decode(std::span<const std::byte> in, Reject& out, Seq& seq) {
  Seq header_seq = 0;
  const DecodeStatus status = check_frame(in, Reject::kType, Reject::kBodySize, header_seq);
  if (status != DecodeStatus::Ok) {
    return status;
  }
  const std::byte* body = in.data() + kHeaderSize;
  if (!reserved_zero(body + Reject::kReservedOffset, Reject::kReservedSize)) {
    return DecodeStatus::BadReserved;
  }
  const std::uint8_t reason = load_u8(body + Reject::kReasonOffset);
  if (reason < kMinWireRejectReason || reason > kMaxWireRejectReason) {
    return DecodeStatus::InvalidField;
  }
  out = Reject{load_u64(body + Reject::kOrderIdOffset), static_cast<RejectReason>(reason)};
  seq = header_seq;
  return DecodeStatus::Ok;
}

DecodeStatus decode(std::span<const std::byte> in, Fill& out, Seq& seq) {
  Seq header_seq = 0;
  const DecodeStatus status = check_frame(in, Fill::kType, Fill::kBodySize, header_seq);
  if (status != DecodeStatus::Ok) {
    return status;
  }
  const std::byte* body = in.data() + kHeaderSize;
  if (!reserved_zero(body + Fill::kReservedOffset, Fill::kReservedSize)) {
    return DecodeStatus::BadReserved;
  }
  out = Fill{load_u64(body + Fill::kMakerIdOffset), load_u64(body + Fill::kTakerIdOffset),
             load_i64(body + Fill::kPriceOffset), load_i64(body + Fill::kQtyOffset)};
  seq = header_seq;
  return DecodeStatus::Ok;
}

DecodeStatus decode(std::span<const std::byte> in, Cancelled& out, Seq& seq) {
  Seq header_seq = 0;
  const DecodeStatus status = check_frame(in, Cancelled::kType, Cancelled::kBodySize, header_seq);
  if (status != DecodeStatus::Ok) {
    return status;
  }
  const std::byte* body = in.data() + kHeaderSize;
  out =
      Cancelled{load_u64(body + Cancelled::kOrderIdOffset), load_i64(body + Cancelled::kQtyOffset)};
  seq = header_seq;
  return DecodeStatus::Ok;
}

DecodeStatus decode(std::span<const std::byte> in, BookUpdate& out, Seq& seq) {
  Seq header_seq = 0;
  const DecodeStatus status = check_frame(in, BookUpdate::kType, BookUpdate::kBodySize, header_seq);
  if (status != DecodeStatus::Ok) {
    return status;
  }
  const std::byte* body = in.data() + kHeaderSize;
  if (!reserved_zero(body + BookUpdate::kReservedOffset, BookUpdate::kReservedSize)) {
    return DecodeStatus::BadReserved;
  }
  const std::uint8_t side = load_u8(body + BookUpdate::kSideOffset);
  if (side > kMaxWireSide) {
    return DecodeStatus::InvalidField;
  }
  out = BookUpdate{load_i64(body + BookUpdate::kPriceOffset),
                   load_i64(body + BookUpdate::kQtyOffset), static_cast<Side>(side)};
  seq = header_seq;
  return DecodeStatus::Ok;
}

DecodeStatus decode(std::span<const std::byte> in, Trade& out, Seq& seq) {
  Seq header_seq = 0;
  const DecodeStatus status = check_frame(in, Trade::kType, Trade::kBodySize, header_seq);
  if (status != DecodeStatus::Ok) {
    return status;
  }
  const std::byte* body = in.data() + kHeaderSize;
  if (!reserved_zero(body + Trade::kReservedOffset, Trade::kReservedSize)) {
    return DecodeStatus::BadReserved;
  }
  out = Trade{load_u64(body + Trade::kMakerIdOffset), load_u64(body + Trade::kTakerIdOffset),
              load_i64(body + Trade::kPriceOffset), load_i64(body + Trade::kQtyOffset)};
  seq = header_seq;
  return DecodeStatus::Ok;
}

DecodeStatus decode(std::span<const std::byte> in, SequenceReset& out, Seq& seq) {
  Seq header_seq = 0;
  const DecodeStatus status =
      check_frame(in, SequenceReset::kType, SequenceReset::kBodySize, header_seq);
  if (status != DecodeStatus::Ok) {
    return status;
  }
  const std::byte* body = in.data() + kHeaderSize;
  if (!reserved_zero(body + SequenceReset::kReservedOffset, SequenceReset::kReservedSize)) {
    return DecodeStatus::BadReserved;
  }
  out = SequenceReset{load_u64(body + SequenceReset::kNextSeqOffset)};
  seq = header_seq;
  return DecodeStatus::Ok;
}

DecodeStatus decode(std::span<const std::byte> in, RecoveryRequest& out, Seq& seq) {
  Seq header_seq = 0;
  const DecodeStatus status =
      check_frame(in, RecoveryRequest::kType, RecoveryRequest::kBodySize, header_seq);
  if (status != DecodeStatus::Ok) {
    return status;
  }
  const std::byte* body = in.data() + kHeaderSize;
  out = RecoveryRequest{load_u64(body + RecoveryRequest::kFromSeqOffset),
                        load_u64(body + RecoveryRequest::kToSeqOffset)};
  seq = header_seq;
  return DecodeStatus::Ok;
}

}  // namespace lob::net
