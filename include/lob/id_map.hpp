#pragma once

#include <algorithm>
#include <bit>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>

#include "lob/pool.hpp"
#include "lob/types.hpp"

namespace lob {

namespace detail {

/// splitmix64 finalizer, used to spread order ids so that sequential ids land
/// in unrelated table slots.
constexpr std::uint64_t hash_order_id(std::uint64_t value) noexcept {
  value += 0x9e3779b97f4a7c15ULL;
  value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
  value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
  return value ^ (value >> 31);
}

}  // namespace detail

/// Fixed-capacity open-addressing map from OrderId to a 32-bit pool index.
///
/// Capacity is the next power of two at or above 2 * max_orders and never
/// changes: the table never rehashes and allocates only in the constructor.
/// Collisions probe linearly; deletion is backward shift (no tombstones), so a
/// lookup stops at the first empty slot. Every OrderId value is a valid key,
/// including 0 and UINT64_MAX: a slot is empty exactly when its stored index
/// is 0, and stored indices are pool indices incremented by one, which can
/// never be 0 for an index the pool can hand out. insert() returns false when
/// the key is already present or the table is full; find() returns kNull when
/// the key is absent. Probes are bounded by the table size, so even a
/// completely full table behaves.
class IdMap {
 public:
  static constexpr std::uint32_t kNull = kNullIndex;

  /// max_orders is the number of live orders the map must hold; the table gets
  /// room for twice that so probe sequences stay short.
  explicit IdMap(std::uint32_t max_orders)
      : capacity_(std::bit_ceil(std::max<std::uint64_t>(std::uint64_t{2} * max_orders, 2))),
        mask_(capacity_ - 1),
        slots_(std::make_unique<Slot[]>(capacity_)),
        size_(0) {}

  IdMap(const IdMap&) = delete;
  IdMap& operator=(const IdMap&) = delete;
  IdMap(IdMap&&) = delete;
  IdMap& operator=(IdMap&&) = delete;

  /// Binds id to index. Fails when id is already bound or the table is full.
  bool insert(OrderId id, std::uint32_t index) {
    assert(index != kNull && "kNull is not a pool index");
    std::size_t slot = home(id);
    for (std::size_t step = 0; step < capacity_; ++step) {
      if (slots_[slot].index == 0) {
        slots_[slot] = Slot{id, index + 1};
        ++size_;
        return true;
      }
      if (slots_[slot].id == id) {
        return false;
      }
      slot = (slot + 1) & mask_;
    }
    return false;
  }

  /// Returns the index bound to id, or kNull when id is absent.
  std::uint32_t find(OrderId id) const {
    std::size_t slot = home(id);
    for (std::size_t step = 0; step < capacity_; ++step) {
      if (slots_[slot].index == 0) {
        return kNull;
      }
      if (slots_[slot].id == id) {
        return slots_[slot].index - 1;
      }
      slot = (slot + 1) & mask_;
    }
    return kNull;
  }

  /// Removes id, closing the gap with backward shifts so lookups stay correct.
  /// Returns false when id is absent.
  bool erase(OrderId id) {
    std::size_t hole = home(id);
    for (std::size_t step = 0; step < capacity_; ++step) {
      if (slots_[hole].index == 0) {
        return false;
      }
      if (slots_[hole].id == id) {
        break;
      }
      hole = (hole + 1) & mask_;
    }
    if (slots_[hole].index == 0 || slots_[hole].id != id) {
      return false;
    }

    // Walk the probe cluster behind the removed entry. An entry whose probe
    // path passes through the hole must stay; any other entry can shift back
    // into it, moving the hole forward. The walk is bounded by the table size
    // so a physically full table still terminates, and the hole left behind
    // becomes the empty slot.
    std::size_t probe = hole;
    for (std::size_t step = 1; step < capacity_; ++step) {
      probe = (probe + 1) & mask_;
      if (slots_[probe].index == 0) {
        break;
      }
      if (!lies_on_probe_path(home(slots_[probe].id), hole, probe)) {
        slots_[hole] = slots_[probe];
        hole = probe;
      }
    }
    slots_[hole] = Slot{};
    --size_;
    return true;
  }

  /// Number of bound ids.
  std::uint32_t size() const { return size_; }

  /// Number of slots in the table.
  std::size_t capacity() const { return capacity_; }

 private:
  struct Slot {
    OrderId id;
    std::uint32_t index;  // pool index + 1; 0 marks an empty slot
  };

  std::size_t home(OrderId id) const {
    return static_cast<std::size_t>(detail::hash_order_id(id)) & mask_;
  }

  /// True when a key hashing to `home` is found by probing forward from `home`
  /// before reaching the slot after `from`, i.e. its probe path passes through
  /// the cyclic interval (from, to]. Such an entry must not be moved back into
  /// the hole.
  static bool lies_on_probe_path(std::size_t home, std::size_t from, std::size_t to) noexcept {
    if (from < to) {
      return home > from && home <= to;
    }
    return home > from || home <= to;
  }

  std::size_t capacity_;
  std::size_t mask_;
  std::unique_ptr<Slot[]> slots_;
  std::uint32_t size_;
};

}  // namespace lob
