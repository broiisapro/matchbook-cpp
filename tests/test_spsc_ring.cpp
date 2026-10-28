#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <thread>
#include <type_traits>

#include "lob/spsc_ring.hpp"

namespace lob {

/// Test-only access to the ring's private layout. Declared as a friend by
/// SpscRing so the cache-line test can measure member offsets without exposing
/// the indices in the public interface.
template <class T, std::size_t Capacity>
struct SpscRingLayoutProbe {
  static std::size_t offset_of(const void* member, const SpscRing<T, Capacity>& ring) {
    return static_cast<std::size_t>(static_cast<const std::byte*>(member) -
                                    reinterpret_cast<const std::byte*>(&ring));
  }

  static std::size_t head_offset() {
    const SpscRing<T, Capacity> ring;
    return offset_of(&ring.head_, ring);
  }

  static std::size_t tail_offset() {
    const SpscRing<T, Capacity> ring;
    return offset_of(&ring.tail_, ring);
  }

  static std::size_t cached_head_offset() {
    const SpscRing<T, Capacity> ring;
    return offset_of(&ring.cached_head_, ring);
  }

  static std::size_t cached_tail_offset() {
    const SpscRing<T, Capacity> ring;
    return offset_of(&ring.cached_tail_, ring);
  }

  static std::size_t buffer_offset() {
    const SpscRing<T, Capacity> ring;
    return offset_of(&ring.buffer_, ring);
  }
};

}  // namespace lob

namespace {

using lob::SpscRing;

#if defined(__SANITIZE_THREAD__)
constexpr bool kUnderSanitizer = true;
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer) || __has_feature(address_sanitizer)
constexpr bool kUnderSanitizer = true;
#else
constexpr bool kUnderSanitizer = false;
#endif
#else
constexpr bool kUnderSanitizer = false;
#endif

/// Messages for the two-thread test. The payload is a bijective function of
/// the sequence number, so a reordered, duplicated, dropped or corrupted
/// message fails both the order check and the content check.
struct Message {
  std::uint64_t sequence;
  std::uint64_t payload;
};

std::uint64_t payload_for(std::uint64_t sequence) {
  std::uint64_t value = sequence * 0x9e3779b97f4a7c15ULL;
  value ^= (sequence << 17U) | (sequence >> 47U);
  value ^= value >> 31U;
  return value * 0xc2b2ae3d27d4eb4fULL;
}

// Compile-time contract: the ring only accepts trivially copyable elements, a
// power-of-two capacity of at least two, and exposes that capacity.
static_assert(SpscRing<std::uint32_t, 8>::capacity() == 8);
static_assert(std::is_trivially_copyable_v<Message>);

TEST(SpscRing, CapacityIsReportedAndStartIsEmpty) {
  SpscRing<std::uint32_t, 8> ring;
  EXPECT_EQ(ring.capacity(), 8U);
  EXPECT_EQ(ring.size_approx(), 0U);

  std::uint32_t value = 42;
  EXPECT_FALSE(ring.try_pop(value));
  EXPECT_EQ(value, 42U);  // a failed pop leaves the output untouched
}

TEST(SpscRing, PushAndPopAreFifo) {
  SpscRing<std::uint32_t, 8> ring;
  for (std::uint32_t value = 0; value < 5; ++value) {
    EXPECT_TRUE(ring.try_push(value));
  }
  EXPECT_EQ(ring.size_approx(), 5U);
  for (std::uint32_t expected = 0; expected < 5; ++expected) {
    std::uint32_t value = 0xFFFF;
    EXPECT_TRUE(ring.try_pop(value));
    EXPECT_EQ(value, expected);
  }
  std::uint32_t value = 0xFFFF;
  EXPECT_FALSE(ring.try_pop(value));
}

TEST(SpscRing, FullBoundaryRejectsExactlyAtCapacity) {
  SpscRing<std::uint32_t, 4> ring;
  for (std::uint32_t value = 0; value < 4; ++value) {
    EXPECT_TRUE(ring.try_push(value)) << "push " << value;
    EXPECT_EQ(ring.size_approx(), value + 1);
  }
  EXPECT_FALSE(ring.try_push(99U));
  EXPECT_EQ(ring.size_approx(), 4U);

  std::uint32_t value = 0;
  EXPECT_TRUE(ring.try_pop(value));
  EXPECT_EQ(value, 0U);
  // One slot is free again: the rejected push can now succeed.
  EXPECT_TRUE(ring.try_push(4U));
  EXPECT_FALSE(ring.try_push(5U));

  for (std::uint32_t expected = 1; expected < 5; ++expected) {
    EXPECT_TRUE(ring.try_pop(value));
    EXPECT_EQ(value, expected);
  }
  EXPECT_FALSE(ring.try_pop(value));
}

TEST(SpscRing, WrapsAroundManyTimesWithoutLosingTheOrder) {
  constexpr std::size_t kCapacity = 8;
  SpscRing<std::uint32_t, kCapacity> ring;
  std::uint32_t next_push = 0;
  std::uint32_t next_pop = 0;
  std::size_t in_ring = 0;

  // Fill, drain, fill again with a different phase, and repeat: every slot is
  // reused many times and head/tail wrap repeatedly.
  for (int round = 0; round < 2000; ++round) {
    const std::size_t free_slots = kCapacity - in_ring;
    const std::size_t fill = free_slots == 0 ? 0 : 1 + static_cast<std::size_t>(round) % free_slots;
    for (std::size_t i = 0; i < fill; ++i) {
      ASSERT_TRUE(ring.try_push(next_push));
      ++next_push;
      ++in_ring;
    }
    if (in_ring == kCapacity) {
      ASSERT_FALSE(ring.try_push(0xFFFFFFFFU));  // exactly full
    }
    const std::size_t drain = 1 + (static_cast<std::size_t>(round) * 7) % in_ring;
    for (std::size_t i = 0; i < drain; ++i) {
      std::uint32_t value = 0;
      ASSERT_TRUE(ring.try_pop(value));
      ASSERT_EQ(value, next_pop);
      ++next_pop;
      --in_ring;
    }
    ASSERT_EQ(ring.size_approx(), in_ring);
  }
  while (next_pop < next_push) {
    std::uint32_t value = 0;
    ASSERT_TRUE(ring.try_pop(value));
    ASSERT_EQ(value, next_pop);
    ++next_pop;
  }
  EXPECT_EQ(ring.size_approx(), 0U);
}

TEST(SpscRing, TwoThreadsPreserveOrderAndContent) {
  constexpr std::uint64_t kMessages = kUnderSanitizer ? 200'000 : 10'000'000;
  SpscRing<Message, 1024> ring;
  std::atomic<bool> failed{false};

  std::thread producer([&] {
    for (std::uint64_t sequence = 0; sequence < kMessages; ++sequence) {
      const Message message{sequence, payload_for(sequence)};
      while (!ring.try_push(message)) {
        std::this_thread::yield();
      }
    }
  });

  std::thread consumer([&] {
    std::uint64_t received = 0;
    std::uint64_t expected_sequence = 0;
    bool valid = true;
    Message message{};
    // Keep draining even after a violation so the producer can always finish
    // and the join below cannot deadlock on a full ring.
    while (received < kMessages) {
      if (!ring.try_pop(message)) {
        std::this_thread::yield();
        continue;
      }
      ++received;
      if (!valid) {
        continue;
      }
      if (message.sequence != expected_sequence) {
        ADD_FAILURE() << "order violation: expected sequence " << expected_sequence << ", got "
                      << message.sequence;
        valid = false;
        continue;
      }
      if (message.payload != payload_for(expected_sequence)) {
        ADD_FAILURE() << "content violation at sequence " << expected_sequence;
        valid = false;
        continue;
      }
      ++expected_sequence;
    }
    if (!valid) {
      failed = true;
    }
  });

  producer.join();
  consumer.join();
  EXPECT_FALSE(failed.load());
  EXPECT_EQ(ring.size_approx(), 0U);
}

TEST(SpscRing, HeadAndTailLiveOnDifferentCacheLines) {
  using Ring = SpscRing<std::uint32_t, 64>;
  using Probe = lob::SpscRingLayoutProbe<std::uint32_t, 64>;

  EXPECT_EQ(static_cast<std::size_t>(alignof(Ring)) % lob::kSpscCacheLineSize, 0U);
  EXPECT_GE(sizeof(Ring), 2 * lob::kSpscCacheLineSize + 64 * sizeof(std::uint32_t));

  const std::size_t head = Probe::head_offset();
  const std::size_t tail = Probe::tail_offset();
  EXPECT_EQ(head, 0U);
  EXPECT_GE(tail - head, lob::kSpscCacheLineSize) << "head and tail share a cache line";

  // Locality, not just separation: each index shares its line with the cached
  // copy its own thread maintains, and never with the other thread's.
  const std::size_t cached_tail = Probe::cached_tail_offset();
  const std::size_t cached_head = Probe::cached_head_offset();
  EXPECT_GE(cached_tail, head);
  EXPECT_LT(cached_tail, head + lob::kSpscCacheLineSize);
  EXPECT_GE(cached_head, tail);
  EXPECT_LT(cached_head, tail + lob::kSpscCacheLineSize);

  EXPECT_GE(Probe::buffer_offset(), tail + lob::kSpscCacheLineSize);
}

}  // namespace
