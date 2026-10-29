// The fast engine against the reference engine at scale, plus three stress
// configurations where capacities and the price range bite. A failure here is
// a real semantic difference; `lobsim difftest` prints a shrunk repro for it.

#include <gtest/gtest.h>

#include <cstdint>

#include "lob/config.hpp"
#include "lob/order_book.hpp"
#include "reference/reference_book.hpp"
#include "tools/flowfile/replay_config.hpp"
#include "tools/opgen/differential.hpp"
#include "tools/opgen/opgen.hpp"

#if defined(__has_feature)
#define LOB_HAS_FEATURE(feature) __has_feature(feature)
#else
#define LOB_HAS_FEATURE(feature) 0
#endif

namespace {

constexpr bool kSanitized =
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
    true;
#else
    LOB_HAS_FEATURE(address_sanitizer) || LOB_HAS_FEATURE(thread_sanitizer);
#endif

using lob::Config;
using lob::OrderBook;
using lob::opgen::DiffResult;
using lob::opgen::OpGenerator;
using lob::opgen::OpMix;
using lob::opgen::run_differential;
using lob::ref::ReferenceBook;

void expect_match(const Config& config, std::uint64_t seed, std::uint64_t ops, OpMix mix = {}) {
  OpGenerator generator(seed, config, mix);
  const DiffResult result = run_differential<OrderBook, ReferenceBook>(config, generator, ops);

  ASSERT_EQ(result.mismatches, 0U)
      << "first mismatch at op " << result.first_mismatch_index << ": " << result.op;
  EXPECT_EQ(result.ops_run, ops);
}

TEST(FastVsReference, RandomOpsMatchReference) {
  constexpr Config config{100, 200, 128, 32};
  expect_match(config, 20261018, kSanitized ? 100000 : 1000000);
}

// Phase 2 order types: FOK and PostOnly are part of the generated stream,
// including PostOnly orders priced through the book, so the accept, kill,
// WouldCross and rest paths all run through the differential harness. This is
// the all-types companion of RandomOpsMatchReference.
TEST(FastVsReference, RandomOpsMatchReferenceAllTypes) {
  constexpr Config config{100, 200, 128, 32};
  expect_match(config, 20261028, kSanitized ? 100000 : 1000000, lob::opgen::all_types_mix());
}

TEST(FastVsReference, TightMaxOrdersMatchesReference) {
  // Capacity 8 on the order pool: most limit remainders cannot rest.
  constexpr Config config{100, 200, 8, 32};
  expect_match(config, 20261019, kSanitized ? 20000 : 200000);
}

TEST(FastVsReference, TightMaxLevelsMatchesReference) {
  // Four levels per side inside a twenty-tick range: new levels are refused
  // while existing ones keep working.
  constexpr Config config{100, 120, 256, 4};
  expect_match(config, 20261020, kSanitized ? 20000 : 200000);
}

TEST(FastVsReference, NarrowPriceRangeMatchesReference) {
  // Three valid prices: the generator's edge prices are rejected often and
  // matching happens in a very crowded ladder.
  constexpr Config config{150, 152, 128, 8};
  expect_match(config, 20261021, kSanitized ? 20000 : 200000);
}

TEST(FastVsReference, RandomOpsMatchReferenceWideConfig) {
  // The wide price range and level cap (1..200000, 200000 levels per side)
  // with the wide generator mix: the 300-tick mid band makes the book
  // hundreds of price levels deep, and the harness reports the peak depth it
  // sampled on each side of each engine.
  //
  // max_orders is stepped down from the CLI preset's 1048576: the harness
  // compares full-book digests after every operation, so a million-order book
  // makes a 300k-op run quadratic (measured ~128 s of CPU with the full
  // capacity). 2048 keeps the run in the low seconds while still letting the
  // depth test see ~300 levels per side. The CLI's `--config wide` keeps the
  // full replay config, and the nightly runs it at a reduced op count.
  Config config = lob::flowfile::default_replay_config();
  config.max_orders = 2048;
  const std::uint64_t ops = kSanitized ? 30000 : 300000;
  OpGenerator generator(20261025, config, lob::opgen::wide_mix());
  const DiffResult result = run_differential<OrderBook, ReferenceBook>(config, generator, ops);

  ASSERT_EQ(result.mismatches, 0U)
      << "first mismatch at op " << result.first_mismatch_index << ": " << result.op;
  EXPECT_EQ(result.ops_run, ops);
  EXPECT_GT(result.max_bid_depth_a, 100U) << "fast engine bid depth";
  EXPECT_GT(result.max_ask_depth_a, 100U) << "fast engine ask depth";
  EXPECT_GT(result.max_bid_depth_b, 100U) << "reference engine bid depth";
  EXPECT_GT(result.max_ask_depth_b, 100U) << "reference engine ask depth";
}

TEST(FastVsReference, LadderStressMatchesReference) {
  // The ladder-stress mix clusters prices at the occupancy bitmap boundaries
  // (offsets 0, 63, 64, 65, 4095, 4096, 4097 and the top), sweeps a whole
  // side empty with market orders and drains/refills the best level. The
  // 1..20000 range spans five 64-bit summary words of the array ladder's
  // occupancy bitmap, and the build's ladder (tree or array) is the one under
  // test.
  constexpr Config config{1, 20000, 4096, 64};
  expect_match(config, 20261022, kSanitized ? 20000 : 200000, lob::opgen::ladder_stress_mix());
}

}  // namespace
