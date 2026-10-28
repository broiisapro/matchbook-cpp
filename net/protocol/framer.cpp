#include "net/protocol/framer.hpp"

#include <algorithm>

namespace lob::net {
namespace {

FramerError to_framer_error(DecodeStatus status) {
  switch (status) {
    case DecodeStatus::Ok:
      return FramerError::None;
    case DecodeStatus::UnknownType:
      return FramerError::UnknownType;
    case DecodeStatus::Oversize:
      return FramerError::Oversize;
    case DecodeStatus::BadReserved:
      return FramerError::BadReserved;
    // Truncated cannot happen once 16 header bytes are buffered, and the other
    // statuses concern a body the framer deliberately does not interpret; they
    // all mean the declared length cannot belong to a valid frame here.
    case DecodeStatus::Truncated:
    case DecodeStatus::WrongType:
    case DecodeStatus::BadLength:
    case DecodeStatus::InvalidField:
      return FramerError::BadLength;
  }
  return FramerError::BadLength;
}

}  // namespace

std::string_view to_string(FramerError error) {
  switch (error) {
    case FramerError::None:
      return "none";
    case FramerError::UnknownType:
      return "unknown_type";
    case FramerError::BadLength:
      return "bad_length";
    case FramerError::Oversize:
      return "oversize";
    case FramerError::BadReserved:
      return "bad_reserved";
  }
  return "unknown";
}

PushResult StreamFramer::push(std::span<const std::byte> chunk, FrameSink& sink) {
  if (error_ != FramerError::None) {
    return PushResult{FramerResult::Error, 0};
  }

  std::size_t used = 0;
  while (used < chunk.size()) {
    if (size_ < kHeaderSize) {
      const std::size_t take = std::min(kHeaderSize - size_, chunk.size() - used);
      std::copy_n(chunk.data() + used, take, buffer_.data() + size_);
      size_ += take;
      used += take;
      if (size_ < kHeaderSize) {
        return PushResult{FramerResult::Ok, used};
      }
      // Fall through with a complete header so a bad one is reported without
      // waiting for a body that may never arrive.
    }

    Header header;
    const DecodeStatus status =
        decode_header(std::span<const std::byte>(buffer_.data(), size_), header);
    if (status != DecodeStatus::Ok) {
      error_ = to_framer_error(status);
      size_ = 0;
      return PushResult{FramerResult::Error, used};
    }

    const std::size_t total = kHeaderSize + header.length;
    if (size_ < total) {
      const std::size_t take = std::min(total - size_, chunk.size() - used);
      std::copy_n(chunk.data() + used, take, buffer_.data() + size_);
      size_ += take;
      used += take;
      if (size_ < total) {
        return PushResult{FramerResult::Ok, used};
      }
    }

    ++frames_;
    const bool keep_going = sink.on_frame(std::span<const std::byte>(buffer_.data(), total));
    size_ = 0;
    if (!keep_going) {
      return PushResult{FramerResult::Stopped, used};
    }
  }
  return PushResult{FramerResult::Ok, used};
}

void StreamFramer::reset() {
  size_ = 0;
  error_ = FramerError::None;
  frames_ = 0;
}

}  // namespace lob::net
