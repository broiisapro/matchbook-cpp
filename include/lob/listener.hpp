#pragma once

#include "lob/types.hpp"

namespace lob {

/// Event sink shared by both engines. Callbacks are synchronous, arrive in the
/// order defined by the event contract, and must not call back into the book
/// (debug builds assert on reentrancy).
class Listener {
 public:
  virtual ~Listener() = default;

  /// The order passed validation and enters the book.
  virtual void on_accept(OrderId id) = 0;
  /// The order was rejected; nothing else is emitted for it.
  virtual void on_reject(OrderId id, RejectReason reason) = 0;
  /// A maker was filled by a taker at the resting price.
  virtual void on_trade(OrderId maker_id, OrderId taker_id, Price price, Qty qty) = 0;
  /// The remaining quantity of an order left the book without trading.
  virtual void on_cancel(OrderId id, Qty cancelled_qty) = 0;
  /// In-place quantity reduction (same price, same queue position).
  virtual void on_modify(OrderId id, Price price, Qty new_qty) = 0;
  /// A price level changed; new_level_qty == 0 means the level is gone.
  virtual void on_book_update(Side side, Price price, Qty new_level_qty) = 0;
};

/// Discards every event.
class NullListener final : public Listener {
 public:
  void on_accept(OrderId) override {}
  void on_reject(OrderId, RejectReason) override {}
  void on_trade(OrderId, OrderId, Price, Qty) override {}
  void on_cancel(OrderId, Qty) override {}
  void on_modify(OrderId, Price, Qty) override {}
  void on_book_update(Side, Price, Qty) override {}
};

}  // namespace lob
