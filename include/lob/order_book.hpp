#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include "lob/array_ladder.hpp"
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

  /// The price-level container this engine was compiled against. The build
  /// option LOB_PRICE_LADDER selects the std::map tree (Epic 1 baseline) or
  /// the bitmap-indexed array ladder (Epic 2); both implement the same
  /// interface and produce identical events.
#if defined(LOB_PRICE_LADDER_ARRAY)
  using Ladder = ArrayPriceLadder;
#else
  using Ladder = PriceLadder;
#endif

  Ladder& ladder(Side side) { return side == Side::Buy ? bids_ : asks_; }
  const Ladder& ladder(Side side) const { return side == Side::Buy ? bids_ : asks_; }

  /// Places an order whose validation has already passed: emits on_accept,
  /// matches, then rests a limit remainder under the capacity rules or
  /// cancels a market/IOC remainder. A FOK is killed after on_accept when the
  /// crossing quantity is short, and a PostOnly (which never crosses here)
  /// always rests. Shared by submit and by modify's cancel + new path.
  void place(OrderId id, Side side, OrderType type, Price price, Qty qty);

  /// Removes a live order: on_cancel(id, remaining), unlink (which emits
  /// on_book_update), id map erase and pool release.
  void remove_live(std::uint32_t index);

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

  /// True when the best price on the opposite side already crosses this limit
  /// price, i.e. a PostOnly order at this price would take liquidity
  /// (section 4.1.2).
  bool would_cross(Side side, Price price) const;

  /// True when the resting opposite side holds at least `qty` at the levels
  /// this limit price crosses (section 4.1.1). Walks the levels best first
  /// without mutating the book and stops as soon as the quantity is covered.
  bool can_fill_immediately(Side side, Price price, Qty qty) const;

  // Listener callbacks are routed through these helpers so the reentrancy
  // guard is armed for exactly as long as a callback is in flight.
  void notify_accept(OrderId id);
  void notify_reject(OrderId id, RejectReason reason);
  void notify_trade(OrderId maker_id, OrderId taker_id, Price price, Qty qty);
  void notify_cancel(OrderId id, Qty cancelled_qty);
  void notify_modify(OrderId id, Price price, Qty new_qty);
  void notify_book_update(Side side, Price price, Qty new_level_qty);

  /// Asserts in debug builds that a listener callback does not call back into
  /// the book.
  class CallbackScope {
   public:
    explicit CallbackScope(OrderBook& book) noexcept : book_(book) {
      assert(!book_.in_callback_ && "reentrancy: a listener callback called back into the book");
      book_.in_callback_ = true;
    }
    ~CallbackScope() { book_.in_callback_ = false; }

    CallbackScope(const CallbackScope&) = delete;
    CallbackScope& operator=(const CallbackScope&) = delete;
    CallbackScope(CallbackScope&&) = delete;
    CallbackScope& operator=(CallbackScope&&) = delete;

   private:
    OrderBook& book_;
  };

  friend struct OrderBookTestAccess;

  Config config_;
  Listener& listener_;
  ObjectPool<OrderNode> orders_;
  IdMap ids_;
  Ladder bids_;
  Ladder asks_;
  bool in_callback_ = false;
};

}  // namespace lob
