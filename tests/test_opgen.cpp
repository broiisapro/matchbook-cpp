// The golden stream hash below was produced once by running the generator with
// the pinned seed and config through the byte serializer in this file; the
// byte layout is one operation after another as:
//   kind u8, id u64 LE, side u8, type u8, price i64 LE, qty i64 LE
// hashed with XXH64 (seed 0). The golden is a regression pin: changing the
// generator on purpose means regenerating this constant.
//
// Regenerated for the Phase 2 order-type mix (issue #90): the operation stream
// was written to a file by a throwaway program that implements the layout
// above, then hashed with the Python `xxhash` binding (XXH64, seed 0), outside
// this test binary.

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
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
using lob::opgen::ladder_stress_mix;
using lob::opgen::Op;
using lob::opgen::OpGenerator;
using lob::opgen::OpKind;
using lob::opgen::OpMix;

constexpr std::uint64_t kGoldenSeed = 12345;
constexpr Config kGoldenConfig{100, 200, 20000, 500};
constexpr std::uint64_t kGoldenHash = 0xa0215bec375085e8ULL;

constexpr std::uint64_t kLadderStressSeed = 424242;
constexpr Config kLadderStressConfig{1, 20000, 4096, 64};
constexpr std::uint64_t kLadderStressHash = 0x4f03adaadaaca3abULL;

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

std::vector<Op> generate(std::uint64_t seed, const Config& config, std::size_t count,
                         OpMix mix = {}) {
  OpGenerator generator(seed, config, mix);
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
  std::size_t foks = 0;
  std::size_t post_onlys = 0;

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
          case OrderType::FOK:
            ++foks;
            break;
          case OrderType::PostOnly:
            ++post_onlys;
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
  EXPECT_GT(foks, 0U);
  EXPECT_GT(post_onlys, 0U);
}

// The Phase 2 types are rejected as unsupported until the matching semantics
// land (decision C47), so the generator emits them at a small, bounded weight:
// enough for the differential test to cover the reject path in every preset,
// far below the types that actually trade. The sequential chances in the
// generator make the share a few percent at most.
TEST(OpGenerator, EmitsPhaseTwoTypesAtASmallWeight) {
  struct Counts {
    std::size_t submits = 0;
    std::size_t foks = 0;
    std::size_t post_onlys = 0;
  };

  const auto count = [](const std::vector<Op>& ops) {
    Counts counts;
    for (const Op& op : ops) {
      if (op.kind != OpKind::Submit) {
        continue;
      }
      ++counts.submits;
      if (op.type == OrderType::FOK) {
        ++counts.foks;
      } else if (op.type == OrderType::PostOnly) {
        ++counts.post_onlys;
      }
    }
    return counts;
  };

  const Counts defaults = count(generate(kGoldenSeed, kGoldenConfig, 100000));
  const Counts wide = count(generate(kGoldenSeed, kGoldenConfig, 100000, lob::opgen::wide_mix()));
  const Counts ladder =
      count(generate(kGoldenSeed, kLadderStressConfig, 100000, ladder_stress_mix()));

  for (const Counts& counts : {defaults, wide, ladder}) {
    ASSERT_GT(counts.submits, 0U);
    EXPECT_GT(counts.foks, 0U);
    EXPECT_GT(counts.post_onlys, 0U);
    // At most 5% of each, and at most 10% of both together.
    EXPECT_LT(counts.foks * 20, counts.submits);
    EXPECT_LT(counts.post_onlys * 20, counts.submits);
    EXPECT_LT((counts.foks + counts.post_onlys) * 10, counts.submits);
  }
}

// A FOK or PostOnly order never rests, so the generator's own book view must
// not grow from one. With unknown-id picks disabled, a cancel can only fall
// back to a synthetic unknown id when the model holds no live order at all.
TEST(OpGenerator, PhaseTwoSubmitsDoNotEnterTheModelledBook) {
  OpMix mix;
  mix.submit_weight = 1;
  mix.cancel_weight = 1;
  mix.modify_weight = 0;
  mix.market_weight = 0;
  mix.ioc_weight = 0;
  mix.fok_weight = 100;  // every submit is an unsupported type
  mix.duplicate_id_weight = 0;
  mix.reuse_id_weight = 0;
  mix.unknown_id_weight = 0;
  mix.invalid_qty_weight = 0;

  OpGenerator generator(kGoldenSeed, kGoldenConfig, mix);
  std::size_t submits = 0;
  std::size_t cancels = 0;
  for (int i = 0; i < 40; ++i) {
    const Op op = generator.next();
    if (op.kind == OpKind::Submit) {
      EXPECT_EQ(op.type, OrderType::FOK);
      ++submits;
      continue;
    }
    ASSERT_EQ(op.kind, OpKind::Cancel);
    ++cancels;
    EXPECT_GE(op.id, 1ULL << 63);
  }
  EXPECT_GT(submits, 0U);
  EXPECT_GT(cancels, 0U);
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

TEST(OpGenerator, LadderStressGoldenHash) {
  const std::vector<Op> first =
      generate(kLadderStressSeed, kLadderStressConfig, 10000, ladder_stress_mix());
  const std::vector<Op> second =
      generate(kLadderStressSeed, kLadderStressConfig, 10000, ladder_stress_mix());

  EXPECT_EQ(first, second);
  EXPECT_EQ(hash_ops(first), kLadderStressHash);
}

TEST(OpGenerator, LadderStressPricesClusterAtBitmapBoundaries) {
  const std::vector<Op> ops =
      generate(kLadderStressSeed, kLadderStressConfig, 100000, ladder_stress_mix());

  // Offsets above min_price: 63/64/65 end the first occupancy word and
  // 4095/4096/4097 the first summary word; `top` is the configured maximum.
  constexpr std::int64_t kOffsets[] = {0, 63, 64, 65, 4095, 4096, 4097, -1};
  const std::int64_t min = kLadderStressConfig.min_price;
  const std::int64_t max = kLadderStressConfig.max_price;

  std::array<bool, 8> saw_exact{};
  for (const Op& op : ops) {
    if (op.kind == OpKind::Cancel || (op.kind == OpKind::Submit && op.type == OrderType::Market)) {
      continue;
    }
    // Every price sits within two ticks of a boundary offset.
    bool near_boundary = false;
    for (std::size_t i = 0; i < 8; ++i) {
      const std::int64_t offset = kOffsets[i] < 0 ? max - min : kOffsets[i];
      const std::int64_t price = std::clamp<std::int64_t>(min + offset, min, max);
      if (op.price >= price - 2 && op.price <= price + 2) {
        near_boundary = true;
        if (op.price == price) {
          saw_exact[i] = true;
        }
      }
    }
    EXPECT_TRUE(near_boundary) << op;
  }

  // Index 0 is min_price and index 7 max_price, so this also pins orders at
  // both ends of the configured range.
  for (std::size_t i = 0; i < 8; ++i) {
    EXPECT_TRUE(saw_exact[i]) << "boundary index " << i << " never produced its exact price";
  }
}

TEST(OpGenerator, LadderStressIncludesLongOneSidedSweeps) {
  const std::vector<Op> ops =
      generate(kLadderStressSeed, kLadderStressConfig, 100000, ladder_stress_mix());

  std::size_t buy_sweeps = 0;
  std::size_t sell_sweeps = 0;
  for (const Op& op : ops) {
    if (op.kind != OpKind::Submit || op.type != OrderType::Market) {
      continue;
    }
    // market_weight is zero in this preset, so every market order is a sweep.
    EXPECT_EQ(op.price, 0);
    EXPECT_GE(op.qty, 100000);
    if (op.side == Side::Buy) {
      ++buy_sweeps;
    } else {
      ++sell_sweeps;
    }
  }
  EXPECT_GT(buy_sweeps, 0U);
  EXPECT_GT(sell_sweeps, 0U);
}

TEST(OpGenerator, LadderStressDrainRefillCycleAlternatesSidesAndWalksBoundaries) {
  OpMix mix = ladder_stress_mix();
  mix.submit_weight = 0;
  mix.cancel_weight = 0;
  mix.modify_weight = 0;
  mix.sweep_weight = 0;
  OpGenerator generator(kLadderStressSeed, kLadderStressConfig, mix);

  const Op add_buy = generator.next();
  const Op cancel_buy = generator.next();
  const Op add_sell = generator.next();
  const Op cancel_sell = generator.next();
  const Op next_add_buy = generator.next();

  EXPECT_EQ(add_buy.kind, OpKind::Submit);
  EXPECT_EQ(add_buy.side, Side::Buy);
  EXPECT_EQ(add_buy.type, OrderType::Limit);
  EXPECT_EQ(cancel_buy.kind, OpKind::Cancel);
  EXPECT_EQ(cancel_buy.id, add_buy.id);
  EXPECT_EQ(add_sell.kind, OpKind::Submit);
  EXPECT_EQ(add_sell.side, Side::Sell);
  EXPECT_EQ(cancel_sell.kind, OpKind::Cancel);
  EXPECT_EQ(cancel_sell.id, add_sell.id);

  // The first cycle uses the bottom boundary (offset 0 + jitter), the second
  // the 63/64/65 word-end cluster.
  EXPECT_GE(add_buy.price, kLadderStressConfig.min_price);
  EXPECT_LE(add_buy.price, kLadderStressConfig.min_price + 2);
  EXPECT_GE(next_add_buy.price, kLadderStressConfig.min_price + 61);
  EXPECT_LE(next_add_buy.price, kLadderStressConfig.min_price + 67);
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
