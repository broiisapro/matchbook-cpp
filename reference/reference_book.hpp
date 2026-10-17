#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <list>
#include <map>
#include <optional>
#include <span>
#include <unordered_map>

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
  struct RestingOrder {
    OrderId id;
    Qty qty;
  };

  using OrderQueue = std::list<RestingOrder>;
  using BidLevels = std::map<Price, OrderQueue, std::greater<Price>>;
  using AskLevels = std::map<Price, OrderQueue, std::less<Price>>;

  struct Location {
    Side side;
    Price price;
    OrderQueue::iterator order;
  };

  /// Removes a resting order from its level and the id index; returns the
  /// quantity that was still resting.
  Qty erase_order(const Location& location);

  /// Appends an order at the back of its price level and reports the new level
  /// quantity.
  void rest_order(Side side, Price price, OrderId id, Qty qty);

  /// Rests the remainder of a limit order when capacity allows, otherwise
  /// reports it as cancelled.
  void rest_remainder(OrderId id, Side side, Price price, Qty qty);

  /// Number of levels currently used on a side.
  std::size_t level_count(Side side) const;

  void notify_accept(OrderId id);
  void notify_reject(OrderId id, RejectReason reason);
  void notify_trade(OrderId maker_id, OrderId taker_id, Price price, Qty qty);
  void notify_cancel(OrderId id, Qty cancelled_qty);
  void notify_modify(OrderId id, Price price, Qty new_qty);
  void notify_book_update(Side side, Price price, Qty new_level_qty);

  Config config_;
  Listener& listener_;
  BidLevels bids_;
  AskLevels asks_;
  std::unordered_map<OrderId, Location> index_;
  bool in_callback_ = false;
};

}  // namespace lob::ref
