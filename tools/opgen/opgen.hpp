#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <list>
#include <map>
#include <ostream>
#include <random>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "lob/config.hpp"
#include "lob/types.hpp"

namespace lob::opgen {

/// Kind of a generated operation. Values are local to the generator; the flow
/// file wire format has its own encoding.
enum class OpKind : std::uint8_t { Submit = 0, Cancel = 1, Modify = 2 };

/// One generated operation. Cancels ignore side, type, price and qty; market
/// orders ignore price (carried as 0); modifies ignore type.
struct Op {
  OpKind kind = OpKind::Submit;
  OrderId id = 0;
  Side side = Side::Buy;
  OrderType type = OrderType::Limit;
  Price price = 0;
  Qty qty = 0;

  friend bool operator==(const Op&, const Op&) = default;
};

std::ostream& operator<<(std::ostream& out, const Op& op);
std::ostream& operator<<(std::ostream& out, OpKind kind);
std::string_view to_string(OpKind kind);

/// Relative weights for the generator. Operation-kind weights are relative to
/// each other; the attribute fields are thresholds on [0, 100) and a value of
/// zero disables that attribute. The generator never uses floating point, so
/// the same seed and mix always produce the same stream on every standard
/// library.
struct OpMix {
  // Operation kind (relative weights).
  std::uint32_t submit_weight = 70;
  std::uint32_t cancel_weight = 20;
  std::uint32_t modify_weight = 10;
  // Submit shape (percent).
  std::uint32_t market_weight = 6;
  std::uint32_t ioc_weight = 10;
  std::uint32_t crossing_weight = 20;
  std::uint32_t edge_price_weight = 12;
  std::uint32_t invalid_qty_weight = 4;
  std::uint32_t duplicate_id_weight = 5;
  std::uint32_t reuse_id_weight = 8;
  // Target selection for cancels and modifies (percent); otherwise unknown.
  std::uint32_t unknown_id_weight = 8;
  // Modify modes (relative weights) and the invalid-price mode (percent).
  std::uint32_t no_change_weight = 10;
  std::uint32_t reduce_weight = 25;
  std::uint32_t increase_weight = 20;
  std::uint32_t move_weight = 30;
  std::uint32_t invalid_price_weight = 8;
};

/// Deterministic operation stream. The generator keeps its own view of the
/// orders it has submitted (as submitted, not as filled) so cancels and
/// modifies usually target ids that are still live in the engine: the head and
/// tail of the queue at the best modelled level, or a random live id. It also
/// produces unknown ids, duplicate live ids, ids reused after a cancel,
/// crossing orders, prices at the configured edges and just outside, invalid
/// quantities, and market/IOC orders against possibly empty sides. A mid price
/// random walk keeps the modelled book deep.
///
/// Randomness is std::mt19937_64 only; every distribution is implemented in
/// this file with integer arithmetic. No std::*_distribution is used, so the
/// stream is portable across standard libraries.
class OpGenerator {
 public:
  OpGenerator(std::uint64_t seed, Config config, OpMix mix = {});

  /// Generates the next operation and updates the generator's book view.
  Op next();

  const Config& config() const { return config_; }
  const OpMix& mix() const { return mix_; }

 private:
  struct ModelLocation {
    Side side;
    Price price;
    Qty qty;
    std::list<OrderId>::iterator order;
  };

  struct Target {
    OrderId id;
    bool known;
  };

  std::uint64_t below(std::uint64_t bound);
  bool chance(std::uint32_t percent);
  Price uniform_price(Price lo, Price hi);

  Op next_submit();
  Op next_cancel();
  Op next_modify();

  Price choose_submit_price(Side side);
  Price choose_move_price(Price old_price);
  Price choose_edge_price();
  Qty choose_invalid_qty();

  Target pick_target();
  OrderId pick_head_at_best();
  OrderId pick_tail_at_best();

  void model_add(Side side, Price price, OrderId id, Qty qty);
  void model_remove(OrderId id, bool reusable);
  void model_update_qty(OrderId id, Qty qty);

  std::mt19937_64 rng_;
  Config config_;
  OpMix mix_;
  Price mid_;

  std::uint64_t next_id_ = 1;
  std::uint64_t next_unknown_id_ = 1ULL << 63;

  std::unordered_map<OrderId, ModelLocation> model_;
  std::vector<OrderId> live_ids_;
  std::unordered_map<OrderId, std::size_t> live_index_;
  std::vector<OrderId> reusable_ids_;

  std::map<Price, std::list<OrderId>, std::greater<Price>> bid_levels_;
  std::map<Price, std::list<OrderId>, std::less<Price>> ask_levels_;
};

}  // namespace lob::opgen
