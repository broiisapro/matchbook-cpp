// A seeded synthetic flow replayed through both engines must produce identical
// trade lists and identical final digests.

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <variant>
#include <vector>

#include "lob/config.hpp"
#include "lob/order_book.hpp"
#include "reference/reference_book.hpp"
#include "support/recording_listener.hpp"
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
using lob::opgen::apply_op;
using lob::opgen::OpGenerator;
using lob::opgen::OpMix;
using lob::ref::ReferenceBook;

struct Replay {
  std::vector<lob::test::Trade> trades;
  std::uint64_t digest = 0;
};

template <class Engine>
Replay replay(const Config& config, std::uint64_t seed, std::uint64_t ops, const OpMix& mix) {
  lob::test::RecordingListener listener;
  Engine engine(config, listener);
  OpGenerator generator(seed, config, mix);

  Replay replay;
  for (std::uint64_t i = 0; i < ops; ++i) {
    listener.clear();
    apply_op(engine, generator.next());
    for (const auto& event : listener.events) {
      if (const auto* trade = std::get_if<lob::test::Trade>(&event); trade != nullptr) {
        replay.trades.push_back(*trade);
      }
    }
  }
  replay.digest = engine.digest();
  return replay;
}

TEST(CrossEngine, SeededFlowGivesIdenticalTradesAndDigest) {
  constexpr Config config{100, 200, 256, 40};
  const std::uint64_t ops = kSanitized ? 20000 : 200000;

  const Replay fast = replay<OrderBook>(config, 20261022, ops, OpMix{});
  const Replay reference = replay<ReferenceBook>(config, 20261022, ops, OpMix{});

  ASSERT_GT(fast.trades.size(), 0U);
  EXPECT_EQ(fast.trades, reference.trades);
  EXPECT_EQ(fast.digest, reference.digest);
}

TEST(CrossEngine, CrossingHeavyFlowGivesIdenticalTradesAndDigest) {
  constexpr Config config{100, 200, 64, 16};
  const std::uint64_t ops = kSanitized ? 20000 : 200000;

  OpMix mix;
  mix.submit_weight = 55;
  mix.cancel_weight = 15;
  mix.modify_weight = 30;
  mix.crossing_weight = 55;
  mix.market_weight = 10;
  mix.ioc_weight = 15;

  const Replay fast = replay<OrderBook>(config, 20261023, ops, mix);
  const Replay reference = replay<ReferenceBook>(config, 20261023, ops, mix);

  ASSERT_GT(fast.trades.size(), 0U);
  EXPECT_EQ(fast.trades, reference.trades);
  EXPECT_EQ(fast.digest, reference.digest);
}

}  // namespace
