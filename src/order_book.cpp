#include "lob/order_book.hpp"

#include <algorithm>
#include <array>
#include <cassert>

#include "lob/digest.hpp"

namespace lob {

OrderBook::OrderBook(const Config& config, Listener& listener)
    : config_(config),
      listener_(listener),
      orders_(config.max_orders),
      ids_(config.max_orders),
      bids_(Side::Buy, config.min_price, config.max_price, config.max_levels),
      asks_(Side::Sell, config.min_price, config.max_price, config.max_levels) {
  assert(valid(config) && "OrderBook requires a valid Config");
}

void OrderBook::submit(OrderId id, Side side, OrderType type, Price price, Qty qty) {
  assert(!in_callback_ && "reentrancy: a listener callback called back into the book");

  if (qty <= 0) {
    notify_reject(id, RejectReason::InvalidQuantity);
    return;
  }
  if (type != OrderType::Market && (price < config_.min_price || price > config_.max_price)) {
    notify_reject(id, RejectReason::InvalidPrice);
    return;
  }
  if (ids_.find(id) != IdMap::kNull) {
    notify_reject(id, RejectReason::DuplicateId);
    return;
  }

  place(id, side, type, price, qty);
}

void OrderBook::cancel(OrderId id) {
  assert(!in_callback_ && "reentrancy: a listener callback called back into the book");

  const std::uint32_t index = ids_.find(id);
  if (index == IdMap::kNull) {
    notify_reject(id, RejectReason::UnknownOrder);
    return;
  }

  remove_live(index);
}

void OrderBook::modify(OrderId id, Price new_price, Qty new_qty) {
  assert(!in_callback_ && "reentrancy: a listener callback called back into the book");

  const std::uint32_t index = ids_.find(id);
  if (index == IdMap::kNull) {
    notify_reject(id, RejectReason::UnknownOrder);
    return;
  }
  if (new_qty <= 0) {
    notify_reject(id, RejectReason::InvalidQuantity);
    return;
  }
  if (new_price < config_.min_price || new_price > config_.max_price) {
    notify_reject(id, RejectReason::InvalidPrice);
    return;
  }

  OrderNode& node = orders_[index];
  const Side side = node.side;
  const Price old_price = node.price;
  const Qty remaining = node.qty;

  if (new_price == old_price) {
    if (new_qty == remaining) {
      notify_reject(id, RejectReason::ModifyNoChange);
      return;
    }
    if (new_qty < remaining) {
      Ladder& book = ladder(side);
      Level* level = book.find(old_price);
      assert(level != nullptr && "every live order sits on an existing level");
      notify_modify(id, old_price, new_qty);
      level->total_qty -= remaining - new_qty;
      node.qty = new_qty;
      notify_book_update(side, old_price, level->total_qty);
      return;
    }
  }

  remove_live(index);
  place(id, side, OrderType::Limit, new_price, new_qty);
}

void OrderBook::place(OrderId id, Side side, OrderType type, Price price, Qty qty) {
  notify_accept(id);

  Qty remaining = qty;
  switch (type) {
    case OrderType::Limit:
      remaining = match(id, side, price, remaining, /*market=*/false);
      if (remaining > 0) {
        rest(id, side, price, remaining);
      }
      return;
    case OrderType::Market:
      remaining = match(id, side, price, remaining, /*market=*/true);
      break;
    case OrderType::IOC:
      remaining = match(id, side, price, remaining, /*market=*/false);
      break;
  }
  if (remaining > 0) {
    notify_cancel(id, remaining);
  }
}

void OrderBook::remove_live(std::uint32_t index) {
  OrderNode& node = orders_[index];
  const OrderId id = node.id;
  const Qty remaining = node.qty;
  notify_cancel(id, remaining);
  unlink(index);
  ids_.erase(id);
  orders_.release(index);
}

std::optional<Price> OrderBook::best_bid() const {
  assert(!in_callback_ && "reentrancy: a listener callback called back into the book");
  return bids_.best_price();
}

std::optional<Price> OrderBook::best_ask() const {
  assert(!in_callback_ && "reentrancy: a listener callback called back into the book");
  return asks_.best_price();
}

Qty OrderBook::level_qty(Side side, Price price) const {
  assert(!in_callback_ && "reentrancy: a listener callback called back into the book");
  const Level* level = ladder(side).find(price);
  return level == nullptr ? 0 : level->total_qty;
}

std::size_t OrderBook::top_levels(Side side, std::span<LevelInfo> out) const {
  assert(!in_callback_ && "reentrancy: a listener callback called back into the book");

  std::size_t written = 0;
  ladder(side).for_each_best_first([&](Price price, const Level& level) {
    if (written == out.size()) {
      return false;
    }
    out[written] = LevelInfo{price, level.total_qty, level.order_count};
    ++written;
    return true;
  });
  return written;
}

std::size_t OrderBook::order_count() const {
  assert(!in_callback_ && "reentrancy: a listener callback called back into the book");
  return orders_.size();
}

std::uint64_t OrderBook::digest() const {
  assert(!in_callback_ && "reentrancy: a listener callback called back into the book");

  DigestBuilder builder;
  constexpr std::array sides{Side::Buy, Side::Sell};
  for (const Side side : sides) {
    ladder(side).for_each_best_first([&](Price price, const Level& level) {
      builder.level(side, price, level.order_count);
      for (std::uint32_t index = level.head; index != kNullIndex; index = orders_[index].next) {
        const OrderNode& node = orders_[index];
        builder.order(node.id, node.qty);
      }
      return true;
    });
  }
  return builder.finish();
}

Qty OrderBook::match(OrderId taker_id, Side side, Price limit_price, Qty remaining, bool market) {
  Ladder& opposite_book = ladder(opposite(side));
  while (remaining > 0 && !opposite_book.empty()) {
    const Price maker_price = *opposite_book.best_price();
    const bool crosses =
        market || (side == Side::Buy ? maker_price <= limit_price : maker_price >= limit_price);
    if (!crosses) {
      break;
    }

    Level* level = opposite_book.best();
    const std::uint32_t maker_index = level->head;
    OrderNode& maker = orders_[maker_index];
    const Qty fill = std::min(remaining, maker.qty);
    const OrderId maker_id = maker.id;

    notify_trade(maker_id, taker_id, maker_price, fill);
    remaining -= fill;
    maker.qty -= fill;
    level->total_qty -= fill;

    const bool fully_filled = maker.qty == 0;
    if (fully_filled) {
      OrderFifo fifo(orders_, level->head, level->tail);
      fifo.erase(maker_index);
      --level->order_count;
      ids_.erase(maker_id);
    }

    const Qty new_level_qty = level->total_qty;
    notify_book_update(opposite(side), maker_price, new_level_qty);
    opposite_book.erase_if_empty(maker_price);
    if (fully_filled) {
      orders_.release(maker_index);
    }
  }
  return remaining;
}

void OrderBook::unlink(std::uint32_t index) {
  OrderNode& node = orders_[index];
  Ladder& book = ladder(node.side);
  Level* level = book.find(node.price);
  assert(level != nullptr && "every live order sits on an existing level");

  OrderFifo fifo(orders_, level->head, level->tail);
  fifo.erase(index);
  level->total_qty -= node.qty;
  --level->order_count;
  const Qty new_level_qty = level->total_qty;
  const Side side = node.side;
  const Price price = node.price;
  book.erase_if_empty(price);
  notify_book_update(side, price, new_level_qty);
}

void OrderBook::rest(OrderId id, Side side, Price price, Qty qty) {
  Ladder& book = ladder(side);
  const bool level_exists = book.find(price) != nullptr;
  const bool order_capacity_full = orders_.size() >= config_.max_orders;
  const bool level_capacity_full = !level_exists && book.level_count() >= config_.max_levels;
  if (order_capacity_full || level_capacity_full) {
    notify_cancel(id, qty);
    return;
  }

  const std::uint32_t index = orders_.acquire();
  assert(index != ObjectPool<OrderNode>::kNull && "capacity was checked above");
  OrderNode& node = orders_[index];
  node.id = id;
  node.price = price;
  node.qty = qty;
  node.side = side;

  const bool inserted = ids_.insert(id, index);
  assert(inserted && "a duplicate id was rejected before resting");
  static_cast<void>(inserted);

  Level* level = book.find_or_insert(price);
  assert(level != nullptr && "capacity was checked above");
  OrderFifo fifo(orders_, level->head, level->tail);
  fifo.push_back(index);
  level->total_qty += qty;
  ++level->order_count;
  notify_book_update(side, price, level->total_qty);
}

void OrderBook::notify_accept(OrderId id) {
  CallbackScope scope(*this);
  listener_.on_accept(id);
}

void OrderBook::notify_reject(OrderId id, RejectReason reason) {
  CallbackScope scope(*this);
  listener_.on_reject(id, reason);
}

void OrderBook::notify_trade(OrderId maker_id, OrderId taker_id, Price price, Qty qty) {
  CallbackScope scope(*this);
  listener_.on_trade(maker_id, taker_id, price, qty);
}

void OrderBook::notify_cancel(OrderId id, Qty cancelled_qty) {
  CallbackScope scope(*this);
  listener_.on_cancel(id, cancelled_qty);
}

void OrderBook::notify_modify(OrderId id, Price price, Qty new_qty) {
  CallbackScope scope(*this);
  listener_.on_modify(id, price, new_qty);
}

void OrderBook::notify_book_update(Side side, Price price, Qty new_level_qty) {
  CallbackScope scope(*this);
  listener_.on_book_update(side, price, new_level_qty);
}

}  // namespace lob
