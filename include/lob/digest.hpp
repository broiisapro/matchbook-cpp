#pragma once

// The builder stores its XXH64 state inline (no allocation), which requires
// the state struct definition from the advanced API. This header is the first
// and only includer of xxhash.h in the project, so the setting is consistent.
#define XXH_STATIC_LINKING_ONLY
#include <xxhash.h>

#include <cstddef>
#include <cstdint>

#include "lob/types.hpp"

namespace lob {

/// Streams the canonical byte representation of a book into XXH64 and hashes
/// it (seed 0). The byte stream is the digest spec of the architecture
/// document: a version byte, then bid levels from best to worst followed by
/// ask levels from best to worst; per level a side byte, a little-endian i64
/// price and a little-endian u32 order count; then each resting order in FIFO
/// order, little-endian u64 id and i64 remaining quantity. Integers are
/// written byte by byte so the layout is independent of host endianness and
/// struct padding.
///
/// Engines walk their own structures and call level()/order() in that order;
/// the builder only hashes bytes.
class DigestBuilder {
 public:
  DigestBuilder() : state_{} {
    XXH64_reset(&state_, 0);
    put_u8(0x01);  // stream version
  }

  /// Starts a level; follow with one order() call per resting order in FIFO
  /// order.
  void level(Side side, Price price, std::uint32_t order_count) {
    put_u8(static_cast<std::uint8_t>(side));
    put_i64(price);
    put_u32(order_count);
  }

  /// Adds one resting order at the current level.
  void order(OrderId id, Qty remaining_qty) {
    put_u64(id);
    put_i64(remaining_qty);
  }

  /// Returns the XXH64 digest of everything streamed so far.
  std::uint64_t finish() { return XXH64_digest(&state_); }

 private:
  void put_u8(std::uint8_t value) {
    const auto byte = static_cast<unsigned char>(value);
    XXH64_update(&state_, &byte, 1);
  }

  void put_u32(std::uint32_t value) {
    unsigned char bytes[4];
    for (std::size_t i = 0; i < 4; ++i) {
      bytes[i] = static_cast<unsigned char>((value >> (8 * i)) & 0xffU);
    }
    XXH64_update(&state_, bytes, 4);
  }

  void put_u64(std::uint64_t value) {
    unsigned char bytes[8];
    for (std::size_t i = 0; i < 8; ++i) {
      bytes[i] = static_cast<unsigned char>((value >> (8 * i)) & 0xffU);
    }
    XXH64_update(&state_, bytes, 8);
  }

  void put_i64(std::int64_t value) {
    // Conversion is modulo 2^64, so the byte stream is the two's-complement
    // representation.
    put_u64(static_cast<std::uint64_t>(value));
  }

  XXH64_state_t state_;
};

}  // namespace lob
