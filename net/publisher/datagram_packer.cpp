#include "net/publisher/datagram_packer.hpp"

#include <cstring>
#include <stdexcept>

#include "net/protocol/messages.hpp"

namespace lob::publisher {

using net::kMaxWireSize;

DatagramPacker::DatagramPacker(std::size_t max_bytes) : max_bytes_(max_bytes) {
  if (max_bytes_ < kMaxWireSize || max_bytes_ > buffer_.size()) {
    throw std::invalid_argument("DatagramPacker max_bytes must fit one frame and one datagram");
  }
}

bool DatagramPacker::add(std::span<const std::byte> frame) {
  if (frame.size() > max_bytes_ || frame.size() > max_bytes_ - size_) {
    return false;
  }
  std::memcpy(buffer_.data() + size_, frame.data(), frame.size());
  size_ += frame.size();
  return true;
}

}  // namespace lob::publisher
