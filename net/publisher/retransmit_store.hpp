#pragma once

// Fixed-capacity store of the most recent published frames, used to answer
// RecoveryRequest messages. Capacity slots are allocated once at construction;
// appending a frame never allocates and never touches the heap.
//
// Sequence numbers are the market-data sequence carried in the frame header.
// The store keeps insertion order and does not require the numbers to be
// contiguous, but a subscriber asking for a range gets the frames that are
// actually held, clamped to the newest one.

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "net/protocol/messages.hpp"

namespace lob::publisher {

using net::kMaxWireSize;
using net::Seq;

/// What the recovery server should send for a [from, to] request. One of three
/// outcomes: reset the subscriber to `next_seq`, replay `from..to` inclusive,
/// or send nothing (the request starts after the newest held frame).
struct RecoveryPlan {
  enum class Kind : std::uint8_t { Reset, Replay, Nothing };

  Kind kind = Kind::Nothing;
  Seq next_seq = 0;  ///< first sequence the subscriber should expect (Reset)
  Seq from = 0;      ///< first replayed sequence (Replay)
  Seq to = 0;        ///< last replayed sequence (Replay), inclusive

  friend bool operator==(const RecoveryPlan&, const RecoveryPlan&) = default;
};

class RetransmitStore {
 public:
  /// `capacity` must be positive. Throws std::invalid_argument otherwise.
  explicit RetransmitStore(std::size_t capacity);

  /// Stores a complete encoded frame under its header sequence number. A frame
  /// whose sequence is already held is ignored (a duplicate). If the sequence
  /// moves backwards to one the store no longer holds, the stream restarted and
  /// the old contents are dropped first.
  void append(Seq seq, std::span<const std::byte> frame);

  void clear();

  std::size_t capacity() const { return slots_.size(); }
  std::size_t size() const { return count_; }
  bool empty() const { return count_ == 0; }

  /// Oldest and newest held sequence numbers, 0 when the store is empty.
  Seq oldest() const;
  Seq newest() const { return newest_; }

  bool holds(Seq seq) const;

  /// The stored frame for `seq`, or nullopt when it is not held.
  std::optional<std::span<const std::byte>> frame(Seq seq) const;

  /// Decides how to answer a recovery request. `to` is clamped to the newest
  /// held frame, `from` older than the store yields a Reset to `oldest()`.
  RecoveryPlan plan(Seq from, Seq to) const;

 private:
  struct Slot {
    Seq seq = 0;
    std::uint16_t length = 0;
    std::array<std::byte, kMaxWireSize> bytes{};
  };

  static constexpr std::size_t kNone = static_cast<std::size_t>(-1);

  std::size_t index_of(Seq seq) const;

  std::vector<Slot> slots_;
  // Maps seq % capacity to the slot holding it; kNone when that slot is unused
  // or holds a different sequence.
  std::vector<std::size_t> lookup_;
  std::size_t head_ = 0;  ///< slot of the oldest held frame
  std::size_t count_ = 0;
  Seq newest_ = 0;
};

}  // namespace lob::publisher
