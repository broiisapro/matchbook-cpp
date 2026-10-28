#pragma once

// Incremental framer for the wire protocol (§14). A stream is an arbitrary
// sequence of chunks: push() copies bytes into a fixed buffer, validates the
// header as soon as its 16 bytes are present, and hands each complete frame to
// the sink before reusing the buffer. The framer never allocates.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "net/protocol/codec.hpp"
#include "net/protocol/messages.hpp"

namespace lob::net {

/// Receives complete frames. The span points into the framer's buffer and is
/// only valid for the duration of the call; copy the bytes to keep them.
class FrameSink {
 public:
  FrameSink() = default;
  virtual ~FrameSink() = default;
  FrameSink(const FrameSink&) = delete;
  FrameSink& operator=(const FrameSink&) = delete;

  /// Called once per complete frame in stream order. Return false to stop the
  /// current push; the caller resubmits the unconsumed bytes when ready.
  virtual bool on_frame(std::span<const std::byte> frame) = 0;
};

/// Protocol violation found while framing. The framer latches the first one
/// and ignores further input until reset().
enum class FramerError : std::uint8_t {
  None = 0,
  UnknownType,  ///< header type id is not in the message table
  BadLength,    ///< header length does not match the type's body size
  Oversize,     ///< declared body is larger than any message in the table
  BadReserved,  ///< reserved header field is not zero
};

std::string_view to_string(FramerError error);

enum class FramerResult : std::uint8_t {
  Ok = 0,   ///< input consumed, no complete frame left buffered
  Stopped,  ///< the sink returned false; resubmit from `consumed`
  Error,    ///< protocol violation; see error()
};

/// Result of one push. `consumed` counts the input bytes the framer took, which
/// is all of them except after a stop or an error.
struct PushResult {
  FramerResult result = FramerResult::Ok;
  std::size_t consumed = 0;
};

/// Turns arbitrary byte chunks into protocol frames. One instance per
/// connection; not thread-safe.
class StreamFramer {
 public:
  StreamFramer() = default;

  /// Consumes as much of `chunk` as possible, invoking `sink` for every
  /// complete frame. A partial frame stays buffered for the next call. After a
  /// protocol error the result is Error, the buffer is discarded, and later
  /// pushes keep returning Error until reset().
  PushResult push(std::span<const std::byte> chunk, FrameSink& sink);

  FramerError error() const { return error_; }

  /// Bytes of an incomplete frame held from the previous push.
  std::size_t buffered() const { return size_; }

  /// Frames handed to the sink since construction (or the last reset()).
  std::uint64_t frames_delivered() const { return frames_; }

  /// Drops any buffered bytes and clears a latched error.
  void reset();

 private:
  std::array<std::byte, kMaxWireSize> buffer_{};
  std::size_t size_ = 0;
  FramerError error_ = FramerError::None;
  std::uint64_t frames_ = 0;
};

}  // namespace lob::net
