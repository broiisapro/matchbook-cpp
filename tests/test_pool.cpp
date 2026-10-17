#include <gtest/gtest.h>

#include <cstdint>
#include <map>
#include <new>
#include <utility>

#include "lob/pool.hpp"

namespace {

using lob::ObjectPool;
using lob::PoolAllocator;

TEST(ObjectPool, AcquireAndReleaseFollowLifoOrder) {
  ObjectPool<int> pool(3);
  EXPECT_EQ(pool.capacity(), 3U);
  EXPECT_EQ(pool.size(), 0U);

  const std::uint32_t first = pool.acquire();
  const std::uint32_t second = pool.acquire();
  const std::uint32_t third = pool.acquire();
  EXPECT_EQ(first, 0U);
  EXPECT_EQ(second, 1U);
  EXPECT_EQ(third, 2U);
  EXPECT_EQ(pool.size(), 3U);

  EXPECT_EQ(pool.acquire(), ObjectPool<int>::kNull);

  pool.release(first);
  pool.release(third);
  EXPECT_EQ(pool.acquire(), third);
  EXPECT_EQ(pool.acquire(), first);
  EXPECT_EQ(pool.acquire(), ObjectPool<int>::kNull);
}

TEST(ObjectPool, AcquireNeverAllocatesWhenExhausted) {
  ObjectPool<int> pool(1);
  EXPECT_NE(pool.acquire(), ObjectPool<int>::kNull);
  EXPECT_EQ(pool.acquire(), ObjectPool<int>::kNull);
  EXPECT_EQ(pool.size(), 1U);
}

TEST(ObjectPool, IndicesStayStableAcrossReuse) {
  ObjectPool<int> pool(3);
  const std::uint32_t a = pool.acquire();
  const std::uint32_t b = pool.acquire();
  const std::uint32_t c = pool.acquire();
  pool[a] = 10;
  pool[b] = 20;
  pool[c] = 30;

  pool.release(b);
  const std::uint32_t reused = pool.acquire();
  EXPECT_EQ(reused, b);
  pool[reused] = 21;

  EXPECT_EQ(pool[a], 10);
  EXPECT_EQ(pool[b], 21);
  EXPECT_EQ(pool[c], 30);
}

TEST(ObjectPool, ZeroCapacityIsUsableButEmpty) {
  ObjectPool<int> pool(0);
  EXPECT_EQ(pool.capacity(), 0U);
  EXPECT_EQ(pool.acquire(), ObjectPool<int>::kNull);
  EXPECT_EQ(pool.size(), 0U);
}

TEST(ObjectPool, WriteThroughOperatorIndexSurvivesReconfiguration) {
  ObjectPool<int> pool(2);
  const std::uint32_t index = pool.acquire();
  pool[index] = 7;
  EXPECT_EQ(pool[index], 7);
  pool.release(index);
  const std::uint32_t again = pool.acquire();
  EXPECT_EQ(again, index);
  pool[again] = 8;
  EXPECT_EQ(pool[again], 8);
}

using Key = std::int64_t;
using Mapped = int;
using PoolMap = std::map<Key, Mapped, std::less<>, PoolAllocator<std::pair<const Key, Mapped>>>;
using PairAlloc = PoolAllocator<std::pair<const Key, Mapped>>;

TEST(PoolAllocator, MapAcceptsCapacityNodesAndReusesThem) {
  constexpr std::uint32_t kCapacity = 64;
  const auto pool = lob::detail::make_node_pool_for_map<PoolMap>(kCapacity);
  PoolMap map{std::less<>{}, PairAlloc{pool.get()}};

  for (std::uint32_t i = 0; i < kCapacity; ++i) {
    const auto [it, inserted] = map.emplace(static_cast<Key>(i), static_cast<Mapped>(i));
    ASSERT_TRUE(inserted);
    EXPECT_EQ(it->second, static_cast<Mapped>(i));
  }
  EXPECT_EQ(pool->size(), kCapacity);
  EXPECT_EQ(map.size(), kCapacity);

  EXPECT_THROW(map.emplace(Key{10000}, Mapped{0}), std::bad_alloc);
  EXPECT_EQ(map.size(), kCapacity);

  map.clear();
  EXPECT_EQ(pool->size(), 0U);
  EXPECT_TRUE(map.empty());

  for (std::uint32_t i = 0; i < kCapacity; ++i) {
    const auto [it, inserted] = map.emplace(static_cast<Key>(i) * 2, static_cast<Mapped>(i));
    ASSERT_TRUE(inserted);
    EXPECT_EQ(it->second, static_cast<Mapped>(i));
  }
  EXPECT_EQ(pool->size(), kCapacity);
}

TEST(PoolAllocator, MultiNodeRequestsThrow) {
  const auto pool = lob::detail::make_node_pool_for_map<PoolMap>(4);
  PairAlloc allocator{pool.get()};
  EXPECT_THROW(allocator.allocate(2), std::bad_alloc);
  EXPECT_THROW(allocator.allocate(0), std::bad_alloc);
}

TEST(PoolAllocator, DefaultConstructedAllocatorThrows) {
  PairAlloc allocator;
  EXPECT_EQ(allocator.pool(), nullptr);
  EXPECT_THROW(allocator.allocate(1), std::bad_alloc);
}

TEST(PoolAllocator, EqualityFollowsTheControlBlock) {
  const auto first_pool = lob::detail::make_node_pool_for_map<PoolMap>(4);
  const auto second_pool = lob::detail::make_node_pool_for_map<PoolMap>(4);
  const PairAlloc first{first_pool.get()};
  const PairAlloc first_copy{first};
  const PairAlloc second{second_pool.get()};

  EXPECT_TRUE(first == first_copy);
  EXPECT_FALSE(first == second);
  EXPECT_EQ(first.pool(), first_copy.pool());

  const PoolAllocator<const PairAlloc::value_type> rebound{first};
  EXPECT_EQ(rebound.pool(), first.pool());
  EXPECT_TRUE(rebound == first);
}

TEST(PoolAllocator, ExhaustedPoolThrowsBadAlloc) {
  const auto pool = lob::detail::make_node_pool_for_map<PoolMap>(1);
  PairAlloc allocator{pool.get()};
  void* first = allocator.allocate(1);
  ASSERT_NE(first, nullptr);
  EXPECT_THROW(allocator.allocate(1), std::bad_alloc);
  allocator.deallocate(static_cast<PairAlloc::value_type*>(first), 1);
  EXPECT_NO_THROW(static_cast<void>(allocator.allocate(1)));
}

}  // namespace
