#include <gtest/gtest.h>

#include <cstdint>

#include "lob/digest.hpp"
#include "lob/types.hpp"

namespace {

// Golden digest values. Each was computed independently, outside C++, from the
// byte stream built by hand in Python; the exact command is in the comment
// above each constant.

// Empty book: byte stream is only the version byte 0x01.
//   uv run --with xxhash python -c 'import xxhash; print(hex(xxhash.xxh64(bytes([1]),
//   seed=0).intdigest()))'
constexpr std::uint64_t kEmptyBookDigest = 0x8a4127811b21e730ULL;

// Bids 100: [(1, 10), (2, 5)], 99: [(3, 7)]; asks 101: [(4, 8)].
//   uv run --with xxhash python -c '
//   import struct, xxhash
//   data = bytes([1])
//   def level(side, price, count):
//       global data
//       data += struct.pack("<BqI", side, price, count)
//   def order(order_id, qty):
//       global data
//       data += struct.pack("<Qq", order_id, qty)
//   level(0, 100, 2)
//   order(1, 10)
//   order(2, 5)
//   level(0, 99, 1)
//   order(3, 7)
//   level(1, 101, 1)
//   order(4, 8)
//   print(hex(xxhash.xxh64(data, seed=0).intdigest()))
//   '
constexpr std::uint64_t kThreeLevelBookDigest = 0x73a74ea7e72c5b1bULL;

// One bid level, price 1, with a large id and a negative quantity, to pin the
// full-width little-endian i64/u64 encoding.
//   uv run --with xxhash python -c '
//   import struct, xxhash
//   data = bytes([1])
//   data += struct.pack("<BqI", 0, 1, 1)
//   data += struct.pack("<Qq", 0x0102030405060708, -2)
//   print(hex(xxhash.xxh64(data, seed=0).intdigest()))
//   '
constexpr std::uint64_t kWideValueDigest = 0xf82c1ca2d804ec0fULL;

std::uint64_t three_level_book_digest() {
  lob::DigestBuilder builder;
  builder.level(lob::Side::Buy, 100, 2);
  builder.order(1, 10);
  builder.order(2, 5);
  builder.level(lob::Side::Buy, 99, 1);
  builder.order(3, 7);
  builder.level(lob::Side::Sell, 101, 1);
  builder.order(4, 8);
  return builder.finish();
}

TEST(Digest, EmptyBookGoldenValue) {
  lob::DigestBuilder builder;
  EXPECT_EQ(builder.finish(), kEmptyBookDigest);
}

TEST(Digest, ThreeLevelBookGoldenValue) {
  EXPECT_EQ(three_level_book_digest(), kThreeLevelBookDigest);
}

TEST(Digest, WideValuesGoldenValue) {
  lob::DigestBuilder builder;
  builder.level(lob::Side::Buy, 1, 1);
  builder.order(0x0102030405060708ULL, -2);
  EXPECT_EQ(builder.finish(), kWideValueDigest);
}

TEST(Digest, ReorderingOrdersChangesDigest) {
  lob::DigestBuilder builder;
  builder.level(lob::Side::Buy, 100, 2);
  builder.order(2, 5);
  builder.order(1, 10);
  builder.level(lob::Side::Buy, 99, 1);
  builder.order(3, 7);
  builder.level(lob::Side::Sell, 101, 1);
  builder.order(4, 8);
  EXPECT_NE(builder.finish(), kThreeLevelBookDigest);
}

TEST(Digest, ChangingQtyChangesDigest) {
  lob::DigestBuilder builder;
  builder.level(lob::Side::Buy, 100, 2);
  builder.order(1, 11);
  builder.order(2, 5);
  builder.level(lob::Side::Buy, 99, 1);
  builder.order(3, 7);
  builder.level(lob::Side::Sell, 101, 1);
  builder.order(4, 8);
  EXPECT_NE(builder.finish(), kThreeLevelBookDigest);
}

TEST(Digest, ChangingSideChangesDigest) {
  lob::DigestBuilder builder;
  builder.level(lob::Side::Sell, 100, 2);
  builder.order(1, 10);
  builder.order(2, 5);
  builder.level(lob::Side::Sell, 99, 1);
  builder.order(3, 7);
  builder.level(lob::Side::Buy, 101, 1);
  builder.order(4, 8);
  EXPECT_NE(builder.finish(), kThreeLevelBookDigest);
}

TEST(Digest, ChangingOrderIdChangesDigest) {
  lob::DigestBuilder builder;
  builder.level(lob::Side::Buy, 100, 2);
  builder.order(9, 10);
  builder.order(2, 5);
  builder.level(lob::Side::Buy, 99, 1);
  builder.order(3, 7);
  builder.level(lob::Side::Sell, 101, 1);
  builder.order(4, 8);
  EXPECT_NE(builder.finish(), kThreeLevelBookDigest);
}

TEST(Digest, ChangingPriceChangesDigest) {
  lob::DigestBuilder builder;
  builder.level(lob::Side::Buy, 100, 2);
  builder.order(1, 10);
  builder.order(2, 5);
  builder.level(lob::Side::Buy, 98, 1);
  builder.order(3, 7);
  builder.level(lob::Side::Sell, 101, 1);
  builder.order(4, 8);
  EXPECT_NE(builder.finish(), kThreeLevelBookDigest);
}

}  // namespace
