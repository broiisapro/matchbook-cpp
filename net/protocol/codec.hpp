#pragma once

// Byte-level codec for the wire protocol (§14). Encoding writes one field at a
// time, little-endian, into a caller-provided buffer; decoding reads one field
// at a time and validates the header before any body field is touched. No
// function reinterprets a struct as bytes, so the layouts do not depend on the
// compiler's padding or the host byte order.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "net/protocol/messages.hpp"

namespace lob::net {

/// Outcome of decoding. Ok means the buffer held a valid message; every other
/// value names the first check that failed. Decoders leave their output
/// arguments untouched unless they return Ok.
enum class DecodeStatus : std::uint8_t {
  Ok = 0,
  Truncated,     ///< fewer bytes than the frame needs
  WrongType,     ///< the header names a different message type than requested
  UnknownType,   ///< the header type id is not in the table
  BadLength,     ///< the header length does not match the type's body size
  Oversize,      ///< the declared body is larger than any message in the table
  BadReserved,   ///< a reserved header or body field is not zero
  InvalidField,  ///< an enum-valued field is outside its wire range
};

std::string_view to_string(DecodeStatus status);

/// Decoded common header. The reserved field is validated and not exposed.
struct Header {
  MessageType type = MessageType::NewOrder;
  std::uint16_t length = 0;  ///< body bytes, header excluded
  Seq seq = 0;

  friend bool operator==(const Header&, const Header&) = default;
};

/// Validates the 16-byte header at the front of `in`. Truncated when `in` has
/// fewer than kHeaderSize bytes; the other failures are checked in this order:
/// reserved non-zero, body larger than kMaxBodySize (Oversize), unknown type,
/// length different from body_size(type).
DecodeStatus decode_header(std::span<const std::byte> in, Header& out);

// One encoder per message. Writes exactly Msg::kWireSize bytes into `out`,
// including the header and any reserved padding (written as zero), and returns
// the number of bytes written. Returns 0 and writes nothing when `out` is too
// small.
std::size_t encode(const NewOrder& message, Seq seq, std::span<std::byte> out);
std::size_t encode(const Cancel& message, Seq seq, std::span<std::byte> out);
std::size_t encode(const Modify& message, Seq seq, std::span<std::byte> out);
std::size_t encode(const Heartbeat& message, Seq seq, std::span<std::byte> out);
std::size_t encode(const Ack& message, Seq seq, std::span<std::byte> out);
std::size_t encode(const Reject& message, Seq seq, std::span<std::byte> out);
std::size_t encode(const Fill& message, Seq seq, std::span<std::byte> out);
std::size_t encode(const Cancelled& message, Seq seq, std::span<std::byte> out);
std::size_t encode(const BookUpdate& message, Seq seq, std::span<std::byte> out);
std::size_t encode(const Trade& message, Seq seq, std::span<std::byte> out);
std::size_t encode(const SequenceReset& message, Seq seq, std::span<std::byte> out);
std::size_t encode(const RecoveryRequest& message, Seq seq, std::span<std::byte> out);

// One decoder per message. `in` may be longer than the message; only the first
// Msg::kWireSize bytes are read, so a caller can decode frame by frame. seq
// receives the header sequence number.
DecodeStatus decode(std::span<const std::byte> in, NewOrder& out, Seq& seq);
DecodeStatus decode(std::span<const std::byte> in, Cancel& out, Seq& seq);
DecodeStatus decode(std::span<const std::byte> in, Modify& out, Seq& seq);
DecodeStatus decode(std::span<const std::byte> in, Heartbeat& out, Seq& seq);
DecodeStatus decode(std::span<const std::byte> in, Ack& out, Seq& seq);
DecodeStatus decode(std::span<const std::byte> in, Reject& out, Seq& seq);
DecodeStatus decode(std::span<const std::byte> in, Fill& out, Seq& seq);
DecodeStatus decode(std::span<const std::byte> in, Cancelled& out, Seq& seq);
DecodeStatus decode(std::span<const std::byte> in, BookUpdate& out, Seq& seq);
DecodeStatus decode(std::span<const std::byte> in, Trade& out, Seq& seq);
DecodeStatus decode(std::span<const std::byte> in, SequenceReset& out, Seq& seq);
DecodeStatus decode(std::span<const std::byte> in, RecoveryRequest& out, Seq& seq);

}  // namespace lob::net
