#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

#include "lob/types.hpp"
#include "tools/flowfile/flow_record.hpp"

namespace {

using lob::flowfile::FlowHeader;
using lob::flowfile::FlowOp;
using lob::flowfile::FlowRecord;

constexpr std::size_t kHeaderSize = lob::flowfile::kFlowHeaderSize;
constexpr std::size_t kRecordSize = lob::flowfile::kFlowRecordSize;

using HeaderBuffer = std::array<std::byte, kHeaderSize>;
using RecordBuffer = std::array<std::byte, kRecordSize>;

RecordBuffer encoded(const FlowRecord& record) {
  RecordBuffer buffer{};
  lob::flowfile::encode(record, buffer);
  return buffer;
}

HeaderBuffer encoded_header(const FlowHeader& header) {
  HeaderBuffer buffer{};
  lob::flowfile::encode_header(header, buffer);
  return buffer;
}

TEST(FlowFormat, Constants) {
  EXPECT_EQ(lob::flowfile::kFlowMagic, "LOBFLOW1");
  EXPECT_EQ(lob::flowfile::kFlowVersion, 1U);
  EXPECT_EQ(kHeaderSize, 32U);
  EXPECT_EQ(kRecordSize, 40U);
  EXPECT_EQ(static_cast<int>(FlowOp::Submit), 1);
  EXPECT_EQ(static_cast<int>(FlowOp::Cancel), 2);
  EXPECT_EQ(static_cast<int>(FlowOp::Modify), 3);
}

TEST(FlowRecord, RoundTripsEveryOpSideAndType) {
  for (const FlowOp op : {FlowOp::Submit, FlowOp::Cancel, FlowOp::Modify}) {
    for (const lob::Side side : {lob::Side::Buy, lob::Side::Sell}) {
      for (const lob::OrderType type :
           {lob::OrderType::Limit, lob::OrderType::Market, lob::OrderType::IOC}) {
        const FlowRecord record{op, side, type, 42, 101, 7, 123456789};
        EXPECT_EQ(lob::flowfile::decode(encoded(record)), record);
      }
    }
  }
}

TEST(FlowRecord, RoundTripsNegativeAndLargeValues) {
  const FlowRecord negative{FlowOp::Modify, lob::Side::Sell, lob::OrderType::IOC, 7, -5, -7, 11};
  EXPECT_EQ(lob::flowfile::decode(encoded(negative)), negative);

  const FlowRecord extremes{FlowOp::Submit,
                            lob::Side::Buy,
                            lob::OrderType::Market,
                            std::numeric_limits<std::uint64_t>::max(),
                            std::numeric_limits<std::int64_t>::min(),
                            std::numeric_limits<std::int64_t>::max(),
                            std::numeric_limits<std::uint64_t>::max()};
  EXPECT_EQ(lob::flowfile::decode(encoded(extremes)), extremes);

  const FlowRecord zeroes{FlowOp::Cancel, lob::Side::Buy, lob::OrderType::Limit, 0, 0, 0, 0};
  EXPECT_EQ(lob::flowfile::decode(encoded(zeroes)), zeroes);
}

TEST(FlowRecord, EncodesFieldsAtDocumentedOffsets) {
  const FlowRecord record{
      FlowOp::Modify, lob::Side::Buy, lob::OrderType::Market, 0x0102030405060708ULL, -7, 42, 99};
  const RecordBuffer bytes = encoded(record);

  EXPECT_EQ(bytes[0], std::byte{3});  // op modify
  EXPECT_EQ(bytes[1], std::byte{0});  // side buy
  EXPECT_EQ(bytes[2], std::byte{1});  // type market
  for (std::size_t i = 3; i < 8; ++i) {
    EXPECT_EQ(bytes[i], std::byte{0}) << "reserved byte " << i << " must be zero";
  }
  EXPECT_EQ(bytes[8], std::byte{0x08});  // id, little-endian
  EXPECT_EQ(bytes[15], std::byte{0x01});
  EXPECT_EQ(bytes[16], std::byte{0xF9});  // price -7, two's complement
  EXPECT_EQ(bytes[23], std::byte{0xFF});
  EXPECT_EQ(bytes[24], std::byte{42});  // qty
  EXPECT_EQ(bytes[31], std::byte{0});
  EXPECT_EQ(bytes[32], std::byte{99});  // ts_ns
  EXPECT_EQ(bytes[39], std::byte{0});
}

TEST(FlowRecord, DecodesHandBuiltBuffer) {
  // Built by hand from the byte spec: op submit, side sell, type IOC,
  // id 0x1122334455667788, price -2, qty 0x0102030405060708, ts_ns 1.
  const RecordBuffer buffer{
      std::byte{0x01}, std::byte{0x01}, std::byte{0x02}, std::byte{0x00}, std::byte{0x00},
      std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x88}, std::byte{0x77},
      std::byte{0x66}, std::byte{0x55}, std::byte{0x44}, std::byte{0x33}, std::byte{0x22},
      std::byte{0x11}, std::byte{0xFE}, std::byte{0xFF}, std::byte{0xFF}, std::byte{0xFF},
      std::byte{0xFF}, std::byte{0xFF}, std::byte{0xFF}, std::byte{0xFF}, std::byte{0x08},
      std::byte{0x07}, std::byte{0x06}, std::byte{0x05}, std::byte{0x04}, std::byte{0x03},
      std::byte{0x02}, std::byte{0x01}, std::byte{0x01}, std::byte{0x00}, std::byte{0x00},
      std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
  };
  const FlowRecord expected{FlowOp::Submit,
                            lob::Side::Sell,
                            lob::OrderType::IOC,
                            0x1122334455667788ULL,
                            -2,
                            0x0102030405060708LL,
                            1};
  EXPECT_EQ(lob::flowfile::decode(buffer), expected);
  EXPECT_EQ(encoded(expected), buffer);
}

TEST(FlowRecord, DecodeIsTheInverseOfEncode) {
  for (std::int64_t ts = 0; ts < 3; ++ts) {
    const FlowRecord record{FlowOp::Submit,
                            lob::Side::Sell,
                            lob::OrderType::Limit,
                            static_cast<lob::OrderId>(ts),
                            100 + ts,
                            5 + ts,
                            static_cast<std::uint64_t>(ts)};
    const RecordBuffer bytes = encoded(record);
    EXPECT_EQ(encoded(lob::flowfile::decode(bytes)), bytes);
  }
}

TEST(FlowHeader, EncodesFieldsAtDocumentedOffsets) {
  const HeaderBuffer bytes = encoded_header(FlowHeader{0x1122334455667788ULL});

  const std::array<unsigned char, 8> magic{0x4C, 0x4F, 0x42, 0x46, 0x4C, 0x4F, 0x57, 0x31};
  for (std::size_t i = 0; i < magic.size(); ++i) {
    EXPECT_EQ(bytes[i], static_cast<std::byte>(magic[i])) << "magic byte " << i;
  }
  EXPECT_EQ(bytes[8], std::byte{1});  // version
  EXPECT_EQ(bytes[11], std::byte{0});
  EXPECT_EQ(bytes[12], std::byte{40});  // record size
  EXPECT_EQ(bytes[15], std::byte{0});
  EXPECT_EQ(bytes[16], std::byte{0x88});  // record count, little-endian
  EXPECT_EQ(bytes[23], std::byte{0x11});
  for (std::size_t i = 24; i < kHeaderSize; ++i) {
    EXPECT_EQ(bytes[i], std::byte{0}) << "reserved byte " << i << " must be zero";
  }
}

TEST(FlowHeader, RoundTripsRecordCount) {
  for (const std::uint64_t count : {std::uint64_t{0}, std::uint64_t{1}, std::uint64_t{1} << 40}) {
    const auto decoded = lob::flowfile::decode_header(encoded_header(FlowHeader{count}));
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(*decoded, (FlowHeader{count}));
  }
}

TEST(FlowHeader, RejectsBadMagic) {
  HeaderBuffer bytes = encoded_header(FlowHeader{3});
  bytes[0] = std::byte{'X'};
  EXPECT_FALSE(lob::flowfile::decode_header(bytes).has_value());
}

TEST(FlowHeader, RejectsBadVersion) {
  HeaderBuffer bytes = encoded_header(FlowHeader{3});
  bytes[8] = std::byte{2};
  EXPECT_FALSE(lob::flowfile::decode_header(bytes).has_value());
}

TEST(FlowHeader, RejectsBadRecordSize) {
  HeaderBuffer bytes = encoded_header(FlowHeader{3});
  bytes[12] = std::byte{41};
  EXPECT_FALSE(lob::flowfile::decode_header(bytes).has_value());
}

}  // namespace
