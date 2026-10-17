// Self-tests for the differential harness. The harness must report zero
// mismatches when both engines are the reference engine, and must detect each
// injected fault at the exact operation index where it first shows up:
//   - a dropped on_book_update event (event-stream difference),
//   - a trade price moved by one tick (event-stream difference),
//   - two orders swapped in the digest walk (digest-only difference).

#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <span>
#include <variant>

#include "lob/config.hpp"
#include "lob/order_book.hpp"
#include "lob/types.hpp"
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

using lob::Config;
using lob::OrderId;
using lob::OrderType;
using lob::Price;
using lob::Qty;
using lob::RejectReason;
using lob::Side;
using lob::opgen::apply_op;
using lob::opgen::DiffResult;
using lob::opgen::Op;
using lob::opgen::OpGenerator;
using lob::opgen::run_differential;
using lob::ref::ReferenceBook;

constexpr bool kSanitized =
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
    true;
#else
    LOB_HAS_FEATURE(address_sanitizer) || LOB_HAS_FEATURE(thread_sanitizer);
#endif

constexpr std::uint64_t kScaleOps = kSanitized ? 20000 : 1000000;
// Small capacities keep the book (and therefore the per-op digest walk) small
// so one million operations with a digest comparison after every op stay fast.
constexpr Config kScaleConfig{100, 200, 100, 25};

enum class Fault { None, DropBookUpdate, ShiftTradePrice, PerturbDigest };

/// Fault parameters read by FaultyBook when it is constructed inside
/// run_differential, which builds engines itself. Tests run sequentially in
/// one binary, so a single static schedule is enough.
struct TestFault {
  static Fault fault;
  static std::uint64_t at_index;
};

Fault TestFault::fault = Fault::None;
std::uint64_t TestFault::at_index = 0;

/// Digest xor applied from the fault index on. It simulates the digest-only
/// difference a swapped pair of queue positions would cause: events stay
/// identical, the book digest changes.
constexpr std::uint64_t kQueueSwapSalt = 0x9E3779B97F4A7C15ULL;

/// Reference engine with an injected fault, same public API.
class FaultyBook {
 public:
  FaultyBook(const Config& config, lob::Listener& sink)
      : proxy_(sink, TestFault::fault, TestFault::at_index),
        book_(config, proxy_),
        fault_(TestFault::fault),
        at_index_(TestFault::at_index) {}

  void submit(OrderId id, Side side, OrderType type, Price price, Qty qty) {
    proxy_.current_op = ops_;
    book_.submit(id, side, type, price, qty);
    ++ops_;
  }

  void cancel(OrderId id) {
    proxy_.current_op = ops_;
    book_.cancel(id);
    ++ops_;
  }

  void modify(OrderId id, Price new_price, Qty new_qty) {
    proxy_.current_op = ops_;
    book_.modify(id, new_price, new_qty);
    ++ops_;
  }

  std::optional<Price> best_bid() const { return book_.best_bid(); }
  std::optional<Price> best_ask() const { return book_.best_ask(); }
  Qty level_qty(Side side, Price price) const { return book_.level_qty(side, price); }
  std::size_t top_levels(Side side, std::span<lob::LevelInfo> out) const {
    return book_.top_levels(side, out);
  }
  std::size_t order_count() const { return book_.order_count(); }

  std::uint64_t digest() const {
    const std::uint64_t value = book_.digest();
    if (fault_ == Fault::PerturbDigest && ops_ > at_index_) {
      return value ^ kQueueSwapSalt;
    }
    return value;
  }

 private:
  class Proxy final : public lob::Listener {
   public:
    Proxy(lob::Listener& sink, Fault fault, std::uint64_t at_index)
        : sink_(sink), fault_(fault), at_index_(at_index) {}

    void on_accept(OrderId id) override { sink_.on_accept(id); }
    void on_reject(OrderId id, RejectReason reason) override { sink_.on_reject(id, reason); }

    void on_trade(OrderId maker_id, OrderId taker_id, Price price, Qty qty) override {
      if (fault_ == Fault::ShiftTradePrice && current_op >= at_index_ && !fired_) {
        fired_ = true;
        sink_.on_trade(maker_id, taker_id, price + 1, qty);
        return;
      }
      sink_.on_trade(maker_id, taker_id, price, qty);
    }

    void on_cancel(OrderId id, Qty cancelled_qty) override { sink_.on_cancel(id, cancelled_qty); }

    void on_modify(OrderId id, Price price, Qty new_qty) override {
      sink_.on_modify(id, price, new_qty);
    }

    void on_book_update(Side side, Price price, Qty new_level_qty) override {
      if (fault_ == Fault::DropBookUpdate && current_op >= at_index_ && !fired_) {
        fired_ = true;
        return;
      }
      sink_.on_book_update(side, price, new_level_qty);
    }

    std::uint64_t current_op = 0;

   private:
    lob::Listener& sink_;
    Fault fault_;
    std::uint64_t at_index_;
    bool fired_ = false;
  };

  Proxy proxy_;
  ReferenceBook book_;
  Fault fault_;
  std::uint64_t at_index_;
  std::uint64_t ops_ = 0;
};

bool has_book_update(const std::vector<lob::test::Event>& events) {
  for (const auto& event : events) {
    if (std::holds_alternative<lob::test::BookUpdate>(event)) {
      return true;
    }
  }
  return false;
}

bool has_trade(const std::vector<lob::test::Event>& events) {
  for (const auto& event : events) {
    if (std::holds_alternative<lob::test::Trade>(event)) {
      return true;
    }
  }
  return false;
}

/// Index of the first operation whose events satisfy `predicate`, or `ops`
/// when there is none.
template <class Predicate>
std::uint64_t first_index_matching(std::uint64_t seed, std::uint64_t ops, Predicate predicate) {
  lob::test::RecordingListener listener;
  ReferenceBook book(kScaleConfig, listener);
  OpGenerator generator(seed, kScaleConfig);
  for (std::uint64_t index = 0; index < ops; ++index) {
    listener.clear();
    apply_op(book, generator.next());
    if (predicate(listener.events)) {
      return index;
    }
  }
  return ops;
}

TEST(DifferentialHarness, ReferenceMatchesReferenceAtScale) {
  TestFault::fault = Fault::None;
  TestFault::at_index = 0;

  OpGenerator generator(20261017, kScaleConfig);
  const DiffResult result =
      run_differential<ReferenceBook, ReferenceBook>(kScaleConfig, generator, kScaleOps);

  EXPECT_EQ(result.mismatches, 0U);
  EXPECT_EQ(result.ops_run, kScaleOps);
}

TEST(DifferentialHarness, DroppedBookUpdateIsDetectedAtThatOperation) {
  constexpr std::uint64_t kOps = 2000;
  const std::uint64_t at = first_index_matching(5, kOps, has_book_update);
  ASSERT_LT(at, kOps);

  TestFault::fault = Fault::DropBookUpdate;
  TestFault::at_index = at;

  OpGenerator generator(5, kScaleConfig);
  const DiffResult result =
      run_differential<ReferenceBook, FaultyBook>(kScaleConfig, generator, kOps);

  EXPECT_EQ(result.first_mismatch_index, at);
  EXPECT_EQ(result.mismatches, 1U);
  EXPECT_TRUE(result.events_differ);
  EXPECT_FALSE(result.digest_differ);
  ASSERT_EQ(result.events_a.size(), result.events_b.size() + 1);
}

TEST(DifferentialHarness, ShiftedTradePriceIsDetectedAtThatOperation) {
  constexpr std::uint64_t kOps = 2000;
  const std::uint64_t at = first_index_matching(5, kOps, has_trade);
  ASSERT_LT(at, kOps);

  TestFault::fault = Fault::ShiftTradePrice;
  TestFault::at_index = at;

  OpGenerator generator(5, kScaleConfig);
  const DiffResult result =
      run_differential<ReferenceBook, FaultyBook>(kScaleConfig, generator, kOps);

  EXPECT_EQ(result.first_mismatch_index, at);
  EXPECT_EQ(result.mismatches, 1U);
  EXPECT_TRUE(result.events_differ);
  EXPECT_FALSE(result.digest_differ);
}

TEST(DifferentialHarness, SwappedQueuePositionsChangeOnlyTheDigest) {
  constexpr std::uint64_t kOps = 100;
  constexpr std::uint64_t kAt = 17;

  TestFault::fault = Fault::PerturbDigest;
  TestFault::at_index = kAt;

  OpGenerator generator(5, kScaleConfig);
  const DiffResult result =
      run_differential<ReferenceBook, FaultyBook>(kScaleConfig, generator, kOps);

  EXPECT_EQ(result.first_mismatch_index, kAt);
  EXPECT_EQ(result.mismatches, kOps - kAt);
  EXPECT_FALSE(result.events_differ);
  EXPECT_TRUE(result.digest_differ);
  EXPECT_EQ(result.events_a, result.events_b);
  EXPECT_NE(result.digest_a, result.digest_b);
}

}  // namespace
