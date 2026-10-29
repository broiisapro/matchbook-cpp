#include "reference/reference_book.hpp"

#include <algorithm>
#include <cassert>

#include "lob/digest.hpp"
#include "lob/order_book.hpp"

namespace lob::ref {
namespace {

template <class Queue>
Qty queue_qty(const Queue& queue) {
  Qty total = 0;
  for (const auto& order : queue) {
    total += order.qty;
  }
  return total;
}

/// Total quantity resting on the opposite side that an order with this limit
/// price crosses: every ask at or below the price for a buy, every bid at or
/// above it for a sell. The maps are ordered best price first, so the walk
/// stops at the first level that no longer crosses.
template <class Levels>
Qty crossing_qty(const Levels& levels, Side side, Price limit_price) {
  Qty total = 0;
  for (const auto& [level_price, queue] : levels) {
    if (side == Side::Buy ? level_price > limit_price : level_price < limit_price) {
      break;
    }
    total += queue_qty(queue);
  }
  return total;
}

}  // namespace

ReferenceBook::ReferenceBook(const Config& config, Listener& listener)
    : config_(config), listener_(listener) {}

void ReferenceBook::submit(OrderId id, Side side, OrderType type, Price price, Qty qty) {
  assert(!in_callback_);

  if (qty <= 0) {
    notify_reject(id, RejectReason::InvalidQuantity);
    return;
  }
  if (type != OrderType::Market && (price < config_.min_price || price > config_.max_price)) {
    notify_reject(id, RejectReason::InvalidPrice);
    return;
  }
  if (index_.find(id) != index_.end()) {
    notify_reject(id, RejectReason::DuplicateId);
    return;
  }

  // PostOnly may only add liquidity: it is rejected before acceptance when the
  // opposite side's best price already crosses its limit price. The book is
  // single threaded and unchanged between here and the rest of the call, so
  // this is the state the operation is applied to (section 4.1.2).
  if (type == OrderType::PostOnly) {
    const bool crosses = side == Side::Buy ? (!asks_.empty() && asks_.begin()->first <= price)
                                           : (!bids_.empty() && bids_.begin()->first >= price);
    if (crosses) {
      notify_reject(id, RejectReason::WouldCross);
      return;
    }
  }

  notify_accept(id);

  Qty remaining = qty;

  // Fill or kill: the order may only trade if the whole quantity can be filled
  // right now. Summing the crossing quantity first means a shortfall is seen
  // before the first trade, so no trade or book update escapes (section 4.1.1).
  if (type == OrderType::FOK) {
    // The two sides use different map types (opposite comparators), so the
    // walk is selected per side.
    const Qty available =
        side == Side::Buy ? crossing_qty(asks_, side, price) : crossing_qty(bids_, side, price);
    if (available < qty) {
      notify_cancel(id, qty);
      return;
    }
  }

  if (side == Side::Buy) {
    while (remaining > 0 && !asks_.empty()) {
      auto level_it = asks_.begin();
      const Price maker_price = level_it->first;
      if (type != OrderType::Market && maker_price > price) {
        break;
      }

      auto order_it = level_it->second.begin();
      const OrderId maker_id = order_it->id;
      const Qty maker_qty = order_it->qty;
      const Qty fill = std::min(remaining, maker_qty);

      notify_trade(maker_id, id, maker_price, fill);
      remaining -= fill;

      if (fill == maker_qty) {
        index_.erase(maker_id);
        level_it->second.erase(order_it);
        const Qty new_level_qty = queue_qty(level_it->second);
        if (level_it->second.empty()) {
          asks_.erase(level_it);
        }
        notify_book_update(Side::Sell, maker_price, new_level_qty);
      } else {
        order_it->qty -= fill;
        notify_book_update(Side::Sell, maker_price, queue_qty(level_it->second));
      }
    }
  } else {
    while (remaining > 0 && !bids_.empty()) {
      auto level_it = bids_.begin();
      const Price maker_price = level_it->first;
      if (type != OrderType::Market && maker_price < price) {
        break;
      }

      auto order_it = level_it->second.begin();
      const OrderId maker_id = order_it->id;
      const Qty maker_qty = order_it->qty;
      const Qty fill = std::min(remaining, maker_qty);

      notify_trade(maker_id, id, maker_price, fill);
      remaining -= fill;

      if (fill == maker_qty) {
        index_.erase(maker_id);
        level_it->second.erase(order_it);
        const Qty new_level_qty = queue_qty(level_it->second);
        if (level_it->second.empty()) {
          bids_.erase(level_it);
        }
        notify_book_update(Side::Buy, maker_price, new_level_qty);
      } else {
        order_it->qty -= fill;
        notify_book_update(Side::Buy, maker_price, queue_qty(level_it->second));
      }
    }
  }

  if (remaining <= 0) {
    return;
  }
  // Market, IOC and FOK never rest. A FOK cannot reach this point: the kill
  // check above guarantees that the crossing quantity fills it completely, so
  // the cancel is defensive only. PostOnly never crosses, so it falls through
  // to the ordinary resting path below.
  if (type == OrderType::Market || type == OrderType::IOC || type == OrderType::FOK) {
    notify_cancel(id, remaining);
    return;
  }
  rest_remainder(id, side, price, remaining);
}

void ReferenceBook::cancel(OrderId id) {
  assert(!in_callback_);

  const auto found = index_.find(id);
  if (found == index_.end()) {
    notify_reject(id, RejectReason::UnknownOrder);
    return;
  }

  const Location location = found->second;
  const Side side = location.side;
  const Price price = location.price;
  const Qty remaining = erase_order(location);

  notify_cancel(id, remaining);
  notify_book_update(side, price, level_qty(side, price));
}

void ReferenceBook::modify(OrderId id, Price new_price, Qty new_qty) {
  assert(!in_callback_);

  const auto found = index_.find(id);
  if (found == index_.end()) {
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

  const Location location = found->second;
  const Side side = location.side;
  const Price old_price = location.price;
  const Qty remaining = location.order->qty;

  if (new_price == old_price) {
    if (new_qty == remaining) {
      notify_reject(id, RejectReason::ModifyNoChange);
      return;
    }
    if (new_qty < remaining) {
      location.order->qty = new_qty;
      notify_modify(id, old_price, new_qty);
      notify_book_update(side, old_price, level_qty(side, old_price));
      return;
    }
  }

  erase_order(location);
  notify_cancel(id, remaining);
  notify_book_update(side, old_price, level_qty(side, old_price));
  submit(id, side, OrderType::Limit, new_price, new_qty);
}

std::optional<Price> ReferenceBook::best_bid() const {
  assert(!in_callback_);
  if (bids_.empty()) {
    return std::nullopt;
  }
  return bids_.begin()->first;
}

std::optional<Price> ReferenceBook::best_ask() const {
  assert(!in_callback_);
  if (asks_.empty()) {
    return std::nullopt;
  }
  return asks_.begin()->first;
}

Qty ReferenceBook::level_qty(Side side, Price price) const {
  assert(!in_callback_);
  if (side == Side::Buy) {
    const auto level_it = bids_.find(price);
    return level_it == bids_.end() ? 0 : queue_qty(level_it->second);
  }
  const auto level_it = asks_.find(price);
  return level_it == asks_.end() ? 0 : queue_qty(level_it->second);
}

std::size_t ReferenceBook::top_levels(Side side, std::span<LevelInfo> out) const {
  assert(!in_callback_);

  std::size_t written = 0;
  if (side == Side::Buy) {
    for (auto level_it = bids_.begin(); level_it != bids_.end() && written < out.size();
         ++level_it) {
      out[written] = LevelInfo{level_it->first, queue_qty(level_it->second),
                               static_cast<std::uint32_t>(level_it->second.size())};
      ++written;
    }
  } else {
    for (auto level_it = asks_.begin(); level_it != asks_.end() && written < out.size();
         ++level_it) {
      out[written] = LevelInfo{level_it->first, queue_qty(level_it->second),
                               static_cast<std::uint32_t>(level_it->second.size())};
      ++written;
    }
  }
  return written;
}

std::size_t ReferenceBook::order_count() const {
  assert(!in_callback_);
  return index_.size();
}

std::uint64_t ReferenceBook::digest() const {
  assert(!in_callback_);

  DigestBuilder builder;
  for (const auto& [price, queue] : bids_) {
    builder.level(Side::Buy, price, static_cast<std::uint32_t>(queue.size()));
    for (const auto& order : queue) {
      builder.order(order.id, order.qty);
    }
  }
  for (const auto& [price, queue] : asks_) {
    builder.level(Side::Sell, price, static_cast<std::uint32_t>(queue.size()));
    for (const auto& order : queue) {
      builder.order(order.id, order.qty);
    }
  }
  return builder.finish();
}

Qty ReferenceBook::erase_order(const Location& location) {
  const OrderId id = location.order->id;
  const Qty remaining = location.order->qty;
  index_.erase(id);

  if (location.side == Side::Buy) {
    const auto level_it = bids_.find(location.price);
    level_it->second.erase(location.order);
    if (level_it->second.empty()) {
      bids_.erase(level_it);
    }
  } else {
    const auto level_it = asks_.find(location.price);
    level_it->second.erase(location.order);
    if (level_it->second.empty()) {
      asks_.erase(level_it);
    }
  }
  return remaining;
}

void ReferenceBook::rest_order(Side side, Price price, OrderId id, Qty qty) {
  if (side == Side::Buy) {
    auto level_it = bids_.try_emplace(price).first;
    level_it->second.push_back(RestingOrder{id, qty});
    index_.emplace(id, Location{Side::Buy, price, std::prev(level_it->second.end())});
    notify_book_update(Side::Buy, price, queue_qty(level_it->second));
  } else {
    auto level_it = asks_.try_emplace(price).first;
    level_it->second.push_back(RestingOrder{id, qty});
    index_.emplace(id, Location{Side::Sell, price, std::prev(level_it->second.end())});
    notify_book_update(Side::Sell, price, queue_qty(level_it->second));
  }
}

void ReferenceBook::rest_remainder(OrderId id, Side side, Price price, Qty qty) {
  if (index_.size() >= config_.max_orders) {
    notify_cancel(id, qty);
    return;
  }

  const bool level_exists =
      side == Side::Buy ? bids_.find(price) != bids_.end() : asks_.find(price) != asks_.end();
  if (!level_exists && level_count(side) >= config_.max_levels) {
    notify_cancel(id, qty);
    return;
  }

  rest_order(side, price, id, qty);
}

std::size_t ReferenceBook::level_count(Side side) const {
  return side == Side::Buy ? bids_.size() : asks_.size();
}

void ReferenceBook::notify_accept(OrderId id) {
  in_callback_ = true;
  listener_.on_accept(id);
  in_callback_ = false;
}

void ReferenceBook::notify_reject(OrderId id, RejectReason reason) {
  in_callback_ = true;
  listener_.on_reject(id, reason);
  in_callback_ = false;
}

void ReferenceBook::notify_trade(OrderId maker_id, OrderId taker_id, Price price, Qty qty) {
  in_callback_ = true;
  listener_.on_trade(maker_id, taker_id, price, qty);
  in_callback_ = false;
}

void ReferenceBook::notify_cancel(OrderId id, Qty cancelled_qty) {
  in_callback_ = true;
  listener_.on_cancel(id, cancelled_qty);
  in_callback_ = false;
}

void ReferenceBook::notify_modify(OrderId id, Price price, Qty new_qty) {
  in_callback_ = true;
  listener_.on_modify(id, price, new_qty);
  in_callback_ = false;
}

void ReferenceBook::notify_book_update(Side side, Price price, Qty new_level_qty) {
  in_callback_ = true;
  listener_.on_book_update(side, price, new_level_qty);
  in_callback_ = false;
}

}  // namespace lob::ref
