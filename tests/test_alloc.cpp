#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <new>
#include <vector>

#include "lob/order_book.hpp"

#if defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
#define LOB_ALLOC_TEST_SANITIZED 1
#endif
#endif

#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define LOB_ALLOC_TEST_SANITIZED 1
#endif

#ifndef LOB_ALLOC_TEST_SANITIZED
#define LOB_ALLOC_TEST_SANITIZED 0
#endif

// This binary replaces every global allocation function, so it must not share
// a process with tests that assume the default allocator; CMake keeps it as a
// separate `test_alloc` executable.
namespace {

std::atomic<std::uint64_t> g_allocation_count{0};
std::atomic<bool> g_armed{false};

void* allocate_bytes(std::size_t size, std::size_t alignment) {
  if (size == 0) {
    size = 1;
  }
  if (g_armed.load(std::memory_order_relaxed)) {
    g_allocation_count.fetch_add(1, std::memory_order_relaxed);
  }
  void* memory = nullptr;
  if (alignment <= __STDCPP_DEFAULT_NEW_ALIGNMENT__) {
    memory = std::malloc(size);
  } else {
    const std::size_t rounded = (size + alignment - 1) / alignment * alignment;
    memory = std::aligned_alloc(alignment, rounded);
  }
  if (memory == nullptr) {
    throw std::bad_alloc{};
  }
  return memory;
}

std::uint64_t allocation_count() { return g_allocation_count.load(std::memory_order_relaxed); }

void reset_count() { g_allocation_count.store(0, std::memory_order_relaxed); }

void arm() { g_armed.store(true, std::memory_order_relaxed); }

void disarm() { g_armed.store(false, std::memory_order_relaxed); }

}  // namespace

void* operator new(std::size_t size) {
  return allocate_bytes(size, __STDCPP_DEFAULT_NEW_ALIGNMENT__);
}

void* operator new[](std::size_t size) {
  return allocate_bytes(size, __STDCPP_DEFAULT_NEW_ALIGNMENT__);
}

void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
  try {
    return allocate_bytes(size, __STDCPP_DEFAULT_NEW_ALIGNMENT__);
  } catch (...) {
    return nullptr;
  }
}

void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
  try {
    return allocate_bytes(size, __STDCPP_DEFAULT_NEW_ALIGNMENT__);
  } catch (...) {
    return nullptr;
  }
}

void* operator new(std::size_t size, std::align_val_t alignment) {
  return allocate_bytes(size, static_cast<std::size_t>(alignment));
}

void* operator new[](std::size_t size, std::align_val_t alignment) {
  return allocate_bytes(size, static_cast<std::size_t>(alignment));
}

void* operator new(std::size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept {
  try {
    return allocate_bytes(size, static_cast<std::size_t>(alignment));
  } catch (...) {
    return nullptr;
  }
}

void* operator new[](std::size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept {
  try {
    return allocate_bytes(size, static_cast<std::size_t>(alignment));
  } catch (...) {
    return nullptr;
  }
}

void operator delete(void* pointer) noexcept { std::free(pointer); }

void operator delete[](void* pointer) noexcept { std::free(pointer); }

void operator delete(void* pointer, const std::nothrow_t&) noexcept { std::free(pointer); }

void operator delete[](void* pointer, const std::nothrow_t&) noexcept { std::free(pointer); }

void operator delete(void* pointer, std::size_t) noexcept { std::free(pointer); }

void operator delete[](void* pointer, std::size_t) noexcept { std::free(pointer); }

void operator delete(void* pointer, std::align_val_t) noexcept { std::free(pointer); }

void operator delete[](void* pointer, std::align_val_t) noexcept { std::free(pointer); }

void operator delete(void* pointer, std::size_t, std::align_val_t) noexcept { std::free(pointer); }

void operator delete[](void* pointer, std::size_t, std::align_val_t) noexcept {
  std::free(pointer);
}

void operator delete(void* pointer, std::align_val_t, const std::nothrow_t&) noexcept {
  std::free(pointer);
}

void operator delete[](void* pointer, std::align_val_t, const std::nothrow_t&) noexcept {
  std::free(pointer);
}

namespace {

using lob::OrderBook;
using lob::OrderId;
using lob::OrderType;
using lob::Price;
using lob::Qty;
using lob::Side;

/// Deterministic splitmix64 stream; no standard distribution is used, so the
/// operation stream is the same on every standard library.
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
  enum class Kind : std::uint8_t {
    Submit,
    Cancel,
    Modify,
  };

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

/// Builds a seeded mixed stream. Each round builds a few resting orders on
/// both sides and then exercises every operation kind against them: duplicate
/// submits, in-place reduces, price moves, crossing limits, market and IOC
/// orders, cancels of live and unknown ids, and modifies of unknown ids. The
/// book is drained again by the crossing orders and cancels, so it never
/// saturates on a long run.
std::vector<Op> make_operations(std::uint64_t seed, std::size_t count) {
  Rng rng(seed);
  std::vector<Op> ops;
  ops.reserve(count);
  OrderId next_id = 1;

  const auto jitter = [&rng](Price base, Price spread) {
    return static_cast<Price>(base + rng.next() % spread);
  };
  const auto quantity = [&rng]() { return static_cast<Qty>(1 + rng.next() % 64); };

  while (ops.size() < count) {
    const OrderId bid0 = next_id++;
    const OrderId ask0 = next_id++;
    const OrderId bid1 = next_id++;
    const OrderId ask1 = next_id++;
    const OrderId taker = next_id++;
    const Price bid_price0 = jitter(1000, 256);
    const Price ask_price0 = jitter(3000, 256);

    Op submit_bid0{Op::Kind::Submit, bid0, Side::Buy, OrderType::Limit, bid_price0, quantity()};
    ops.push_back(submit_bid0);
    ops.push_back(submit_bid0);  // duplicate id -> DuplicateId
    ops.push_back(
        Op{Op::Kind::Modify, bid0, Side::Buy, OrderType::Limit, bid_price0, quantity() / 2});
    ops.push_back(Op{Op::Kind::Submit, ask0, Side::Sell, OrderType::Limit, ask_price0, quantity()});
    ops.push_back(Op{Op::Kind::Modify, ask0, Side::Sell, OrderType::Limit,
                     ask_price0 + jitter(0, 256), quantity()});
    ops.push_back(Op{Op::Kind::Modify, ask0, Side::Sell, OrderType::Limit, 0, quantity()});
    ops.push_back(
        Op{Op::Kind::Submit, bid1, Side::Buy, OrderType::Limit, jitter(500, 256), quantity()});
    ops.push_back(
        Op{Op::Kind::Modify, bid1, Side::Buy, OrderType::Limit, jitter(400, 256), quantity()});
    ops.push_back(
        Op{Op::Kind::Submit, ask1, Side::Sell, OrderType::Limit, jitter(4000, 256), quantity()});
    ops.push_back(
        Op{Op::Kind::Modify, ask1, Side::Sell, OrderType::Limit, jitter(4000, 256), quantity()});

    ops.push_back(Op{Op::Kind::Submit, next_id++, Side::Buy, OrderType::Limit, 100000, quantity()});
    ops.push_back(Op{Op::Kind::Submit, next_id++, Side::Sell, OrderType::Limit, 1, quantity()});
    ops.push_back(Op{Op::Kind::Submit, next_id++, Side::Buy, OrderType::Market, 0, quantity()});
    ops.push_back(Op{Op::Kind::Submit, next_id++, Side::Sell, OrderType::Market, 0, quantity()});
    ops.push_back(Op{Op::Kind::Submit, next_id++, Side::Buy, OrderType::IOC, 100000, quantity()});
    ops.push_back(Op{Op::Kind::Submit, next_id++, Side::Sell, OrderType::IOC, 1, quantity()});

    ops.push_back(Op{Op::Kind::Cancel, bid0, Side::Buy, OrderType::Limit, 0, 0});
    ops.push_back(Op{Op::Kind::Cancel, ask0, Side::Sell, OrderType::Limit, 0, 0});
    ops.push_back(Op{Op::Kind::Cancel, bid1, Side::Buy, OrderType::Limit, 0, 0});
    ops.push_back(Op{Op::Kind::Cancel, ask1, Side::Sell, OrderType::Limit, 0, 0});
    ops.push_back(Op{Op::Kind::Cancel, 1000000000ULL + taker, Side::Buy, OrderType::Limit, 0, 0});
    ops.push_back(
        Op{Op::Kind::Modify, 2000000000ULL + taker, Side::Buy, OrderType::Limit, bid_price0, 1});
  }
  ops.resize(count);
  return ops;
}

TEST(Alloc, PositiveControlCountsEveryOperatorNewForm) {
#if LOB_ALLOC_TEST_SANITIZED
  GTEST_SKIP() << "allocation counting is unreliable when a sanitizer intercepts allocation";
#endif
  reset_count();
  arm();
  int* scalar = new int(7);
  int* array = new int[4];
  int* nothrow_scalar = new (std::nothrow) int(7);
  int* nothrow_array = new (std::nothrow) int[4];
  int* aligned_scalar = new (std::align_val_t{64}) int(7);
  int* aligned_array = new (std::align_val_t{64}) int[4];
  int* aligned_nothrow_scalar = new (std::align_val_t{64}, std::nothrow) int(7);
  int* aligned_nothrow_array = new (std::align_val_t{64}, std::nothrow) int[4];
  disarm();

  EXPECT_EQ(allocation_count(), 8U);
  ASSERT_NE(scalar, nullptr);
  ASSERT_NE(array, nullptr);
  ASSERT_NE(nothrow_scalar, nullptr);
  ASSERT_NE(nothrow_array, nullptr);
  ASSERT_NE(aligned_scalar, nullptr);
  ASSERT_NE(aligned_array, nullptr);
  ASSERT_NE(aligned_nothrow_scalar, nullptr);
  ASSERT_NE(aligned_nothrow_array, nullptr);

  ::operator delete(scalar);
  ::operator delete[](array);
  ::operator delete(nothrow_scalar, std::nothrow);
  ::operator delete[](nothrow_array, std::nothrow);
  ::operator delete(aligned_scalar, std::align_val_t{64});
  ::operator delete[](aligned_array, std::align_val_t{64});
  ::operator delete(aligned_nothrow_scalar, std::align_val_t{64}, std::nothrow);
  ::operator delete[](aligned_nothrow_array, std::align_val_t{64}, std::nothrow);
}

TEST(Alloc, ArmedCounterIsZeroBeforeArmingAndAfterDisarming) {
#if LOB_ALLOC_TEST_SANITIZED
  GTEST_SKIP() << "allocation counting is unreliable when a sanitizer intercepts allocation";
#endif
  reset_count();
  std::unique_ptr<int> outside(new int(1));
  EXPECT_EQ(allocation_count(), 0U);
  arm();
  disarm();
  outside.reset();
  EXPECT_EQ(allocation_count(), 0U);
}

TEST(Alloc, NoHeapAllocationOnHotPathAfterWarmup) {
#if LOB_ALLOC_TEST_SANITIZED
  GTEST_SKIP() << "allocation counting is unreliable when a sanitizer intercepts allocation";
#endif
  constexpr std::uint64_t kWarmupOps = 5000;
  constexpr std::uint64_t kMeasuredOps = 1000000;
  const lob::Config config{1, 100000, 100000, 10000};

  const std::vector<Op> ops = make_operations(0xA11C0A7EULL, kWarmupOps + kMeasuredOps);
  ASSERT_EQ(ops.size(), kWarmupOps + kMeasuredOps);

  lob::NullListener listener;
  OrderBook book(config, listener);

  for (std::uint64_t index = 0; index < kWarmupOps; ++index) {
    apply(ops[index], book);
  }

  reset_count();
  arm();
  for (std::uint64_t index = kWarmupOps; index < ops.size(); ++index) {
    apply(ops[index], book);
  }
  disarm();

  EXPECT_EQ(allocation_count(), 0U) << "the hot path allocated " << allocation_count()
                                    << " times in " << kMeasuredOps << " operations";
}

}  // namespace
