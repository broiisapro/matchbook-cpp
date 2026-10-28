#pragma once

#include <cstddef>
#include <span>

#include "net/protocol/framer.hpp"

namespace lob::loadgen {

/// One order-entry connection used by the runner. The real implementation is a
/// TCP socket; tests substitute an in-process fake gateway that answers
/// frames synchronously.
class OrderEntryTransport {
 public:
  virtual ~OrderEntryTransport() = default;

  /// Writes one complete frame. False means the connection failed and the run
  /// should stop.
  virtual bool send(std::span<const std::byte> frame) = 0;

  /// Socket to wait on for readability, or -1 for an in-process transport
  /// whose receive() never blocks and should be drained on every loop
  /// iteration.
  virtual int fd() const = 0;

  /// Reads what is available and hands complete frames to `sink`. False means
  /// the connection ended or the peer sent something the framer rejected.
  virtual bool receive(lob::net::FrameSink& sink) = 0;

  /// Human-readable reason for the last failure, empty when none.
  virtual const char* last_error() const { return ""; }
};

}  // namespace lob::loadgen
