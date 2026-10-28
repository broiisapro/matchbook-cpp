#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <random>
#include <span>

#include "net/protocol/codec.hpp"
#include "net/protocol/messages.hpp"

namespace {

namespace proto = lob::net;

constexpr std::size_t kHeader = proto::kHeaderSize;

// The table is the contract the framer and every peer depend on: type ids and
// body sizes are frozen, and each struct's static constants must agree with
// messages::body_size().
static_assert(static_cast<std::uint16_t>(proto::MessageType::NewOrder) == 1);
static_assert(static_cast<std::uint16_t>(proto::MessageType::Cancel) == 2);
static_assert(static_cast<std::uint16_t>(proto::MessageType::Modify) == 3);
static_assert(static_cast<std::uint16_t>(proto::MessageType::Heartbeat) == 4);
static_assert(static_cast<std::uint16_t>(proto::MessageType::Ack) == 5);
static_assert(static_cast<std::uint16_t>(proto::MessageType::Reject) == 6);
static_assert(static_cast<std::uint16_t>(proto::MessageType::Fill) == 7);
static_assert(static_cast<std::uint16_t>(proto::MessageType::Cancelled) == 8);
static_assert(static_cast<std::uint16_t>(proto::MessageType::BookUpdate) == 9);
static_assert(static_cast<std::uint16_t>(proto::MessageType::Trade) == 10);
static_assert(static_cast<std::uint16_t>(proto::MessageType::SequenceReset) == 11);
static_assert(static_cast<std::uint16_t>(proto::MessageType::RecoveryRequest) == 12);
static_assert(proto::NewOrder::kWireSize == 48);
static_assert(proto::Cancel::kWireSize == 32);
static_assert(proto::Modify::kWireSize == 40);
static_assert(proto::Heartbeat::kWireSize == 24);
static_assert(proto::Ack::kWireSize == 32);
static_assert(proto::Reject::kWireSize == 32);
static_assert(proto::Fill::kWireSize == 56);
static_assert(proto::Cancelled::kWireSize == 32);
static_assert(proto::BookUpdate::kWireSize == 40);
static_assert(proto::Trade::kWireSize == 56);
static_assert(proto::SequenceReset::kWireSize == 32);
static_assert(proto::RecoveryRequest::kWireSize == 32);

template <class Message>
std::array<std::byte, Message::kWireSize> encoded(const Message& message, proto::Seq seq) {
  std::array<std::byte, Message::kWireSize> bytes{};
  EXPECT_EQ(proto::encode(message, seq, bytes), bytes.size());
  return bytes;
}

template <class Message>
void expect_round_trip(const Message& message, proto::Seq seq) {
  const std::array<std::byte, Message::kWireSize> bytes = encoded(message, seq);
  Message decoded{};
  proto::Seq decoded_seq = 0;
  ASSERT_EQ(proto::decode(std::span<const std::byte>(bytes), decoded, decoded_seq),
            proto::DecodeStatus::Ok);
  EXPECT_EQ(decoded, message);
  EXPECT_EQ(decoded_seq, seq);

  // Encoding what was decoded reproduces the same bytes, so the codec is its
  // own inverse for every field value.
  EXPECT_EQ(encoded(decoded, decoded_seq), bytes);

  // Extra bytes after the frame (the next frame on a stream) are not read.
  std::array<std::byte, Message::kWireSize + 3> longer{};
  std::copy(bytes.begin(), bytes.end(), longer.begin());
  Message from_longer{};
  proto::Seq longer_seq = 0;
  ASSERT_EQ(proto::decode(std::span<const std::byte>(longer), from_longer, longer_seq),
            proto::DecodeStatus::Ok);
  EXPECT_EQ(from_longer, message);
  EXPECT_EQ(longer_seq, seq);
}

void set_u16(std::span<std::byte> bytes, std::size_t offset, std::uint16_t value) {
  bytes[offset] = static_cast<std::byte>(value & 0xffU);
  bytes[offset + 1] = static_cast<std::byte>((value >> 8) & 0xffU);
}

template <class Message>
void expect_nonzero_reserved_rejected(const Message& message, std::size_t offset,
                                      std::size_t size) {
  const auto clean = encoded(message, 9);
  for (std::size_t i = 0; i < size; ++i) {
    auto mutated = clean;
    mutated[kHeader + offset + i] = std::byte{0x80};
    Message decoded{};
    proto::Seq seq = 0;
    EXPECT_EQ(proto::decode(std::span<const std::byte>(mutated), decoded, seq),
              proto::DecodeStatus::BadReserved)
        << "reserved body byte " << i << " at offset " << offset << " must be zero";
  }
}

TEST(ProtocolMessages, TypeTableCoversEveryMessage) {
  EXPECT_EQ(proto::body_size(proto::MessageType::NewOrder), proto::NewOrder::kBodySize);
  EXPECT_EQ(proto::body_size(proto::MessageType::Cancel), proto::Cancel::kBodySize);
  EXPECT_EQ(proto::body_size(proto::MessageType::Modify), proto::Modify::kBodySize);
  EXPECT_EQ(proto::body_size(proto::MessageType::Heartbeat), proto::Heartbeat::kBodySize);
  EXPECT_EQ(proto::body_size(proto::MessageType::Ack), proto::Ack::kBodySize);
  EXPECT_EQ(proto::body_size(proto::MessageType::Reject), proto::Reject::kBodySize);
  EXPECT_EQ(proto::body_size(proto::MessageType::Fill), proto::Fill::kBodySize);
  EXPECT_EQ(proto::body_size(proto::MessageType::Cancelled), proto::Cancelled::kBodySize);
  EXPECT_EQ(proto::body_size(proto::MessageType::BookUpdate), proto::BookUpdate::kBodySize);
  EXPECT_EQ(proto::body_size(proto::MessageType::Trade), proto::Trade::kBodySize);
  EXPECT_EQ(proto::body_size(proto::MessageType::SequenceReset), proto::SequenceReset::kBodySize);
  EXPECT_EQ(proto::body_size(proto::MessageType::RecoveryRequest),
            proto::RecoveryRequest::kBodySize);

  EXPECT_EQ(proto::wire_size(proto::MessageType::NewOrder), 48U);
  EXPECT_EQ(proto::wire_size(proto::MessageType::Fill), proto::kMaxWireSize);

  for (std::uint16_t id = 1; id <= 12; ++id) {
    EXPECT_TRUE(proto::is_known_type(id)) << "type id " << id;
    EXPECT_NE(proto::to_string(static_cast<proto::MessageType>(id)), "unknown");
  }
  EXPECT_FALSE(proto::is_known_type(0));
  EXPECT_FALSE(proto::is_known_type(13));
  EXPECT_FALSE(proto::is_known_type(0xffff));
  EXPECT_EQ(proto::to_string(proto::MessageType::BookUpdate), "book_update");
  EXPECT_EQ(proto::to_string(static_cast<proto::MessageType>(99)), "unknown");
}

TEST(ProtocolMessages, RoundTripsEveryMessage) {
  constexpr proto::Seq kSeq = 0x0102030405060708ULL;

  expect_round_trip(
      proto::NewOrder{0x1122334455667788ULL, 100, 25, lob::Side::Buy, lob::OrderType::Limit}, kSeq);
  expect_round_trip(proto::NewOrder{2, 0, 7, lob::Side::Sell, lob::OrderType::Market}, kSeq + 1);
  expect_round_trip(proto::Cancel{0xAABBCCDDEEFF0011ULL}, kSeq + 2);
  expect_round_trip(proto::Modify{7, 12345, 3}, kSeq + 3);
  expect_round_trip(proto::Heartbeat{0xDEADBEEFCAFEF00DULL}, kSeq + 4);
  expect_round_trip(proto::Ack{42}, kSeq + 5);
  expect_round_trip(proto::Reject{42, lob::RejectReason::UnknownOrder}, kSeq + 6);
  expect_round_trip(proto::Fill{1, 2, 999, 5}, kSeq + 7);
  expect_round_trip(proto::Cancelled{1, 5}, kSeq + 8);
  expect_round_trip(proto::BookUpdate{100, 0, lob::Side::Sell}, kSeq + 9);
  expect_round_trip(proto::Trade{1, 2, 100, 5}, kSeq + 10);
  expect_round_trip(proto::SequenceReset{0xFEEDFACECAFEBEEFULL}, kSeq + 11);
  expect_round_trip(proto::RecoveryRequest{10, 20}, kSeq + 12);
}

TEST(ProtocolMessages, RoundTripsExtremeValues) {
  constexpr auto kMaxU64 = std::numeric_limits<std::uint64_t>::max();
  constexpr auto kMinI64 = std::numeric_limits<std::int64_t>::min();
  constexpr auto kMaxI64 = std::numeric_limits<std::int64_t>::max();

  expect_round_trip(
      proto::NewOrder{kMaxU64, kMinI64, kMaxI64, lob::Side::Sell, lob::OrderType::IOC}, kMaxU64);
  expect_round_trip(proto::Fill{kMaxU64, kMaxU64 - 1, kMinI64, kMaxI64}, 0);
  expect_round_trip(proto::BookUpdate{kMinI64, kMinI64, lob::Side::Buy}, 0);
  expect_round_trip(proto::RecoveryRequest{kMaxU64, kMaxU64}, 0);
  expect_round_trip(proto::Modify{0, 0, 0}, 0);
  expect_round_trip(proto::Reject{0, lob::RejectReason::ModifyNoChange}, 0);
}

TEST(ProtocolMessages, EncodesNewOrderAtDocumentedOffsets) {
  // seq 0x0102030405060708; order id 0x1122334455667788; price -2; qty 300;
  // side sell (1); type IOC (2); six reserved zero bytes.
  const proto::NewOrder message{0x1122334455667788ULL, -2, 300, lob::Side::Sell,
                                lob::OrderType::IOC};
  const std::array<std::byte, proto::NewOrder::kWireSize> expected{
      std::byte{0x01}, std::byte{0x00},                                    // type new_order
      std::byte{0x20}, std::byte{0x00},                                    // length: 32 body bytes
      std::byte{0x08}, std::byte{0x07}, std::byte{0x06}, std::byte{0x05},  // seq
      std::byte{0x04}, std::byte{0x03}, std::byte{0x02}, std::byte{0x01},  //
      std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},  // reserved
      std::byte{0x88}, std::byte{0x77}, std::byte{0x66}, std::byte{0x55},  // order id
      std::byte{0x44}, std::byte{0x33}, std::byte{0x22}, std::byte{0x11},  //
      std::byte{0xFE}, std::byte{0xFF}, std::byte{0xFF}, std::byte{0xFF},  // price -2
      std::byte{0xFF}, std::byte{0xFF}, std::byte{0xFF}, std::byte{0xFF},  //
      std::byte{0x2C}, std::byte{0x01}, std::byte{0x00}, std::byte{0x00},  // qty 300
      std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},  //
      std::byte{0x01}, std::byte{0x02},                                    // side, type
      std::byte{0x00}, std::byte{0x00}, std::byte{0x00},                   // reserved
      std::byte{0x00}, std::byte{0x00}, std::byte{0x00},                   //
  };

  const auto bytes = encoded(message, 0x0102030405060708ULL);
  EXPECT_EQ(bytes, expected);

  proto::NewOrder decoded{};
  proto::Seq seq = 0;
  ASSERT_EQ(proto::decode(std::span<const std::byte>(expected), decoded, seq),
            proto::DecodeStatus::Ok);
  EXPECT_EQ(decoded, message);
  EXPECT_EQ(seq, 0x0102030405060708ULL);
}

TEST(ProtocolMessages, EncodesRejectAtDocumentedOffsets) {
  // seq 7; order id 0xAABBCCDDEEFF0011; reason unknown_order (2); seven
  // reserved zero bytes.
  const proto::Reject message{0xAABBCCDDEEFF0011ULL, lob::RejectReason::UnknownOrder};
  const std::array<std::byte, proto::Reject::kWireSize> expected{
      std::byte{0x06}, std::byte{0x00},                                    // type reject
      std::byte{0x10}, std::byte{0x00},                                    // length: 16 body bytes
      std::byte{0x07}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},  // seq
      std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},  //
      std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},  // reserved
      std::byte{0x11}, std::byte{0x00}, std::byte{0xFF}, std::byte{0xEE},  // order id
      std::byte{0xDD}, std::byte{0xCC}, std::byte{0xBB}, std::byte{0xAA},  //
      std::byte{0x02},                                                     // reason
      std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},  // reserved
      std::byte{0x00}, std::byte{0x00}, std::byte{0x00},                   //
  };

  const auto bytes = encoded(message, 7);
  EXPECT_EQ(bytes, expected);

  proto::Reject decoded{};
  proto::Seq seq = 0;
  ASSERT_EQ(proto::decode(std::span<const std::byte>(expected), decoded, seq),
            proto::DecodeStatus::Ok);
  EXPECT_EQ(decoded, message);
  EXPECT_EQ(seq, 7U);
}

TEST(ProtocolMessages, EncodeZeroesReservedBytesInARecycledBuffer) {
  std::array<std::byte, proto::NewOrder::kWireSize> buffer{};
  buffer.fill(std::byte{0xFF});
  ASSERT_EQ(
      proto::encode(proto::NewOrder{1, 2, 3, lob::Side::Buy, lob::OrderType::Limit}, 4, buffer),
      buffer.size());
  for (std::size_t i = 0; i < proto::NewOrder::kReservedSize; ++i) {
    EXPECT_EQ(buffer[kHeader + proto::NewOrder::kReservedOffset + i], std::byte{0})
        << "reserved byte " << i;
  }
  for (std::size_t i = 0; i < 4; ++i) {
    EXPECT_EQ(buffer[kHeader + 12 + i], std::byte{0}) << "header reserved byte " << i;
  }
}

TEST(ProtocolMessages, EncodeRejectsShortBuffers) {
  const proto::NewOrder message{1, 2, 3, lob::Side::Buy, lob::OrderType::Limit};
  std::array<std::byte, proto::NewOrder::kWireSize> buffer{};
  buffer.fill(std::byte{0xAB});
  EXPECT_EQ(proto::encode(message, 1, std::span<std::byte>(buffer).first(buffer.size() - 1)), 0U);
  for (const std::byte value : buffer) {
    EXPECT_EQ(value, std::byte{0xAB}) << "a failed encode must not write";
  }
  EXPECT_EQ(proto::encode(message, 1, std::span<std::byte>(buffer).first(0)), 0U);
}

TEST(ProtocolMessages, DecodeReportsEveryTruncatedLength) {
  const proto::Fill message{1, 2, 3, 4};
  const auto bytes = encoded(message, 5);
  for (std::size_t length = 0; length < bytes.size(); ++length) {
    proto::Fill decoded{};
    proto::Seq seq = 0;
    EXPECT_EQ(proto::decode(std::span<const std::byte>(bytes).first(length), decoded, seq),
              proto::DecodeStatus::Truncated)
        << "length " << length;
  }
}

TEST(ProtocolMessages, DecodeRejectsTheWrongMessageType) {
  const auto bytes = encoded(proto::NewOrder{1, 2, 3, lob::Side::Buy, lob::OrderType::Limit}, 5);
  proto::Cancel decoded{};
  proto::Seq seq = 0;
  EXPECT_EQ(proto::decode(std::span<const std::byte>(bytes), decoded, seq),
            proto::DecodeStatus::WrongType);
  EXPECT_EQ(seq, 0U) << "a failed decode must not publish the sequence number";
}

TEST(ProtocolMessages, DecodeRejectsUnknownType) {
  auto bytes = encoded(proto::Cancel{1}, 5);
  set_u16(bytes, proto::kHeaderTypeOffset, 99);
  proto::Cancel decoded{};
  proto::Seq seq = 0;
  EXPECT_EQ(proto::decode(std::span<const std::byte>(bytes), decoded, seq),
            proto::DecodeStatus::UnknownType);
}

TEST(ProtocolMessages, DecodeRejectsLengthThatDoesNotMatchTheType) {
  auto bytes = encoded(proto::Cancel{1}, 5);
  set_u16(bytes, proto::kHeaderLengthOffset, 15);
  proto::Cancel decoded{};
  proto::Seq seq = 0;
  EXPECT_EQ(proto::decode(std::span<const std::byte>(bytes), decoded, seq),
            proto::DecodeStatus::BadLength);
}

TEST(ProtocolMessages, DecodeRejectsOversizeLength) {
  for (const std::uint16_t length : {std::uint16_t{41}, std::uint16_t{60000}}) {
    auto bytes = encoded(proto::NewOrder{1, 2, 3, lob::Side::Buy, lob::OrderType::Limit}, 5);
    set_u16(bytes, proto::kHeaderLengthOffset, length);
    proto::NewOrder decoded{};
    proto::Seq seq = 0;
    EXPECT_EQ(proto::decode(std::span<const std::byte>(bytes), decoded, seq),
              proto::DecodeStatus::Oversize)
        << "length " << length;
  }
}

TEST(ProtocolMessages, DecodeRejectsNonZeroHeaderReserved) {
  auto bytes = encoded(proto::Cancel{1}, 5);
  for (std::size_t i = 0; i < 4; ++i) {
    auto mutated = bytes;
    mutated[proto::kHeaderReservedOffset + i] = std::byte{1};
    proto::Cancel decoded{};
    proto::Seq seq = 0;
    EXPECT_EQ(proto::decode(std::span<const std::byte>(mutated), decoded, seq),
              proto::DecodeStatus::BadReserved)
        << "header reserved byte " << i;
  }
}

TEST(ProtocolMessages, DecodeRejectsNonZeroBodyReserved) {
  expect_nonzero_reserved_rejected(proto::NewOrder{1, 2, 3, lob::Side::Buy, lob::OrderType::Limit},
                                   proto::NewOrder::kReservedOffset,
                                   proto::NewOrder::kReservedSize);
  expect_nonzero_reserved_rejected(proto::Cancel{1}, proto::Cancel::kReservedOffset,
                                   proto::Cancel::kReservedSize);
  expect_nonzero_reserved_rejected(proto::Ack{1}, proto::Ack::kReservedOffset,
                                   proto::Ack::kReservedSize);
  expect_nonzero_reserved_rejected(proto::Reject{1, lob::RejectReason::DuplicateId},
                                   proto::Reject::kReservedOffset, proto::Reject::kReservedSize);
  expect_nonzero_reserved_rejected(proto::Fill{1, 2, 3, 4}, proto::Fill::kReservedOffset,
                                   proto::Fill::kReservedSize);
  expect_nonzero_reserved_rejected(proto::BookUpdate{1, 2, lob::Side::Buy},
                                   proto::BookUpdate::kReservedOffset,
                                   proto::BookUpdate::kReservedSize);
  expect_nonzero_reserved_rejected(proto::Trade{1, 2, 3, 4}, proto::Trade::kReservedOffset,
                                   proto::Trade::kReservedSize);
  expect_nonzero_reserved_rejected(proto::SequenceReset{1}, proto::SequenceReset::kReservedOffset,
                                   proto::SequenceReset::kReservedSize);
}

TEST(ProtocolMessages, DecodeRejectsOutOfRangeEnums) {
  {
    auto bytes = encoded(proto::NewOrder{1, 2, 3, lob::Side::Buy, lob::OrderType::Limit}, 5);
    bytes[kHeader + proto::NewOrder::kSideOffset] = std::byte{2};
    proto::NewOrder decoded{};
    proto::Seq seq = 0;
    EXPECT_EQ(proto::decode(std::span<const std::byte>(bytes), decoded, seq),
              proto::DecodeStatus::InvalidField);
  }
  {
    auto bytes = encoded(proto::NewOrder{1, 2, 3, lob::Side::Buy, lob::OrderType::Limit}, 5);
    bytes[kHeader + proto::NewOrder::kTypeOffset] = std::byte{5};
    proto::NewOrder decoded{};
    proto::Seq seq = 0;
    EXPECT_EQ(proto::decode(std::span<const std::byte>(bytes), decoded, seq),
              proto::DecodeStatus::InvalidField);
  }
  {
    auto bytes = encoded(proto::BookUpdate{1, 2, lob::Side::Buy}, 5);
    bytes[kHeader + proto::BookUpdate::kSideOffset] = std::byte{0xFF};
    proto::BookUpdate decoded{};
    proto::Seq seq = 0;
    EXPECT_EQ(proto::decode(std::span<const std::byte>(bytes), decoded, seq),
              proto::DecodeStatus::InvalidField);
  }
  for (const std::uint8_t reason : {std::uint8_t{0}, std::uint8_t{8}, std::uint8_t{255}}) {
    auto bytes = encoded(proto::Reject{1, lob::RejectReason::DuplicateId}, 5);
    bytes[kHeader + proto::Reject::kReasonOffset] = static_cast<std::byte>(reason);
    proto::Reject decoded{};
    proto::Seq seq = 0;
    EXPECT_EQ(proto::decode(std::span<const std::byte>(bytes), decoded, seq),
              proto::DecodeStatus::InvalidField)
        << "reason " << static_cast<int>(reason);
  }
}

TEST(ProtocolMessages, AcceptsReservedOrderTypeValues) {
  // FOK (3) and post-only (4) are reserved by decision C47: the wire format
  // carries them already so the engine can implement them without a protocol
  // version bump.
  for (const std::uint8_t type : {proto::kWireOrderTypeFok, proto::kWireOrderTypePostOnly}) {
    auto bytes = encoded(proto::NewOrder{1, 2, 3, lob::Side::Buy, lob::OrderType::Limit}, 5);
    bytes[kHeader + proto::NewOrder::kTypeOffset] = static_cast<std::byte>(type);
    proto::NewOrder decoded{};
    proto::Seq seq = 0;
    ASSERT_EQ(proto::decode(std::span<const std::byte>(bytes), decoded, seq),
              proto::DecodeStatus::Ok);
    EXPECT_EQ(static_cast<std::uint8_t>(decoded.type), type);
  }
  // The highest reject reason (7, UnsupportedOrderType after C47) is valid.
  auto bytes = encoded(proto::Reject{1, lob::RejectReason::DuplicateId}, 5);
  bytes[kHeader + proto::Reject::kReasonOffset] = std::byte{7};
  proto::Reject decoded{};
  proto::Seq seq = 0;
  EXPECT_EQ(proto::decode(std::span<const std::byte>(bytes), decoded, seq),
            proto::DecodeStatus::Ok);
}

TEST(ProtocolMessages, DecodeHeader) {
  const auto bytes = encoded(proto::Modify{1, 2, 3}, 0x1122334455667788ULL);

  proto::Header header{};
  ASSERT_EQ(proto::decode_header(bytes, header), proto::DecodeStatus::Ok);
  EXPECT_EQ(header.type, proto::MessageType::Modify);
  EXPECT_EQ(header.length, proto::Modify::kBodySize);
  EXPECT_EQ(header.seq, 0x1122334455667788ULL);

  proto::Header truncated{};
  EXPECT_EQ(proto::decode_header(std::span<const std::byte>(bytes).first(kHeader - 1), truncated),
            proto::DecodeStatus::Truncated);

  // A header is validated on its own: no body needs to be buffered.
  EXPECT_EQ(proto::decode_header(std::span<const std::byte>(bytes).first(kHeader), truncated),
            proto::DecodeStatus::Ok);
}

TEST(ProtocolMessages, SurvivesEverySingleByteMutation) {
  // Flip each byte of a valid frame to 0x00 and to 0xFF. A mutation either
  // decodes or is rejected; the decoders must never read outside the frame or
  // return a status outside the enum (checked by ASan/UBSan too).
  const auto original = encoded(proto::Fill{1, 2, 3, 4}, 5);
  for (std::size_t i = 0; i < original.size(); ++i) {
    for (const std::byte value : {std::byte{0x00}, std::byte{0xFF}}) {
      auto mutated = original;
      mutated[i] = value;
      proto::Fill decoded{};
      proto::Seq seq = 0;
      const auto status = proto::decode(std::span<const std::byte>(mutated), decoded, seq);
      EXPECT_LE(static_cast<unsigned>(status),
                static_cast<unsigned>(proto::DecodeStatus::InvalidField))
          << "byte " << i;
    }
  }
}

TEST(ProtocolMessages, SurvivesRandomBuffers) {
  std::mt19937 rng(12345);
  std::uniform_int_distribution<int> byte_value(0, 255);
  std::array<std::byte, 64> buffer{};
  for (std::size_t iteration = 0; iteration < 2000; ++iteration) {
    const std::size_t size = iteration % (buffer.size() + 1);
    for (std::size_t i = 0; i < size; ++i) {
      buffer[i] = static_cast<std::byte>(byte_value(rng));
    }
    proto::Fill decoded{};
    proto::Seq seq = 0;
    const auto status = proto::decode(std::span<const std::byte>(buffer).first(size), decoded, seq);
    EXPECT_LE(static_cast<unsigned>(status),
              static_cast<unsigned>(proto::DecodeStatus::InvalidField))
        << "buffer size " << size;
  }
}

TEST(ProtocolMessages, DecodeStatusNames) {
  EXPECT_EQ(proto::to_string(proto::DecodeStatus::Ok), "ok");
  EXPECT_EQ(proto::to_string(proto::DecodeStatus::Oversize), "oversize");
  EXPECT_EQ(proto::to_string(proto::DecodeStatus::InvalidField), "invalid_field");
}

}  // namespace
