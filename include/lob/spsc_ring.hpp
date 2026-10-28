#pragma once

#include <atomic>
#include <cstddef>
#include <cstring>
#include <type_traits>

namespace lob {

/// Cache line size used to keep the ring's indices apart.
///
/// Apple M-series performance cores have 128-byte L1 data cache lines and the
/// hardware prefetches the adjacent line, so two indices 64 bytes apart still
/// share a prefetch pair: 128 bytes is the smallest padding that keeps the
/// producer's and the consumer's hot words on separate lines.
/// `std::hardware_destructive_interference_size` is 256 bytes with the
/// toolchain used here, which is a conservative upper bound rather than the
/// measured line size, and would only add padding. On other targets the value
/// is still a multiple of every common cache line, so it is safe, just not
/// minimal.
inline constexpr std::size_t kSpscCacheLineSize = 128;

/// Bounded lock-free single-producer single-consumer ring buffer.
///
/// `Capacity` is the number of elements, must be a power of two and at least
/// two; storage is inline, so construction allocates nothing and neither does
/// `try_push`/`try_pop`. At most one thread may call `try_push` and at most one
/// (different) thread may call `try_pop`; `size_approx` and `capacity` may be
/// called from either thread.
///
/// Memory ordering
/// ---------------
/// Two free-running counters index the ring: `tail_` is the number of elements
/// ever pushed, `head_` the number ever popped. Full means
/// `tail_ - head_ == Capacity`, empty means `tail_ == head_`; both differences
/// are computed modulo 2^64, which is exact as long as fewer than 2^63 elements
/// are in flight (always true here).
///
/// - The producer writes the element into the slot, then release-stores
///   `tail_`. The release store publishes every preceding write, so a consumer
///   that acquire-loads that value and sees the new `tail_` also sees the
///   element bytes. Nothing else orders the element against the index.
/// - The consumer acquire-loads `tail_` (only when its cached copy says the
///   ring is empty), reads the slot, then release-stores `head_`. The release
///   store publishes the read and, more importantly, signals that the slot is
///   free; the producer acquire-loads `head_` before reusing a slot, so it
///   cannot overwrite an element the consumer has not finished reading. The
///   element is not touched between those two loads by the other thread except
///   through this handshake.
/// - `cached_head_` is only ever read and written by the producer, and
///   `cached_tail_` only by the consumer. They are plain (non-atomic) members:
///   each thread owns one, so there is no data race, and they cut the number of
///   cross-core monitor reads in the common case (the other side has not moved
///   since the last check) to zero.
/// - No `seq_cst` is used or needed: the pairwise producer/consumer
///   synchronisation above is a release/acquire edge, and the two threads touch
///   disjoint members. `size_approx()` is a plain approximate read from outside
///   that handshake; it may lag a concurrent push or pop by one element.
template <class T, std::size_t Capacity>
class SpscRing {
 public:
  static_assert(std::is_trivially_copyable_v<T>,
                "SpscRing stores elements inline and bytes them across threads; T must be "
                "trivially copyable");
  static_assert(Capacity >= 2, "SpscRing needs room for at least two elements");
  static_assert((Capacity & (Capacity - 1)) == 0,
                "SpscRing Capacity must be a power of two so the index mask is exact");
  static_assert(std::atomic<std::size_t>::is_always_lock_free,
                "SpscRing's indices must be lock-free; the buffer is not usable with a lock");

  SpscRing() = default;

  SpscRing(const SpscRing&) = delete;
  SpscRing& operator=(const SpscRing&) = delete;
  SpscRing(SpscRing&&) = delete;
  SpscRing& operator=(SpscRing&&) = delete;

  /// Producer thread only. Copies `value` into the ring and returns true, or
  /// returns false when the ring is full (leaving `value` untouched).
  bool try_push(const T& value) noexcept {
    const std::size_t tail = tail_.load(std::memory_order_relaxed);
    const std::size_t next = tail + 1;
    if (next - cached_head_ > Capacity) {
      // The local view says full; re-read the consumer's published index
      // (acquire so that slot reuse is ordered after the consumer's release).
      cached_head_ = head_.load(std::memory_order_acquire);
      if (next - cached_head_ > Capacity) {
        return false;
      }
    }
    std::memcpy(slot(tail & kIndexMask), &value, sizeof(T));
    // Publishes the element write as well as the new tail.
    tail_.store(next, std::memory_order_release);
    return true;
  }

  /// Consumer thread only. Moves the oldest element into `out` and returns
  /// true, or returns false when the ring is empty (leaving `out` untouched).
  bool try_pop(T& out) noexcept {
    const std::size_t head = head_.load(std::memory_order_relaxed);
    if (head == cached_tail_) {
      // The local view says empty; re-read the producer's published index
      // (acquire so the element write is visible before we read it).
      cached_tail_ = tail_.load(std::memory_order_acquire);
      if (head == cached_tail_) {
        return false;
      }
    }
    T* const value = slot(head & kIndexMask);
    std::memcpy(&out, value, sizeof(T));
    // Publishes the slot as reusable as well as the new head.
    head_.store(head + 1, std::memory_order_release);
    return true;
  }

  /// Approximate number of elements currently in the ring: an exact count of
  /// the published indices, which can lag a concurrent push or pop by one.
  std::size_t size_approx() const noexcept {
    // Read the consumer index first: it only moves towards (never past) the
    // producer index, so the later tail read is always at least as large and
    // the unsigned difference cannot wrap in the window between the reads.
    const std::size_t head = head_.load(std::memory_order_acquire);
    const std::size_t tail = tail_.load(std::memory_order_acquire);
    return tail - head;
  }

  /// Number of elements the ring can hold.
  static constexpr std::size_t capacity() noexcept { return Capacity; }

 private:
  static constexpr std::size_t kIndexMask = Capacity - 1;

  // Elements move as bytes: T is trivially copyable, so the memcpy in
  // try_push starts the implicit-lifetime object in the raw slot and the one in
  // try_pop fills the caller's object, without requiring T to be default
  // constructible or assignable.
  T* slot(std::size_t index) noexcept { return reinterpret_cast<T*>(buffer_ + index * sizeof(T)); }

  // One owner per cache line. The consumer's line holds `head_` and its cached
  // copy of the producer's index; the producer's line holds `tail_` and its
  // cached copy of the consumer's index. Sharing a line between the two owners
  // would make their cores bounce it back and forth on every operation.
  alignas(kSpscCacheLineSize) std::atomic<std::size_t> head_{0};  // consumer writes
  std::size_t cached_tail_ = 0;  // consumer-local copy of tail_, same line as head_
  alignas(kSpscCacheLineSize) std::atomic<std::size_t> tail_{0};  // producer writes
  std::size_t cached_head_ = 0;  // producer-local copy of head_, same line as tail_
  // Raw storage on its own line so element traffic never shares a line with an
  // index; aligned to T as well for over-aligned element types.
  alignas(kSpscCacheLineSize) alignas(T) std::byte buffer_[Capacity * sizeof(T)]{};

  template <class, std::size_t>
  friend struct SpscRingLayoutProbe;
};

}  // namespace lob
