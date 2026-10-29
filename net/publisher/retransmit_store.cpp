#include "net/publisher/retransmit_store.hpp"

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace lob::publisher {

RetransmitStore::RetransmitStore(std::size_t capacity)
    : slots_(capacity), lookup_(capacity, kNone) {
  if (capacity == 0) {
    throw std::invalid_argument("RetransmitStore capacity must be positive");
  }
}

void RetransmitStore::append(Seq seq, std::span<const std::byte> frame) {
  if (frame.empty() || frame.size() > kMaxWireSize) {
    return;
  }
  if (count_ > 0 && seq <= newest_) {
    if (holds(seq)) {
      return;  // duplicate of a frame we already hold
    }
    // The stream went backwards to a sequence the store no longer holds: the
    // gateway restarted, so the old epoch cannot answer a recovery request.
    clear();
  }

  if (count_ == slots_.size()) {
    const std::size_t dropped = slots_[head_].seq % slots_.size();
    if (lookup_[dropped] == head_) {
      lookup_[dropped] = kNone;
    }
    head_ = (head_ + 1) % slots_.size();
    --count_;
  }

  const std::size_t index = (head_ + count_) % slots_.size();
  Slot& slot = slots_[index];
  slot.seq = seq;
  slot.length = static_cast<std::uint16_t>(frame.size());
  std::memcpy(slot.bytes.data(), frame.data(), frame.size());
  lookup_[seq % slots_.size()] = index;
  ++count_;
  newest_ = seq;
}

void RetransmitStore::clear() {
  for (Slot& slot : slots_) {
    slot.seq = 0;
    slot.length = 0;
  }
  std::fill(lookup_.begin(), lookup_.end(), kNone);
  head_ = 0;
  count_ = 0;
  newest_ = 0;
}

Seq RetransmitStore::oldest() const { return count_ == 0 ? 0 : slots_[head_].seq; }

std::size_t RetransmitStore::index_of(Seq seq) const {
  if (slots_.empty()) {
    return kNone;
  }
  const std::size_t index = lookup_[seq % slots_.size()];
  if (index == kNone || slots_[index].seq != seq) {
    return kNone;
  }
  return index;
}

bool RetransmitStore::holds(Seq seq) const { return index_of(seq) != kNone; }

std::optional<std::span<const std::byte>> RetransmitStore::frame(Seq seq) const {
  const std::size_t index = index_of(seq);
  if (index == kNone) {
    return std::nullopt;
  }
  return std::span<const std::byte>(slots_[index].bytes.data(), slots_[index].length);
}

RecoveryPlan RetransmitStore::plan(Seq from, Seq to) const {
  if (to < from || count_ == 0) {
    if (count_ == 0) {
      return RecoveryPlan{RecoveryPlan::Kind::Reset, 1, 0, 0};
    }
    return RecoveryPlan{};
  }
  if (from < oldest()) {
    return RecoveryPlan{RecoveryPlan::Kind::Reset, oldest(), 0, 0};
  }
  if (from > newest_) {
    return RecoveryPlan{};
  }
  return RecoveryPlan{RecoveryPlan::Kind::Replay, 0, from, std::min(to, newest_)};
}

}  // namespace lob::publisher
