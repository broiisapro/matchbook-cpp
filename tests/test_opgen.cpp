// The golden stream hash below was produced once by running the generator with
// the pinned seed and config through the byte serializer in this file; the
// byte layout is one operation after another as:
//   kind u8, id u64 LE, side u8, type u8, price i64 LE, qty i64 LE
// hashed with XXH64 (seed 0). The golden is a regression pin: changing the
// generator on purpose means regenerating this constant.

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

#define XXH_STATIC_LINKING_ONLY
#include <xxhash.h>

#include "lob/config.hpp"
#include "lob/types.hpp"
#include "tools/opgen/opgen.hpp"

namespace {

using lob::Config;
using lob::OrderType;
using lob::Side;
using lob::opgen::Op;
using lob::opgen::OpGenerator;
using lob::opgen::OpKind;

constexpr std::uint64_t kGoldenSeed = 12345;
constexpr Config kGoldenConfig{100, 200, 20000, 500};
constexpr std::uint64_t kGoldenHash = 0x2984d944967473fdULL;

std::vector<std::byte> serialize(const std::vector<Op>& ops) {
  std::vector<std::byte> bytes;
  bytes.reserve(ops.size() * 27);
  const auto put = [&bytes](std::uint64_t value, std::size_t width) {
    for (std::size_t i = 0; i < width; ++i) {
      bytes.push_back(static_cast<std::byte>((value >> (8 * i)) & 0xff));
    }
  };
  for (const Op& op : ops) {
    put(static_cast<std::uint64_t>(op.kind), 1);
    put(op.id, 8);
    put(static_cast<std::uint64_t>(op.side), 1);
    put(static_cast<std::uint64_t>(op.type), 1);
    put(static_cast<std::uint64_t>(op.price), 8);
    put(static_cast<std::uint64_t>(op.qty), 8);
  }
  return bytes;
}

std::uint64_t hash_ops(const std::vector<Op>& ops) {
  const std::vector<std::byte> bytes = serialize(ops);
  return XXH64(bytes.data(), bytes.size(), 0);
}

std::vector<Op> generate(std::uint64_t seed, const Config& config, std::size_t count) {
  OpGenerator generator(seed, config);
  std::vector<Op> ops;
  ops.reserve(count);
  for (std::size_t i = 0; i < count; ++i) {
    ops.push_back(generator.next());
  }
  return ops;
}

TEST(OpGenerator, SameSeedProducesIdenticalStreamsAndGoldenHash) {
  const std::vector<Op> first = generate(kGoldenSeed, kGoldenConfig, 10000);
  const std::vector<Op> second = generate(kGoldenSeed, kGoldenConfig, 10000);

  EXPECT_EQ(first, second);
  EXPECT_EQ(hash_ops(first), kGoldenHash);
}

TEST(OpGenerator, DifferentSeedsDiffer) {
  const std::vector<Op> first = generate(1, kGoldenConfig, 1000);
  const std::vector<Op> second = generate(2, kGoldenConfig, 1000);

  EXPECT_NE(first, second);
  EXPECT_NE(hash_ops(first), hash_ops(second));
}

TEST(OpGenerator, EveryKindAndSubmitTypeAppearsIn100kOps) {
  constexpr Config config{1, 1000, 100000, 1000};
  const std::vector<Op> ops = generate(7, config, 100000);
  ASSERT_EQ(ops.size(), 100000U);

  std::size_t submits = 0;
  std::size_t cancels = 0;
  std::size_t modifies = 0;
  std::size_t limits = 0;
  std::size_t markets = 0;
  std::size_t iocs = 0;

  for (const Op& op : ops) {
    switch (op.kind) {
      case OpKind::Submit:
        ++submits;
        switch (op.type) {
          case OrderType::Limit:
            ++limits;
            break;
          case OrderType::Market:
            ++markets;
            break;
          case OrderType::IOC:
            ++iocs;
            break;
        }
        break;
      case OpKind::Cancel:
        ++cancels;
        break;
      case OpKind::Modify:
        ++modifies;
        break;
    }
  }

  EXPECT_GT(submits, 0U);
  EXPECT_GT(cancels, 0U);
  EXPECT_GT(modifies, 0U);
  EXPECT_GT(limits, 0U);
  EXPECT_GT(markets, 0U);
  EXPECT_GT(iocs, 0U);
}

TEST(OpGenerator, PricesStayWithinTwoTicksOfTheConfiguredRange) {
  const std::vector<Op> ops = generate(11, kGoldenConfig, 100000);

  bool saw_min = false;
  bool saw_max = false;
  bool saw_below = false;
  bool saw_above = false;

  for (const Op& op : ops) {
    if (op.kind == OpKind::Cancel) {
      EXPECT_EQ(op.price, 0);
      EXPECT_EQ(op.qty, 0);
      continue;
    }
    if (op.kind == OpKind::Submit && op.type == OrderType::Market) {
      EXPECT_EQ(op.price, 0);
      continue;
    }
    EXPECT_GE(op.price, kGoldenConfig.min_price - 2);
    EXPECT_LE(op.price, kGoldenConfig.max_price + 2);
    saw_min = saw_min || op.price == kGoldenConfig.min_price;
    saw_max = saw_max || op.price == kGoldenConfig.max_price;
    saw_below = saw_below || op.price < kGoldenConfig.min_price;
    saw_above = saw_above || op.price > kGoldenConfig.max_price;
  }

  EXPECT_TRUE(saw_min);
  EXPECT_TRUE(saw_max);
  EXPECT_TRUE(saw_below);
  EXPECT_TRUE(saw_above);
}

TEST(OpGenerator, PrintsKindAndFields) {
  const Op op{OpKind::Modify, 42, Side::Sell, OrderType::Limit, 101, 7};

  EXPECT_EQ(lob::opgen::to_string(OpKind::Submit), "submit");
  EXPECT_EQ(lob::opgen::to_string(OpKind::Cancel), "cancel");
  EXPECT_EQ(lob::opgen::to_string(OpKind::Modify), "modify");

  std::ostringstream out;
  out << op;
  const std::string text = out.str();
  EXPECT_NE(text.find("modify"), std::string::npos);
  EXPECT_NE(text.find("42"), std::string::npos);
  EXPECT_NE(text.find("sell"), std::string::npos);
  EXPECT_NE(text.find("101"), std::string::npos);
}

}  // namespace
