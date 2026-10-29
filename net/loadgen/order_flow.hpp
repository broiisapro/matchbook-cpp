#pragma once

// Seeded order flow for the load generator. One OrderFlow per session produces
// a stream of request shapes (new order, cancel, modify) that the runner turns
// into wire frames. The generator is deliberately local rather than a reuse of
// lob_opgen: the generator's job here is a plausible flow of requests a client
// would send, not a model of the engine's book, and every client order id has
// to stay inside the 40 bits the gateway reserves for it (decision C54).
//
// Randomness is std::mt19937_64 with integer arithmetic only, so a seed always
// produces the same flow on every standard library.

#include <cstdint>
#include <random>
#include <vector>

#include "lob/types.hpp"
#include "net/protocol/messages.hpp"

namespace lob::loadgen {

/// Client order ids must fit the low 40 bits of the gateway's packed order id:
/// gateway id = (session_id << 40) | client_order_id (decision C54).
inline constexpr lob::OrderId kClientOrderIdBits = 40;
inline constexpr lob::OrderId kMaxClientOrderId = (lob::OrderId{1} << kClientOrderIdBits) - 1;

/// The masked order id: what a response's order id says about the client id
/// that produced it. Used to correlate responses with requests.
constexpr lob::OrderId client_order_id_of(lob::OrderId order_id) {
  return order_id & kMaxClientOrderId;
}

/// One request the load generator will send. `kind` selects which wire message
/// is built; fields that do not apply are zero.
struct Request {
  lob::net::MessageType kind = lob::net::MessageType::NewOrder;
  lob::OrderId order_id = 0;  ///< client order id, always <= kMaxClientOrderId
  lob::Side side = lob::Side::Buy;
  lob::OrderType order_type = lob::OrderType::Limit;
  lob::Price price = 0;
  lob::Qty qty = 0;

  friend bool operator==(const Request&, const Request&) = default;
};

/// Per-session order-flow generator. The mix is roughly 70% new orders, 20%
/// cancels and 10% modifies; new orders are mostly limit orders on both sides
/// near a slowly drifting mid, with a minority of IOC, market and deliberately
/// crossing limits. Cancels and modifies target orders the generator believes
/// are live; the engine can still reject one when another session traded the
/// order first, which is a normal exchange event.
class OrderFlow {
 public:
  /// `min_price` and `max_price` bound every limit price the generator emits.
  OrderFlow(std::uint64_t seed, lob::Price min_price, lob::Price max_price);

  /// Next request in the stream.
  Request next();

  /// Requests produced so far.
  std::uint64_t generated() const { return generated_; }

  /// Orders the generator currently expects to be resting.
  std::size_t live_orders() const { return live_.size(); }

 private:
  struct LiveOrder {
    lob::OrderId id;
    lob::Side side;
    lob::Price price;
    lob::Qty qty;
  };

  std::uint64_t below(std::uint64_t bound);
  bool chance(std::uint32_t percent);
  lob::Price clamp_price(lob::Price price) const;
  lob::Price normal_price(lob::Side side);
  lob::Price crossing_price(lob::Side side);

  Request make_submit();
  Request make_cancel();
  Request make_modify();
  void remember(lob::OrderId id, lob::Side side, lob::Price price, lob::Qty qty);
  void forget(std::size_t index);

  std::mt19937_64 rng_;
  lob::Price min_price_;
  lob::Price max_price_;
  lob::Price mid_;
  lob::OrderId next_id_ = 1;
  std::uint64_t generated_ = 0;
  std::vector<LiveOrder> live_;
};

}  // namespace lob::loadgen
