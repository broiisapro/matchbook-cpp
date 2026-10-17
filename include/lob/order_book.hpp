#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include "lob/config.hpp"
#include "lob/id_map.hpp"
#include "lob/intrusive_list.hpp"
#include "lob/listener.hpp"
#include "lob/pool.hpp"
#include "lob/price_ladder.hpp"
#include "lob/types.hpp"

namespace lob {

/// One price level as reported by OrderBook::top_levels().
struct LevelInfo {
  Price price;
  Qty qty;
  std::uint32_t order_count;
};

/// Single-instrument, price-time-priority limit order book.
///
/// Every state change is reported to the Listener in the exact order defined
/// by the event contract; both this engine and the reference engine emit the
/// same sequence. Callbacks must not call back into the book.
///
/// The implementation lands in Epic 1; this wave ships the frozen API.
class OrderBook {
 public:
  OrderBook(const Config& config, Listener& listener);

  OrderBook(const OrderBook&) = delete;
  OrderBook& operator=(const OrderBook&) = delete;
  OrderBook(OrderBook&&) = delete;
  OrderBook& operator=(OrderBook&&) = delete;

  /// Submits an order. Validation, first failure wins and nothing else is
  /// emitted: qty <= 0 -> InvalidQuantity; limit or IOC price outside
  /// [min_price, max_price] -> InvalidPrice (market orders ignore price);
  /// live id -> DuplicateId.
  ///
  /// Otherwise on_accept is emitted, the match loop fills against the best
  /// opposite level while the remainder crosses (on_trade with the maker's
  /// resting price, then on_book_update for the maker's level), and a limit
  /// remainder rests at the back of its level with on_book_update when
  /// capacity allows or is cancelled with on_cancel(remainder) when it does
  /// not. Market and IOC remainders always emit on_cancel(remainder). Fully
  /// filled makers leave the book silently.
  void submit(OrderId id, Side side, OrderType type, Price price, Qty qty);

  /// Cancels a resting order. Unknown id -> UnknownOrder. Otherwise the order
  /// is removed: on_cancel(id, remaining) then on_book_update(side, price,
  /// new level qty).
  void cancel(OrderId id);

  /// Modifies a resting order; new_qty is the new remaining quantity. Unknown
  /// id -> UnknownOrder; new_qty <= 0 -> InvalidQuantity; new_price outside
  /// the configured range -> InvalidPrice; same price and quantity ->
  /// ModifyNoChange. A same-price reduction keeps queue position and emits
  /// on_modify then on_book_update. Any other change is cancel + new:
  /// on_cancel, on_book_update for the old level, then the full submit
  /// sequence for a limit order at the new price and quantity.
  void modify(OrderId id, Price new_price, Qty new_qty);

  /// Best resting bid, if any. Emits no events.
  std::optional<Price> best_bid() const;

  /// Best resting ask, if any. Emits no events.
  std::optional<Price> best_ask() const;

  /// Total remaining quantity resting at a level (0 when the level is empty).
  /// Emits no events.
  Qty level_qty(Side side, Price price) const;

  /// Copies up to out.size() best levels, best first, into out and returns the
  /// number written. Emits no events.
  std::size_t top_levels(Side side, std::span<LevelInfo> out) const;

  /// Number of live resting orders. Emits no events.
  std::size_t order_count() const;

  /// Digest of the book in price-time order (see include/lob/digest.hpp for
  /// the byte spec). Emits no events.
  std::uint64_t digest() const;

 private:
  /// One resting order. prev/next are pool indices of the FIFO neighbours.
  struct OrderNode {
    OrderId id = 0;
    Price price = 0;
    Qty qty = 0;
    Side side = Side::Buy;
    std::uint32_t prev = kNullIndex;
    std::uint32_t next = kNullIndex;
  };

  using OrderFifo = IntrusiveList<OrderNode, &OrderNode::prev, &OrderNode::next>;

  PriceLadder& ladder(Side side) { return side == Side::Buy ? bids_ : asks_; }
  const PriceLadder& ladder(Side side) const { return side == Side::Buy ? bids_ : asks_; }

  /// Fills the taker against the best opposite levels while they cross.
  /// Emits on_trade (at the maker's resting price) followed by
  /// on_book_update for the maker's level; fully filled makers leave the book
  /// silently and empty levels are dropped, so the best level is recomputed
  /// after every drain. Returns the unfilled remainder. A market taker
  /// crosses any level; otherwise the taker's side and limit price decide.
  Qty match(OrderId taker_id, Side side, Price limit_price, Qty remaining, bool market);

  /// Removes a live node from its level: unlinks it from the FIFO, updates the
  /// level aggregates, erases the level when it becomes empty and emits
  /// on_book_update with the new level quantity. The pool node and the id map
  /// entry are left alone for the caller.
  void unlink(std::uint32_t index);

  /// Rests a limit order remainder at the back of its level and emits
  /// on_book_update, or emits on_cancel when the order or level capacity is
  /// exhausted.
  void rest(OrderId id, Side side, Price price, Qty qty);

  Config config_;
  Listener& listener_;
  ObjectPool<OrderNode> orders_;
  IdMap ids_;
  PriceLadder bids_;
  PriceLadder asks_;
};

}  // namespace lob
