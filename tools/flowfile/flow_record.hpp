#pragma once

// Flow file format: a 32-byte header followed by fixed-size 40-byte records,
// all little-endian. This header defines the wire format only; reading and
// writing files lives in the flow file library on top of it.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

#include "lob/types.hpp"

namespace lob::flowfile {

inline constexpr std::string_view kFlowMagic = "LOBFLOW1";
inline constexpr std::uint32_t kFlowVersion = 1;
inline constexpr std::size_t kFlowHeaderSize = 32;
inline constexpr std::size_t kFlowRecordSize = 40;

/// Operation kind of a flow record. Values are part of the wire format.
enum class FlowOp : std::uint8_t { Submit = 1, Cancel = 2, Modify = 3 };

/// Logical record; the on-disk form is the byte layout below, not this struct.
/// Cancels ignore side/type/price, modifies ignore type, market orders ignore
/// price, and ts_ns is informational (it never affects matching).
struct FlowRecord {
  FlowOp op;
  Side side;
  OrderType type;
  OrderId id;
  Price price;
  Qty qty;
  std::uint64_t ts_ns;

  friend bool operator==(const FlowRecord&, const FlowRecord&) = default;
};

/// Header contents that are not implied by the format constants.
struct FlowHeader {
  std::uint64_t record_count;

  friend bool operator==(const FlowHeader&, const FlowHeader&) = default;
};

namespace detail {

inline void store_u8(std::byte* out, std::uint8_t value) { out[0] = static_cast<std::byte>(value); }

inline void store_u32(std::byte* out, std::uint32_t value) {
  for (std::size_t i = 0; i < 4; ++i) {
    out[i] = static_cast<std::byte>((value >> (8 * i)) & 0xffU);
  }
}

inline void store_u64(std::byte* out, std::uint64_t value) {
  for (std::size_t i = 0; i < 8; ++i) {
    out[i] = static_cast<std::byte>((value >> (8 * i)) & 0xffU);
  }
}

inline void store_i64(std::byte* out, std::int64_t value) {
  store_u64(out, static_cast<std::uint64_t>(value));
}

inline std::uint8_t load_u8(const std::byte* in) { return std::to_integer<std::uint8_t>(in[0]); }

inline std::uint32_t load_u32(const std::byte* in) {
  std::uint32_t value = 0;
  for (std::size_t i = 0; i < 4; ++i) {
    value |= std::to_integer<std::uint32_t>(in[i]) << (8 * i);
  }
  return value;
}

inline std::uint64_t load_u64(const std::byte* in) {
  std::uint64_t value = 0;
  for (std::size_t i = 0; i < 8; ++i) {
    value |= std::to_integer<std::uint64_t>(in[i]) << (8 * i);
  }
  return value;
}

inline std::int64_t load_i64(const std::byte* in) {
  return static_cast<std::int64_t>(load_u64(in));
}

}  // namespace detail

/// Serializes a record into exactly kFlowRecordSize bytes:
///   offset  0  op u8, side u8, type u8, then 5 reserved zero bytes
///   offset  8  id u64
///   offset 16  price i64
///   offset 24  qty i64
///   offset 32  ts_ns u64
inline void encode(const FlowRecord& record, std::span<std::byte, kFlowRecordSize> out) {
  std::byte* data = out.data();
  detail::store_u8(data + 0, static_cast<std::uint8_t>(record.op));
  detail::store_u8(data + 1, static_cast<std::uint8_t>(record.side));
  detail::store_u8(data + 2, static_cast<std::uint8_t>(record.type));
  for (std::size_t i = 3; i < 8; ++i) {
    data[i] = std::byte{0};
  }
  detail::store_u64(data + 8, record.id);
  detail::store_i64(data + 16, record.price);
  detail::store_i64(data + 24, record.qty);
  detail::store_u64(data + 32, record.ts_ns);
}

/// Parses exactly kFlowRecordSize bytes. Every 40-byte buffer is a record; the
/// fields only carry meaning according to the op.
inline FlowRecord decode(std::span<const std::byte, kFlowRecordSize> in) {
  const std::byte* data = in.data();
  return FlowRecord{
      static_cast<FlowOp>(detail::load_u8(data + 0)),
      static_cast<Side>(detail::load_u8(data + 1)),
      static_cast<OrderType>(detail::load_u8(data + 2)),
      detail::load_u64(data + 8),
      detail::load_i64(data + 16),
      detail::load_i64(data + 24),
      detail::load_u64(data + 32),
  };
}

/// Serializes the header:
///   offset  0  magic[8] = "LOBFLOW1"
///   offset  8  version u32 = 1
///   offset 12  record_size u32 = 40
///   offset 16  record_count u64
///   offset 24  reserved u64 = 0
inline void encode_header(const FlowHeader& header, std::span<std::byte, kFlowHeaderSize> out) {
  std::byte* data = out.data();
  for (std::size_t i = 0; i < kFlowMagic.size(); ++i) {
    data[i] = static_cast<std::byte>(static_cast<unsigned char>(kFlowMagic[i]));
  }
  detail::store_u32(data + 8, kFlowVersion);
  detail::store_u32(data + 12, static_cast<std::uint32_t>(kFlowRecordSize));
  detail::store_u64(data + 16, header.record_count);
  detail::store_u64(data + 24, 0);
}

/// Parses a header; returns nullopt when the magic, the version or the record
/// size does not match this format. The reserved field is ignored.
inline std::optional<FlowHeader> decode_header(std::span<const std::byte, kFlowHeaderSize> in) {
  const std::byte* data = in.data();
  for (std::size_t i = 0; i < kFlowMagic.size(); ++i) {
    if (data[i] != static_cast<std::byte>(static_cast<unsigned char>(kFlowMagic[i]))) {
      return std::nullopt;
    }
  }
  if (detail::load_u32(data + 8) != kFlowVersion) {
    return std::nullopt;
  }
  if (detail::load_u32(data + 12) != static_cast<std::uint32_t>(kFlowRecordSize)) {
    return std::nullopt;
  }
  return FlowHeader{detail::load_u64(data + 16)};
}

}  // namespace lob::flowfile
