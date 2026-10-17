#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>

namespace lob {

/// 32-bit index value that no live slot uses. Pools, the intrusive FIFO and
/// the id map all spell "no element" this way.
inline constexpr std::uint32_t kNullIndex = std::numeric_limits<std::uint32_t>::max();

/// Fixed-capacity pool of T addressed by 32-bit indices.
///
/// All storage is allocated once, in the constructor, and never grows.
/// acquire() and release() only move indices between a free list and the live
/// set; the free list is threaded through the unused slots themselves, so no
/// side storage is needed. Reuse is LIFO. T must be trivially destructible:
/// the pool treats its slots as plain nodes and never runs destructors.
template <class T>
class ObjectPool {
  static_assert(std::is_trivially_destructible_v<T>,
                "ObjectPool stores plain nodes and never destroys them");

 public:
  static constexpr std::uint32_t kNull = kNullIndex;

  explicit ObjectPool(std::uint32_t capacity)
      : slots_(capacity == 0 ? nullptr : std::make_unique<Slot[]>(capacity)),
        capacity_(capacity),
        size_(0),
        free_head_(kNull) {
    for (std::uint32_t i = 0; i < capacity_; ++i) {
      slots_[i].next = i + 1 < capacity_ ? i + 1 : kNull;
    }
    free_head_ = capacity_ == 0 ? kNull : 0;
  }

  ObjectPool(const ObjectPool&) = delete;
  ObjectPool& operator=(const ObjectPool&) = delete;
  ObjectPool(ObjectPool&&) = delete;
  ObjectPool& operator=(ObjectPool&&) = delete;

  /// Returns the index of a default-constructed element, or kNull when the
  /// pool is exhausted. Never allocates.
  [[nodiscard]] std::uint32_t acquire() {
    if (free_head_ == kNull) {
      return kNull;
    }
    const std::uint32_t index = free_head_;
    free_head_ = slots_[index].next;
    std::construct_at(&slots_[index].value);
    ++size_;
    return index;
  }

  /// Returns a live element to the free list. Its contents become garbage.
  void release(std::uint32_t index) {
    slots_[index].next = free_head_;
    free_head_ = index;
    --size_;
  }

  T& operator[](std::uint32_t index) { return slots_[index].value; }
  const T& operator[](std::uint32_t index) const { return slots_[index].value; }

  /// Number of live elements.
  std::uint32_t size() const { return size_; }

  /// Number of elements the pool can hold in total.
  std::uint32_t capacity() const { return capacity_; }

 private:
  // The engine test suite's invariant checker walks the free list directly.
  friend struct OrderBookTestAccess;

  // Each slot is either a live T or the free list's link; the union keeps the
  // storage exact-sized and correctly aligned without constructing T up front.
  union Slot {
    T value;
    std::uint32_t next;
    Slot() : next(kNull) {}
    ~Slot() {}
  };

  std::unique_ptr<Slot[]> slots_;
  std::uint32_t capacity_;
  std::uint32_t size_;
  std::uint32_t free_head_;
};

namespace detail {

/// Byte size and alignment a container asked its allocator for while probing.
struct NodeSizeProbe {
  std::size_t size = 0;
  std::size_t align = 0;
};

/// Allocator that records what it is asked to allocate and otherwise forwards
/// to the global allocator. Used once to discover a std::map's internal node
/// size on the standard library in use.
template <class T>
class ProbeAllocator {
 public:
  using value_type = T;

  explicit ProbeAllocator(NodeSizeProbe* probe) noexcept : probe_(probe) {}

  template <class U>
  ProbeAllocator(const ProbeAllocator<U>& other) noexcept : probe_(other.probe_) {}

  T* allocate(std::size_t n) {
    const std::size_t bytes = n * sizeof(T);
    if (bytes > probe_->size) {
      probe_->size = bytes;
      probe_->align = alignof(T);
    }
    return static_cast<T*>(::operator new(bytes, std::align_val_t{alignof(T)}));
  }

  void deallocate(T* pointer, std::size_t n) noexcept {
    ::operator delete(pointer, n * sizeof(T), std::align_val_t{alignof(T)});
  }

  template <class U>
  bool operator==(const ProbeAllocator<U>& other) const noexcept {
    return probe_ == other.probe_;
  }

 private:
  template <class U>
  friend class ProbeAllocator;

  NodeSizeProbe* probe_;
};

/// A fixed-capacity slab of equally sized nodes. This is the type-erased
/// control block that PoolAllocator shares between its rebound copies; the
/// owning engine holds it. Counting and reuse happen entirely inside the slab.
class NodePool {
 public:
  NodePool(std::size_t node_size, std::size_t node_align, std::uint32_t capacity)
      : node_size_(node_size),
        node_align_(node_align),
        capacity_(capacity),
        size_(0),
        free_head_(kNullIndex) {
    if (capacity_ != 0) {
      storage_ = static_cast<std::byte*>(
          ::operator new(node_size_ * capacity_, std::align_val_t{node_align_}));
      for (std::uint32_t i = 0; i < capacity_; ++i) {
        const std::uint32_t next = i + 1 < capacity_ ? i + 1 : kNullIndex;
        std::memcpy(storage_ + static_cast<std::size_t>(i) * node_size_, &next, sizeof(next));
      }
      free_head_ = 0;
    }
  }

  ~NodePool() {
    if (storage_ != nullptr) {
      ::operator delete(storage_, node_size_ * capacity_, std::align_val_t{node_align_});
    }
  }

  NodePool(const NodePool&) = delete;
  NodePool& operator=(const NodePool&) = delete;
  NodePool(NodePool&&) = delete;
  NodePool& operator=(NodePool&&) = delete;

  /// Returns a node of node_size() bytes, or nullptr when the pool is
  /// exhausted. Never allocates.
  [[nodiscard]] void* allocate() noexcept {
    if (free_head_ == kNullIndex) {
      return nullptr;
    }
    const std::uint32_t index = free_head_;
    void* node = storage_ + static_cast<std::size_t>(index) * node_size_;
    std::memcpy(&free_head_, node, sizeof(free_head_));
    ++size_;
    return node;
  }

  void deallocate(void* node) noexcept {
    const std::uint32_t index = static_cast<std::uint32_t>(
        static_cast<std::size_t>(static_cast<std::byte*>(node) - storage_) / node_size_);
    std::memcpy(node, &free_head_, sizeof(free_head_));
    free_head_ = index;
    --size_;
  }

  std::size_t node_size() const noexcept { return node_size_; }
  std::size_t node_align() const noexcept { return node_align_; }
  std::uint32_t capacity() const noexcept { return capacity_; }

  /// Number of nodes currently handed out.
  std::uint32_t size() const noexcept { return size_; }

 private:
  std::byte* storage_ = nullptr;
  std::size_t node_size_;
  std::size_t node_align_;
  std::uint32_t capacity_;
  std::uint32_t size_;
  std::uint32_t free_head_;
};

/// Creates a node pool with room for `capacity` nodes of the type the standard
/// library rebinds Map's allocator to. A scratch map with a recording
/// allocator inserts one element; the byte size it asks for is the container's
/// node size on this standard library, so the pool stays exact without
/// depending on internal layouts. Map's key, mapped and comparator types must
/// be default-constructible.
template <class Map>
std::unique_ptr<NodePool> make_node_pool_for_map(std::uint32_t capacity) {
  using Key = typename Map::key_type;
  using Mapped = typename Map::mapped_type;
  using Compare = typename Map::key_compare;

  NodeSizeProbe probe;
  using Probe = ProbeAllocator<std::pair<const Key, Mapped>>;
  std::map<Key, Mapped, Compare, Probe> scratch{Compare{}, Probe{&probe}};
  scratch.emplace(Key{}, Mapped{});
  scratch.erase(scratch.begin());
  return std::make_unique<NodePool>(probe.size, probe.align, capacity);
}

}  // namespace detail

/// Fixed-capacity allocator for std::map nodes.
///
/// Stateful and rebind-able: every copy, including PoolAllocator<U> rebound
/// copies, shares the same detail::NodePool control block, which the owning
/// engine constructs once. allocate(1) pops a node; any other request, a
/// missing control block or an exhausted pool throws std::bad_alloc. The
/// engine checks its level capacity before inserting, so in correct use the
/// pool never exhausts and no allocation ever reaches the heap after
/// construction.
template <class T>
class PoolAllocator {
 public:
  using value_type = T;
  using propagate_on_container_copy_assignment = std::true_type;
  using propagate_on_container_move_assignment = std::true_type;
  using propagate_on_container_swap = std::true_type;
  using is_always_equal = std::false_type;

  PoolAllocator() noexcept : pool_(nullptr) {}

  explicit PoolAllocator(detail::NodePool* pool) noexcept : pool_(pool) {}

  template <class U>
  PoolAllocator(const PoolAllocator<U>& other) noexcept : pool_(other.pool()) {}

  T* allocate(std::size_t n) {
    if (n != 1 || pool_ == nullptr || sizeof(T) > pool_->node_size() ||
        alignof(T) > pool_->node_align()) {
      throw std::bad_alloc{};
    }
    void* node = pool_->allocate();
    if (node == nullptr) {
      throw std::bad_alloc{};
    }
    return static_cast<T*>(node);
  }

  void deallocate(T* pointer, std::size_t) noexcept {
    if (pointer != nullptr) {
      pool_->deallocate(pointer);
    }
  }

  /// The shared control block; nullptr for a default-constructed allocator.
  detail::NodePool* pool() const noexcept { return pool_; }

 private:
  template <class U>
  friend class PoolAllocator;

  detail::NodePool* pool_;
};

/// Two allocators are equal exactly when they share a control block.
template <class T, class U>
bool operator==(const PoolAllocator<T>& lhs, const PoolAllocator<U>& rhs) noexcept {
  return lhs.pool() == rhs.pool();
}

}  // namespace lob
