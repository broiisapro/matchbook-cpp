#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <unordered_map>
#include <vector>

#include "lob/id_map.hpp"

namespace {

constexpr std::uint64_t kUint64Max = std::numeric_limits<std::uint64_t>::max();

/// splitmix64, so the test's randomness is deterministic and independent of
/// the standard library's distributions.
class SplitMix64 {
 public:
  explicit SplitMix64(std::uint64_t seed) : state_(seed) {}

  std::uint64_t next() {
    std::uint64_t value = (state_ += 0x9e3779b97f4a7c15ULL);
    value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31);
  }

 private:
  std::uint64_t state_;
};

std::size_t home_of(std::uint64_t id, std::size_t capacity) {
  return static_cast<std::size_t>(lob::detail::hash_order_id(id)) & (capacity - 1);
}

TEST(IdMap, InsertFindEraseRoundTrip) {
  lob::IdMap map(64);
  EXPECT_EQ(map.size(), 0U);
  EXPECT_EQ(map.find(7), lob::IdMap::kNull);
  EXPECT_FALSE(map.erase(7));

  EXPECT_TRUE(map.insert(7, 3));
  EXPECT_FALSE(map.insert(7, 4));
  EXPECT_EQ(map.size(), 1U);
  EXPECT_EQ(map.find(7), 3U);
  EXPECT_EQ(map.find(8), lob::IdMap::kNull);

  EXPECT_TRUE(map.erase(7));
  EXPECT_EQ(map.size(), 0U);
  EXPECT_EQ(map.find(7), lob::IdMap::kNull);
  EXPECT_FALSE(map.erase(7));

  EXPECT_TRUE(map.insert(7, 9));
  EXPECT_EQ(map.find(7), 9U);
}

TEST(IdMap, ExtremalIdsAreValidKeys) {
  lob::IdMap map(16);
  EXPECT_TRUE(map.insert(0, 0));
  EXPECT_TRUE(map.insert(kUint64Max, 5));
  EXPECT_TRUE(map.insert(1, 1));

  EXPECT_EQ(map.find(0), 0U);
  EXPECT_EQ(map.find(kUint64Max), 5U);
  EXPECT_EQ(map.find(1), 1U);
  EXPECT_EQ(map.find(2), lob::IdMap::kNull);

  EXPECT_TRUE(map.erase(0));
  EXPECT_EQ(map.find(0), lob::IdMap::kNull);
  EXPECT_EQ(map.find(kUint64Max), 5U);
}

TEST(IdMap, BackwardShiftClosesAWrappingCluster) {
  lob::IdMap map(8);
  const std::size_t capacity = map.capacity();
  ASSERT_EQ(capacity, 16U);

  // Find ids that start their probe at the last slots of the table, so the
  // cluster wraps around the end, then delete from its middle.
  std::vector<std::uint64_t> homes(capacity, 0);
  std::vector<bool> found(capacity, false);
  std::size_t distinct_homes = 0;
  for (std::uint64_t candidate = 0; candidate < 1000000 && distinct_homes < capacity; ++candidate) {
    const std::size_t home = home_of(candidate, capacity);
    if (!found[home]) {
      homes[home] = candidate;
      found[home] = true;
      ++distinct_homes;
    }
  }
  ASSERT_EQ(distinct_homes, capacity);

  const std::uint64_t key_last = homes[capacity - 1];
  const std::uint64_t key_wrap = homes[0];
  const std::uint64_t key_second_last = homes[capacity - 2];
  const std::uint64_t key_one = homes[1];

  EXPECT_TRUE(map.insert(key_last, 10));
  EXPECT_TRUE(map.insert(key_wrap, 20));
  EXPECT_TRUE(map.insert(key_second_last, 30));
  EXPECT_TRUE(map.insert(key_one, 40));

  EXPECT_TRUE(map.erase(key_last));
  EXPECT_EQ(map.find(key_last), lob::IdMap::kNull);
  EXPECT_EQ(map.find(key_second_last), 30U);
  EXPECT_EQ(map.find(key_wrap), 20U);
  EXPECT_EQ(map.find(key_one), 40U);

  EXPECT_TRUE(map.erase(key_wrap));
  EXPECT_EQ(map.find(key_one), 40U);
  EXPECT_EQ(map.find(key_second_last), 30U);
  EXPECT_TRUE(map.erase(key_one));
  EXPECT_EQ(map.find(key_second_last), 30U);
}

TEST(IdMap, InsertFailsWhenTheTableIsFull) {
  lob::IdMap map(16);
  const std::size_t capacity = map.capacity();

  std::uint32_t inserted = 0;
  for (std::uint64_t id = 0; id < 1000000 && inserted < capacity; ++id) {
    if (map.insert(id, inserted)) {
      ++inserted;
    }
  }
  EXPECT_EQ(inserted, capacity);
  EXPECT_EQ(map.size(), capacity);

  EXPECT_FALSE(map.insert(kUint64Max, capacity - 1));
  EXPECT_FALSE(map.insert(0, 0));

  for (std::uint64_t id = 0; id < 1000000 && map.size() > 0; ++id) {
    map.erase(id);
  }
  EXPECT_EQ(map.size(), 0U);
}

TEST(IdMap, MatchesUnorderedMapModel) {
  constexpr std::uint32_t kMaxOrders = 4096;
  lob::IdMap map(kMaxOrders);
  std::unordered_map<std::uint64_t, std::uint32_t> model;
  SplitMix64 rng(0x5eed1234ULL);

  for (int operation = 0; operation < 200000; ++operation) {
    const std::uint64_t roll = rng.next() % 100;
    // At most 4098 distinct keys against a table of 8192 slots, so the full
    // table path is never hit here and an absent key always inserts.
    std::uint64_t id = rng.next() % 4096;
    if (roll < 4) {
      id = 0;
    } else if (roll < 8) {
      id = kUint64Max;
    }
    const std::uint32_t index = static_cast<std::uint32_t>(rng.next() % kMaxOrders);

    if (roll < 55) {
      const auto it = model.find(id);
      const bool expected = it == model.end();
      const bool inserted = map.insert(id, index);
      ASSERT_EQ(inserted, expected);
      if (inserted) {
        model.emplace(id, index);
        EXPECT_EQ(map.find(id), index);
      } else {
        EXPECT_EQ(map.find(id), it->second);
      }
    } else if (roll < 80) {
      const auto it = model.find(id);
      const std::uint32_t expected = it == model.end() ? lob::IdMap::kNull : it->second;
      EXPECT_EQ(map.find(id), expected);
    } else {
      const auto it = model.find(id);
      const bool expected = it != model.end();
      EXPECT_EQ(map.erase(id), expected);
      if (expected) {
        model.erase(it);
      }
    }
    ASSERT_EQ(map.size(), model.size());
  }
}

}  // namespace
