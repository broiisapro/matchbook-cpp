// The fast engine against the reference engine at scale, plus three stress
// configurations where capacities and the price range bite. A failure here is
// a real semantic difference; `lobsim difftest` prints a shrunk repro for it.

#include <gtest/gtest.h>

#include <cstdint>

#include "lob/config.hpp"
#include "lob/order_book.hpp"
#include "reference/reference_book.hpp"
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
