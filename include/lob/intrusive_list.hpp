#pragma once

#include <cstdint>

#include "lob/pool.hpp"

namespace lob {

/// FIFO queue of pool indices whose prev/next links live inside the pooled
/// node itself.
///
/// The queue state (head and tail) is owned by the caller, typically a price
/// level, and passed by reference; the list is a thin view over it, so a level
/// can store the indices as plain fields and the matching code can build a
/// view wherever it needs one. Every operation is O(1) and allocates nothing.
///
/// Node must expose two std::uint32_t members, named by the Prev and Next
/// member pointers.
template <class Node, std::uint32_t Node::* Prev, std::uint32_t Node::* Next>
class IntrusiveList {
 public:
  static constexpr std::uint32_t kNull = kNullIndex;

  IntrusiveList(ObjectPool<Node>& pool, std::uint32_t& head, std::uint32_t& tail) noexcept
      : pool_(pool), head_(head), tail_(tail) {}

  bool empty() const noexcept { return head_ == kNull; }

  /// Index of the oldest element; meaningful only when !empty().
  std::uint32_t front() const noexcept { return head_; }

  /// Index of the newest element; meaningful only when !empty().
  std::uint32_t back() const noexcept { return tail_; }

  /// Index after `index` in FIFO order, kNull at the tail.
  std::uint32_t next(std::uint32_t index) const noexcept { return pool_[index].*Next; }

  void push_back(std::uint32_t index) noexcept {
    Node& node = pool_[index];
    node.*Prev = tail_;
    node.*Next = kNull;
    if (tail_ == kNull) {
      head_ = index;
    } else {
      pool_[tail_].*Next = index;
    }
    tail_ = index;
  }

  /// Unlinks `index` from the queue. The caller keeps ownership of the node.
  void erase(std::uint32_t index) noexcept {
    Node& node = pool_[index];
    if (node.*Prev == kNull) {
      head_ = node.*Next;
    } else {
      pool_[node.*Prev].*Next = node.*Next;
    }
    if (node.*Next == kNull) {
      tail_ = node.*Prev;
    } else {
      pool_[node.*Next].*Prev = node.*Prev;
    }
    node.*Prev = kNull;
    node.*Next = kNull;
  }

 private:
  ObjectPool<Node>& pool_;
  std::uint32_t& head_;
  std::uint32_t& tail_;
};

}  // namespace lob
