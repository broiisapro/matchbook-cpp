#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include "lob/config.hpp"
#include "lob/listener.hpp"
#include "lob/types.hpp"

namespace lob {

// Defined in include/lob/order_book.hpp; forward-declared here so this header
// stays free of fast-engine headers while the signature stays identical.
struct LevelInfo;

}  // namespace lob

namespace lob::ref {

/// Naive reference implementation of the book, mirroring the OrderBook public
/// API exactly so the differential harness can be a template over the engine
/// type. It is deliberately simple and never optimized; the implementation
/// lands in Epic 1 (WS B). It shares no engine code with lob::OrderBook.
///
/// The event sequences are identical to OrderBook: see include/lob/order_book.hpp
/// for the contract summary per method.
class ReferenceBook {
 public:
  ReferenceBook(const Config& config, Listener& listener);

  ReferenceBook(const ReferenceBook&) = delete;
  ReferenceBook& operator=(const ReferenceBook&) = delete;
  ReferenceBook(ReferenceBook&&) = delete;
  ReferenceBook& operator=(ReferenceBook&&) = delete;

  void submit(OrderId id, Side side, OrderType type, Price price, Qty qty);
  void cancel(OrderId id);
  void modify(OrderId id, Price new_price, Qty new_qty);
  std::optional<Price> best_bid() const;
  std::optional<Price> best_ask() const;
  Qty level_qty(Side side, Price price) const;
  std::size_t top_levels(Side side, std::span<LevelInfo> out) const;
  std::size_t order_count() const;
  std::uint64_t digest() const;

 private:
  // Frozen members; the Epic 1 implementation reads them.
  [[maybe_unused]] Config config_;
  [[maybe_unused]] Listener& listener_;
};

}  // namespace lob::ref
