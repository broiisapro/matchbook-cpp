#pragma once

// Packs complete wire frames into one UDP datagram without ever splitting a
// frame. The buffer is fixed at construction (1200 bytes by default, the
// smallest MTU every hop in the demo has to carry); the packer does no
// allocation.

#include <array>
#include <cstddef>
#include <span>

namespace lob::publisher {

class DatagramPacker {
 public:
  static constexpr std::size_t kDefaultMaxBytes = 1200;

  explicit DatagramPacker(std::size_t max_bytes = kDefaultMaxBytes);

  /// Appends `frame` when it fits. Returns false without changing the buffer
  /// when the frame does not fit (flush first and retry). A frame larger than
  /// the whole datagram never fits; the caller drops it.
  bool add(std::span<const std::byte> frame);

  std::span<const std::byte> data() const { return {buffer_.data(), size_}; }
  std::size_t size() const { return size_; }
  bool empty() const { return size_ == 0; }
  std::size_t max_bytes() const { return max_bytes_; }

  void clear() { size_ = 0; }

 private:
  std::size_t max_bytes_;
  std::array<std::byte, 65507> buffer_{};
  std::size_t size_ = 0;
};

}  // namespace lob::publisher
