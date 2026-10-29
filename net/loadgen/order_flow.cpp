#include "net/loadgen/order_flow.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>

namespace lob::loadgen {
namespace {

/// Share of the request stream that is new orders, cancels and modifies. The
/// split is fixed rather than configurable: the load generator measures the
/// transport, so the mix only has to be plausible and stable between runs.
constexpr std::uint64_t kSubmitPercent = 70;
constexpr std::uint64_t kCancelPercent = 20;

/// Share of new orders that are limit orders, IOC and market orders.
constexpr std::uint64_t kLimitPercent = 85;
constexpr std::uint64_t kIocPercent = 9;

/// Percent of limit orders priced through the mid so the flow produces trades,
/// and of IOC orders that take liquidity rather than only resting-or-killing.
constexpr std::uint32_t kCrossingLimitPercent = 12;
constexpr std::uint32_t kCrossingIocPercent = 50;

/// Half-width of the band around the mid that resting prices are drawn from.
constexpr std::uint64_t kRestingBand = 5;
constexpr std::uint64_t kCrossingBand = 4;

/// Half-width of the modify price move, in ticks.
constexpr std::uint64_t kMoveBand = 3;

constexpr lob::Qty kMaxOrderQty = 100;

}  // namespace

OrderFlow::OrderFlow(std::uint64_t seed, lob::Price min_price, lob::Price max_price)
    : rng_(seed), min_price_(min_price), max_price_(max_price) {
  if (min_price > max_price) {
    throw std::invalid_argument("OrderFlow: min_price " + std::to_string(min_price) +
                                " is above max_price " + std::to_string(max_price));
  }
  mid_ = min_price + (max_price - min_price) / 2;
}

std::uint64_t OrderFlow::below(std::uint64_t bound) { return bound == 0 ? 0 : rng_() % bound; }

bool OrderFlow::chance(std::uint32_t percent) { return below(100) < percent; }

lob::Price OrderFlow::clamp_price(lob::Price price) const {
  return std::clamp(price, min_price_, max_price_);
}

lob::Price OrderFlow::normal_price(lob::Side side) {
  const auto offset = static_cast<lob::Price>(1 + below(kRestingBand));
  return clamp_price(side == lob::Side::Buy ? mid_ - offset : mid_ + offset);
}

lob::Price OrderFlow::crossing_price(lob::Side side) {
  const auto offset = static_cast<lob::Price>(1 + below(kCrossingBand));
  return clamp_price(side == lob::Side::Buy ? mid_ + offset : mid_ - offset);
}

void OrderFlow::remember(lob::OrderId id, lob::Side side, lob::Price price, lob::Qty qty) {
  live_.push_back(LiveOrder{id, side, price, qty});
}

void OrderFlow::forget(std::size_t index) {
  if (index + 1 != live_.size()) {
    live_[index] = live_.back();
  }
  live_.pop_back();
}

Request OrderFlow::make_submit() {
  const lob::Side side = below(2) == 0 ? lob::Side::Buy : lob::Side::Sell;
  const std::uint64_t shape = below(100);

  Request request;
  request.kind = lob::net::MessageType::NewOrder;
  request.side = side;
  request.qty = static_cast<lob::Qty>(1 + below(kMaxOrderQty));

  if (shape < kLimitPercent) {
    request.order_type = lob::OrderType::Limit;
    request.price = chance(kCrossingLimitPercent) ? crossing_price(side) : normal_price(side);
  } else if (shape < kLimitPercent + kIocPercent) {
    request.order_type = lob::OrderType::IOC;
    request.price = chance(kCrossingIocPercent) ? crossing_price(side) : normal_price(side);
  } else {
    request.order_type = lob::OrderType::Market;
    request.price = 0;
  }

  if (next_id_ > kMaxClientOrderId) {
    throw std::overflow_error("OrderFlow: client order ids exhausted the 40-bit range");
  }
  request.order_id = next_id_++;
  if (request.order_type == lob::OrderType::Limit) {
    remember(request.order_id, request.side, request.price, request.qty);
  }
  return request;
}

Request OrderFlow::make_cancel() {
  const std::size_t index = static_cast<std::size_t>(below(live_.size()));
  const lob::OrderId id = live_[index].id;
  forget(index);

  Request request;
  request.kind = lob::net::MessageType::Cancel;
  request.order_id = id;
  return request;
}

Request OrderFlow::make_modify() {
  const std::size_t index = static_cast<std::size_t>(below(live_.size()));
  LiveOrder& order = live_[index];

  const auto delta = static_cast<lob::Qty>(1 + below(kRestingBand));
  lob::Qty qty = chance(50) ? order.qty + delta : order.qty - delta;
  qty = std::max<lob::Qty>(1, std::min<lob::Qty>(qty, kMaxOrderQty));
  if (chance(25)) {
    const auto move = static_cast<lob::Price>(1 + below(kMoveBand));
    order.price = clamp_price(order.price + (chance(50) ? -move : move));
  }
  order.qty = qty;

  Request request;
  request.kind = lob::net::MessageType::Modify;
  request.order_id = order.id;
  request.side = order.side;
  request.order_type = lob::OrderType::Limit;
  request.price = order.price;
  request.qty = order.qty;
  return request;
}

Request OrderFlow::next() {
  Request request;
  const std::uint64_t roll = below(100);
  if (roll < kSubmitPercent || live_.empty()) {
    request = make_submit();
  } else if (roll < kSubmitPercent + kCancelPercent) {
    request = make_cancel();
  } else {
    request = make_modify();
  }

  // Slow mid random walk (-1, 0, +1 per request) keeps the two sides close
  // together without clustering every order on one price.
  mid_ = clamp_price(mid_ + static_cast<lob::Price>(below(3)) - 1);
  ++generated_;
  return request;
}

}  // namespace lob::loadgen
