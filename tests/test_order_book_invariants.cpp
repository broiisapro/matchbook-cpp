#include <gtest/gtest.h>

#include <bit>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string>
#include <vector>

#include "lob/order_book.hpp"
#include "support/recording_listener.hpp"

#if defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
#define LOB_INVARIANT_TEST_SANITIZED 1
#endif
#endif

#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define LOB_INVARIANT_TEST_SANITIZED 1
#endif

#ifndef LOB_INVARIANT_TEST_SANITIZED
#define LOB_INVARIANT_TEST_SANITIZED 0
#endif

namespace {

using lob::Config;
using lob::OrderBook;
using lob::OrderId;
using lob::OrderType;
using lob::Price;
using lob::Qty;
using lob::Side;

/// Deterministic splitmix64 stream, so the operation stream is identical on
/// every standard library.
class Rng {
 public:
  explicit Rng(std::uint64_t seed) : state_(seed) {}

  std::uint64_t next() {
    std::uint64_t value = (state_ += 0x9e3779b97f4a7c15ULL);
    value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31);
  }

 private:
  std::uint64_t state_;
};

struct Op {
  enum class Kind : std::uint8_t { Submit, Cancel, Modify };

  Kind kind = Kind::Submit;
  OrderId id = 0;
  Side side = Side::Buy;
  OrderType type = OrderType::Limit;
  Price price = 0;
  Qty qty = 0;
};

void apply(const Op& op, OrderBook& book) {
  switch (op.kind) {
    case Op::Kind::Submit:
      book.submit(op.id, op.side, op.type, op.price, op.qty);
      return;
    case Op::Kind::Cancel:
      book.cancel(op.id);
      return;
    case Op::Kind::Modify:
      book.modify(op.id, op.price, op.qty);
      return;
  }
}

/// Local seeded generator: a small id pool makes cancels and modifies hit
/// live orders often, while a share of unknown ids, invalid quantities, IOC
/// and market orders keeps the reject and cancel paths in the mix.
std::vector<Op> make_operations(std::uint64_t seed, std::size_t count, const Config& config) {
  constexpr std::uint64_t kIdPool = 256;
  Rng rng(seed);
  std::vector<Op> ops;
  ops.reserve(count);
  for (std::size_t index = 0; index < count; ++index) {
    const std::uint64_t roll = rng.next() % 100;
    Op op;
    op.side = (rng.next() & 1) != 0 ? Side::Buy : Side::Sell;
    if (roll < 8) {
      op.id = rng.next();  // unknown most of the time
    } else {
      op.id = rng.next() % kIdPool;
    }
    if (roll < 45) {
      op.price = config.min_price + static_cast<Price>(rng.next() % 401);
      op.qty = static_cast<Qty>(1 + rng.next() % 50);
      if (roll < 4) {
        op.qty = 0;  // InvalidQuantity
      }
      op.type = OrderType::Limit;
    } else if (roll < 53) {
      op.type = OrderType::Market;
      op.qty = static_cast<Qty>(1 + rng.next() % 50);
    } else if (roll < 62) {
      op.type = OrderType::IOC;
      op.price = config.min_price + static_cast<Price>(rng.next() % 600);
      op.qty = static_cast<Qty>(1 + rng.next() % 50);
    } else if (roll < 86) {
      op.kind = Op::Kind::Cancel;
    } else {
      op.kind = Op::Kind::Modify;
      op.price = config.min_price + static_cast<Price>(rng.next() % 401);
      op.qty = static_cast<Qty>(1 + rng.next() % 30);
    }
    ops.push_back(op);
  }
  return ops;
}

}  // namespace

namespace lob {

/// Invariant checker shared by the engine tests. It is a friend of OrderBook
/// and ObjectPool, so it can inspect the queues, the id map and the free list
/// directly; the library itself never sees this code.
struct OrderBookTestAccess {
  static void check(const OrderBook& book) {
    bool ok = true;
    const auto fail = [&ok](const char* message) {
      ADD_FAILURE() << message;
      ok = false;
    };

    const std::size_t capacity = book.orders_.capacity();
    std::vector<std::uint32_t> live;
    std::vector<bool> seen(capacity, false);

    for (const Side side : {Side::Buy, Side::Sell}) {
      book.ladder(side).for_each_best_first([&](Price price, const Level& level) {
        if (!ok) {
          return false;
        }
        if (level.head == kNullIndex || level.tail == kNullIndex) {
          fail("an empty level must not exist");
          return false;
        }
        std::uint64_t sum_qty = 0;
        std::uint32_t count = 0;
        std::uint32_t previous = kNullIndex;
        std::uint32_t index = level.head;
        while (index != kNullIndex) {
          if (index >= capacity) {
            fail("queue link out of pool range");
            return false;
          }
          if (seen[index]) {
            fail("order index appears twice");
            return false;
          }
          seen[index] = true;
          live.push_back(index);
          const OrderBook::OrderNode& node = book.orders_[index];
          if (node.side != side || node.price != price) {
            fail("order does not belong to its level");
            return false;
          }
          if (node.qty <= 0) {
            fail("live order with non-positive quantity");
            return false;
          }
          if (node.prev != previous) {
            fail("prev link does not match the previous order");
            return false;
          }
          if (book.ids_.find(node.id) != index) {
            fail("live order is missing from the id map");
            return false;
          }
          sum_qty += static_cast<std::uint64_t>(node.qty);
          ++count;
          previous = index;
          index = node.next;
        }
        if (previous != level.tail) {
          fail("tail does not match the forward walk");
          return false;
        }
        if (count != level.order_count || sum_qty != static_cast<std::uint64_t>(level.total_qty)) {
          fail("level aggregates do not match the queue");
          return false;
        }

        std::uint32_t reverse_count = 0;
        std::uint32_t next = kNullIndex;
        std::uint32_t reverse = level.tail;
        while (reverse != kNullIndex) {
          if (reverse_count >= count) {
            fail("prev links form a cycle");
            return false;
          }
          const OrderBook::OrderNode& node = book.orders_[reverse];
          if (node.next != next) {
            fail("next link does not match the backward walk");
            return false;
          }
          next = reverse;
          reverse = node.prev;
          ++reverse_count;
        }
        if (reverse_count != count) {
          fail("prev links do not cover the queue");
          return false;
        }
        return true;
      });
    }
    if (!ok) {
      return;
    }

    if (live.size() != book.orders_.size()) {
      fail("queue entries do not match the pool's live count");
    }
    if (book.orders_.size() != book.ids_.size()) {
      fail("id map size does not match live orders");
    }
    if (book.order_count() != live.size()) {
      fail("order_count does not match the live orders");
    }

    std::uint32_t free_count = 0;
    std::uint32_t index = book.orders_.free_head_;
    while (index != kNullIndex) {
      if (index >= capacity) {
        fail("free-list link out of pool range");
        break;
      }
      if (seen[index]) {
        fail("a live order is also on the free list");
        break;
      }
      seen[index] = true;
      ++free_count;
      if (free_count > capacity) {
        fail("free list does not terminate");
        break;
      }
      index = book.orders_.slots_[index].next;
    }
    if (free_count + book.orders_.size() != capacity) {
      fail("free list plus live orders does not cover the pool");
    }

    const std::optional<Price> best_bid = book.best_bid();
    const std::optional<Price> best_ask = book.best_ask();
    if (best_bid.has_value() && best_ask.has_value() && *best_bid >= *best_ask) {
      fail("book is crossed");
    }

#if defined(LOB_PRICE_LADDER_ARRAY)
    check_array_bitmap(book.ladder(Side::Buy), fail);
    check_array_bitmap(book.ladder(Side::Sell), fail);
#endif
  }

#if defined(LOB_PRICE_LADDER_ARRAY)
  /// Checks the array ladder's bitmap invariants against the engine's live
  /// levels: every occupancy word agrees with its summary bit, every occupied
  /// bit names a non-empty level, the count of occupied bits matches
  /// level_count(), and the tracked best is the highest bid / lowest ask.
  /// O(word_count + levels) per side.
  template <class Fail>
  static void check_array_bitmap(const OrderBook::Ladder& ladder, const Fail& fail) {
    std::uint32_t occupied_levels = 0;
    bool have_true_best = false;
    std::size_t true_best = 0;

    for (std::size_t word = 0; word < ladder.word_count_; ++word) {
      const std::uint64_t bits = ladder.words_[word];
      const bool summary_bit =
          (ladder.summary_[word >> 6] & (std::uint64_t{1} << (word & 63))) != 0;
      if (summary_bit != (bits != 0)) {
        fail("summary bit does not match its occupancy word");
        return;
      }
      std::uint64_t remaining = bits;
      while (remaining != 0) {
        const std::size_t index = (word << 6) + std::countr_zero(remaining);
        remaining &= remaining - 1;
        if (index >= ladder.range_) {
          fail("occupied bit past the price range");
          return;
        }
        const Level& level = ladder.levels_[index];
        if (level.head == kNullIndex || level.tail == kNullIndex) {
          fail("an occupied level must not be empty");
          return;
        }
        if (level.order_count == 0 || level.total_qty <= 0) {
          fail("an occupied level must hold orders");
          return;
        }
        ++occupied_levels;
        if (!have_true_best ||
            (ladder.side_ == Side::Buy ? index > true_best : index < true_best)) {
          true_best = index;
          have_true_best = true;
        }
      }
    }

    if (occupied_levels != ladder.level_count_) {
      fail("level_count does not match the number of occupied bits");
      return;
    }
    if (have_true_best != ladder.has_best_) {
      fail("tracked best presence does not match the bitmap");
      return;
    }
    if (have_true_best && ladder.best_index_ != true_best) {
      fail("tracked best does not match the highest/lowest occupied index");
      return;
    }
    if (!have_true_best && ladder.best() != nullptr) {
      fail("an empty side must not report a best level");
    }
  }

  /// Full-slot scan: every slot outside the bitmap must be a fresh, empty
  /// Level. O(range); it is only called from the small hand-built book test,
  /// never from the per-operation stream.
  static void check_array_slots_clean(const OrderBook& book) {
    for (const Side side : {Side::Buy, Side::Sell}) {
      const OrderBook::Ladder& ladder = book.ladder(side);
      for (std::size_t index = 0; index < ladder.range_; ++index) {
        const bool occupied = (ladder.words_[index >> 6] & (std::uint64_t{1} << (index & 63))) != 0;
        if (occupied) {
          continue;
        }
        const Level& level = ladder.levels_[index];
        if (level.head != kNullIndex || level.tail != kNullIndex || level.total_qty != 0 ||
            level.order_count != 0) {
          ADD_FAILURE() << "a slot outside the bitmap is not a fresh empty level";
          return;
        }
      }
    }
  }
#endif
};

}  // namespace lob

namespace {

using lob::OrderBookTestAccess;

TEST(OrderBookInvariants, HoldAfterEveryOperationOfFiveSeededStreams) {
#if LOB_INVARIANT_TEST_SANITIZED
  constexpr std::uint64_t kOpsPerSeed = 20000;
#else
  constexpr std::uint64_t kOpsPerSeed = 200000;
#endif
  const Config config{1, 1000, 256, 32};
  const std::vector<std::uint64_t> seeds = {11, 22, 33, 44, 55};

  for (const std::uint64_t seed : seeds) {
    lob::test::RecordingListener listener;
    OrderBook book(config, listener);
    const std::vector<Op> ops = make_operations(seed, kOpsPerSeed, config);
    ASSERT_EQ(ops.size(), kOpsPerSeed);

    for (std::size_t index = 0; index < ops.size(); ++index) {
      apply(ops[index], book);
      OrderBookTestAccess::check(book);
      if (::testing::Test::HasFailure()) {
        ADD_FAILURE() << "invariant failure on seed " << seed << " at operation " << index;
        break;
      }
    }
    SUCCEED();
  }
}

TEST(OrderBookInvariants, CheckerPassesOnAHealthyBook) {
  // The checker runs over a small but non-trivial book built by hand, so a
  // failure here points at the checker itself rather than the random stream.
  lob::test::RecordingListener listener;
  OrderBook book(Config{1, 1000, 8, 4}, listener);
  book.submit(1, Side::Buy, OrderType::Limit, 100, 3);
  book.submit(2, Side::Buy, OrderType::Limit, 100, 4);
  book.submit(3, Side::Sell, OrderType::Limit, 200, 5);
  book.submit(4, Side::Sell, OrderType::Limit, 300, 6);
  book.cancel(2);
  book.modify(3, 200, 2);
  OrderBookTestAccess::check(book);
#if defined(LOB_PRICE_LADDER_ARRAY)
  OrderBookTestAccess::check_array_slots_clean(book);
#endif
  EXPECT_EQ(book.order_count(), 3U);
}

#if defined(NDEBUG)

TEST(OrderBookReentrancy, ListenerCallbackReentryTripsTheDebugAssert) {
  GTEST_SKIP() << "reentrancy is asserted in debug builds only";
}

#else

class ReentrantListener final : public lob::Listener {
 public:
  OrderBook* book = nullptr;

  void on_accept(OrderId) override { static_cast<void>(book->best_bid()); }
  void on_reject(OrderId, lob::RejectReason) override {}
  void on_trade(OrderId, OrderId, Price, Qty) override {}
  void on_cancel(OrderId, Qty) override {}
  void on_modify(OrderId, Price, Qty) override {}
  void on_book_update(Side, Price, Qty) override {}
};

TEST(OrderBookReentrancy, ListenerCallbackReentryTripsTheDebugAssert) {
  EXPECT_DEATH(
      {
        ReentrantListener listener;
        OrderBook book(Config{1, 1000, 16, 16}, listener);
        listener.book = &book;
        book.submit(1, Side::Buy, OrderType::Limit, 100, 1);
      },
      "reentrancy");
}

#endif

}  // namespace
