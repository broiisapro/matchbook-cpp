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

/// Named operation-mix presets. `Default` is the general mixed workload;
/// `LadderStress` redirects price selection to the price ladder's occupancy
/// boundaries and enables the sweep and drain/refill weights; `Wide` is the
/// default workload with a much wider mid band (see wide_mix).
enum class OpMixPreset : std::uint8_t { Default = 0, LadderStress = 1, Wide = 2 };

std::string_view to_string(OpMixPreset preset);

/// Relative weights for the generator. Operation-kind weights are relative to
/// each other; the attribute fields are thresholds on [0, 100) and a value of
/// zero disables that attribute. The generator never uses floating point, so
/// the same seed and mix always produce the same stream on every standard
/// library.
struct OpMix {
  // Preset. LadderStress switches price selection to the occupancy bitmap
  // boundaries and enables sweep_weight and drain_refill_weight below.
  OpMixPreset preset = OpMixPreset::Default;
  // Half-width, in ticks, of the band around the mid that default-mode
  // submits and moves draw from. The same value is used for both sides.
  std::uint32_t mid_half_width = 10;
  // Operation kind (relative weights).
  std::uint32_t submit_weight = 70;
  std::uint32_t cancel_weight = 20;
  std::uint32_t modify_weight = 10;
  // Submit shape (percent).
  std::uint32_t market_weight = 6;
  std::uint32_t ioc_weight = 10;
  // Phase 2 order types. Zero in every default preset: CI must never compare a
  // FOK-implementing engine against a FOK-rejecting one (decision C53), so the
  // types only appear through all_types_mix() / `--types all`.
  std::uint32_t fok_weight = 0;
  std::uint32_t post_only_weight = 0;
  // Share (percent) of PostOnly submits deliberately priced through the
  // opposite best level, so an all-types run covers the WouldCross reject path
  // as well as the rest path. Ignored when post_only_weight is zero.
  std::uint32_t post_only_cross_weight = 0;
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
  // LadderStress-only operation weights (relative to the kind weights).
  std::uint32_t sweep_weight = 12;
  std::uint32_t drain_refill_weight = 20;
};

/// Preset aimed at the array price ladder's failure modes: prices clustered at
/// the occupancy bitmap word and summary boundaries (offsets 0, 63, 64, 65,
/// 4095, 4096, 4097 and the top of the range, all relative to min_price, so
/// min_price and max_price are included), long one-sided market sweeps that
/// empty the best level while the next occupied level is far away, and
/// repeated drain/refill cycles that create and remove the best level on
/// alternating sides.
OpMix ladder_stress_mix();

/// Preset for the wide differential configuration: the default workload with
/// a mid band of 300 ticks instead of 10, so a 200000-tick range grows books
/// hundreds of price levels deep when the capacity allows.
OpMix wide_mix();

/// Turns the Phase 2 order types on for an all-types run: about 5 % of submits
/// become FOK and about 5 % PostOnly, and half of the PostOnly orders are
/// deliberately priced through the opposite best level so both the WouldCross
/// reject and the resting path are exercised. Everything else in `mix` is
/// preserved, so it composes with each preset (`all_types_mix(wide_mix())`).
/// The default mixes leave both weights at zero (decision C53).
OpMix all_types_mix(OpMix mix = {});

/// Deterministic operation stream. The generator keeps its own view of the
/// orders it has submitted (as submitted, not as filled) so cancels and
/// modifies usually target ids that are still live in the engine: the head and
/// tail of the queue at the best modelled level, or a random live id. It also
/// produces unknown ids, duplicate live ids, ids reused after a cancel,
/// crossing orders, prices at the configured edges and just outside, invalid
/// quantities, market/IOC orders against possibly empty sides, and (only when
/// the mix enables them, i.e. all_types_mix) FOK and PostOnly orders. A mid
/// price random walk keeps the modelled book deep.
///
/// FOK and PostOnly submits never enter the modelled book: the model is a
/// heuristic for aiming cancels and modifies at live ids, and treating the
/// phase 2 types as non-resting keeps it simple and honest.
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

  /// LadderStress path: weighted choice over the kind weights plus the sweep
  /// and drain/refill weights.
  Op next_ladder_stress();
  /// One long one-sided sweep: a large market order on the persistent sweep
  /// side that walks through every level of the opposite side.
  Op next_sweep();
  /// One step of the drain/refill cycle: submit then cancel, buy then sell.
  Op next_drain_refill_step();

  Price choose_submit_price(Side side);
  /// The opposite side's best modelled price when it exists, else `fallback`.
  /// Used to price a PostOnly order deliberately through the book.
  Price crossing_price(Side side, Price fallback);
  Price choose_move_price(Price old_price);
  Price choose_edge_price();
  /// Price at boundary `index` of the ladder's occupancy bitmaps, relative to
  /// min_price: word ends 63/64/65, summary-word ends 4095/4096/4097, the
  /// bottom (0) and the top of the configured range. A small jitter keeps a
  /// few ticks around each boundary.
  Price ladder_boundary_price(std::uint32_t index);
  /// Price for a drain/refill refill: the modelled best level on `side`, or a
  /// boundary price when the modelled side is empty.
  Price drain_refill_price(Side side);
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

  // LadderStress state: the sweep side persists across a run of sweeps, and
  // the drain/refill cycle walks the boundary offsets one submit/cancel pair
  // at a time.
  Side sweep_side_ = Side::Buy;
  bool sweep_side_set_ = false;
  std::uint32_t ladder_offset_ = 0;
  std::uint32_t ladder_cycle_step_ = 0;
  OrderId ladder_cycle_id_ = 0;
  Side ladder_cycle_side_ = Side::Buy;

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
