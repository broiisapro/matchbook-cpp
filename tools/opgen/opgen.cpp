#include "tools/opgen/opgen.hpp"

#include <algorithm>
#include <limits>

namespace lob::opgen {
namespace {

constexpr std::size_t kMaxReusableIds = 1024;

/// Boundary offsets, in ticks above min_price, that the ladder-stress preset
/// clusters around: 63/64/65 straddle the first occupancy word boundary and
/// 4095/4096/4097 the first summary word boundary. Entry 0 is the bottom and
/// the last entry is the top of the configured range (filled in per call).
constexpr std::size_t kLadderBoundaryCount = 8;
constexpr std::uint64_t kLadderBoundaryOffsets[kLadderBoundaryCount] = {0,    63,   64,   65,
                                                                        4095, 4096, 4097, 0};

/// Sweep size: large enough to empty a differential test's whole side in one
/// market order, so the next best level is far away.
constexpr Qty kSweepBaseQty = 100000;
constexpr std::uint64_t kSweepExtraQty = 100000;

}  // namespace

std::string_view to_string(OpKind kind) {
  switch (kind) {
    case OpKind::Submit:
      return "submit";
    case OpKind::Cancel:
      return "cancel";
    case OpKind::Modify:
      return "modify";
  }
  return "unknown";
}

std::string_view to_string(OpMixPreset preset) {
  switch (preset) {
    case OpMixPreset::Default:
      return "default";
    case OpMixPreset::LadderStress:
      return "ladder_stress";
    case OpMixPreset::Wide:
      return "wide";
  }
  return "default";
}

OpMix wide_mix() {
  OpMix mix;
  mix.preset = OpMixPreset::Wide;
  mix.mid_half_width = 300;
  return mix;
}

OpMix ladder_stress_mix() {
  OpMix mix;
  mix.preset = OpMixPreset::LadderStress;
  mix.submit_weight = 55;
  mix.cancel_weight = 15;
  mix.modify_weight = 10;
  mix.sweep_weight = 12;
  mix.drain_refill_weight = 20;
  // Sweeps are the deliberate market orders in this preset.
  mix.market_weight = 0;
  mix.ioc_weight = 4;
  // Boundary clusters cross each other on their own, and min_price and
  // max_price come from the boundary set, so the default price modes are off.
  mix.crossing_weight = 0;
  mix.edge_price_weight = 0;
  mix.invalid_qty_weight = 2;
  mix.duplicate_id_weight = 3;
  mix.reuse_id_weight = 5;
  mix.unknown_id_weight = 5;
  return mix;
}

std::ostream& operator<<(std::ostream& out, OpKind kind) { return out << to_string(kind); }

std::ostream& operator<<(std::ostream& out, const Op& op) {
  out << "Op{" << op.kind << ", id=" << op.id << ", side=" << lob::to_string(op.side)
      << ", type=" << lob::to_string(op.type) << ", price=" << op.price << ", qty=" << op.qty
      << "}";
  return out;
}

OpGenerator::OpGenerator(std::uint64_t seed, Config config, OpMix mix)
    : rng_(seed),
      config_(config),
      mix_(mix),
      mid_(config.min_price + (config.max_price - config.min_price) / 2) {}

std::uint64_t OpGenerator::below(std::uint64_t bound) {
  if (bound <= 1) {
    return 0;
  }
  constexpr std::uint64_t kMax = std::numeric_limits<std::uint64_t>::max();
  const std::uint64_t limit = kMax - (kMax % bound);
  std::uint64_t value = rng_();
  while (value >= limit) {
    value = rng_();
  }
  return value % bound;
}

bool OpGenerator::chance(std::uint32_t percent) {
  if (percent == 0) {
    return false;
  }
  if (percent >= 100) {
    return true;
  }
  return below(100) < percent;
}

Price OpGenerator::uniform_price(Price lo, Price hi) {
  return lo + static_cast<Price>(below(static_cast<std::uint64_t>(hi - lo + 1)));
}

Op OpGenerator::next() {
  if (mix_.preset == OpMixPreset::LadderStress) {
    return next_ladder_stress();
  }

  if (config_.min_price < config_.max_price) {
    const auto step = static_cast<Price>(below(5));
    mid_ = std::clamp<Price>(mid_ + step - 2, config_.min_price, config_.max_price);
  }

  const std::uint32_t total = mix_.submit_weight + mix_.cancel_weight + mix_.modify_weight;
  if (total == 0) {
    return next_submit();
  }
  const auto pick = static_cast<std::uint32_t>(below(total));
  if (pick < mix_.submit_weight) {
    return next_submit();
  }
  if (pick < mix_.submit_weight + mix_.cancel_weight) {
    return next_cancel();
  }
  return next_modify();
}

Op OpGenerator::next_ladder_stress() {
  const std::uint32_t total = mix_.submit_weight + mix_.cancel_weight + mix_.modify_weight +
                              mix_.sweep_weight + mix_.drain_refill_weight;
  if (total == 0) {
    return next_submit();
  }
  auto roll = static_cast<std::uint32_t>(below(total));
  if (roll < mix_.submit_weight) {
    return next_submit();
  }
  roll -= mix_.submit_weight;
  if (roll < mix_.cancel_weight) {
    return next_cancel();
  }
  roll -= mix_.cancel_weight;
  if (roll < mix_.modify_weight) {
    return next_modify();
  }
  roll -= mix_.modify_weight;
  if (roll < mix_.sweep_weight) {
    return next_sweep();
  }
  return next_drain_refill_step();
}

Op OpGenerator::next_sweep() {
  if (!sweep_side_set_) {
    sweep_side_ = below(2) == 0 ? Side::Buy : Side::Sell;
    sweep_side_set_ = true;
  } else if (chance(35)) {
    sweep_side_ = sweep_side_ == Side::Buy ? Side::Sell : Side::Buy;
  }
  const OrderId id = next_id_++;
  const Qty qty = kSweepBaseQty + static_cast<Qty>(below(kSweepExtraQty));
  return Op{OpKind::Submit, id, sweep_side_, OrderType::Market, 0, qty};
}

Op OpGenerator::next_drain_refill_step() {
  if (ladder_cycle_step_ % 2 == 0) {
    const Side side = ladder_cycle_step_ == 0 ? Side::Buy : Side::Sell;
    const Price price = drain_refill_price(side);
    const OrderId id = next_id_++;
    const Qty qty = static_cast<Qty>(1 + below(100));
    model_add(side, price, id, qty);
    ladder_cycle_id_ = id;
    ladder_cycle_side_ = side;
    ++ladder_cycle_step_;
    return Op{OpKind::Submit, id, side, OrderType::Limit, price, qty};
  }

  const OrderId id = ladder_cycle_id_;
  const Side side = ladder_cycle_side_;
  if (model_.find(id) != model_.end()) {
    model_remove(id, true);
  }
  ++ladder_cycle_step_;
  if (ladder_cycle_step_ == 4) {
    ladder_cycle_step_ = 0;
    ladder_offset_ = static_cast<std::uint32_t>((ladder_offset_ + 1) % kLadderBoundaryCount);
  }
  return Op{OpKind::Cancel, id, side, OrderType::Limit, 0, 0};
}

Op OpGenerator::next_submit() {
  const Side side = below(2) == 0 ? Side::Buy : Side::Sell;

  OrderType type = OrderType::Limit;
  if (chance(mix_.market_weight)) {
    type = OrderType::Market;
  } else if (chance(mix_.ioc_weight)) {
    type = OrderType::IOC;
  }

  OrderId id = 0;
  bool duplicate = false;
  if (chance(mix_.duplicate_id_weight) && !live_ids_.empty()) {
    id = live_ids_[static_cast<std::size_t>(below(live_ids_.size()))];
    duplicate = true;
  } else if (chance(mix_.reuse_id_weight) && !reusable_ids_.empty()) {
    const auto index = static_cast<std::size_t>(below(reusable_ids_.size()));
    id = reusable_ids_[index];
    reusable_ids_[index] = reusable_ids_.back();
    reusable_ids_.pop_back();
  } else {
    id = next_id_++;
  }

  const Qty qty =
      chance(mix_.invalid_qty_weight) ? choose_invalid_qty() : static_cast<Qty>(1 + below(100));
  const Price price = type == OrderType::Market ? 0 : choose_submit_price(side);

  const bool would_validate = type == OrderType::Limit && !duplicate && qty > 0 &&
                              price >= config_.min_price && price <= config_.max_price;
  if (would_validate && model_.find(id) == model_.end()) {
    model_add(side, price, id, qty);
  }
  return Op{OpKind::Submit, id, side, type, price, qty};
}

Op OpGenerator::next_cancel() {
  const Target target = pick_target();
  Side side = Side::Buy;
  if (target.known) {
    side = model_.find(target.id)->second.side;
    model_remove(target.id, true);
  }
  return Op{OpKind::Cancel, target.id, side, OrderType::Limit, 0, 0};
}

Op OpGenerator::next_modify() {
  const Target target = pick_target();
  if (!target.known) {
    const Qty qty =
        chance(mix_.invalid_qty_weight) ? choose_invalid_qty() : static_cast<Qty>(1 + below(100));
    const Price price = choose_submit_price(below(2) == 0 ? Side::Buy : Side::Sell);
    return Op{OpKind::Modify, target.id, Side::Buy, OrderType::Limit, price, qty};
  }

  const ModelLocation location = model_.find(target.id)->second;
  const std::uint32_t total = mix_.no_change_weight + mix_.reduce_weight + mix_.increase_weight +
                              mix_.move_weight + mix_.invalid_price_weight;
  std::uint32_t roll = total == 0 ? 0 : static_cast<std::uint32_t>(below(total));
  const auto take = [&roll](std::uint32_t weight) {
    if (roll < weight) {
      return true;
    }
    roll -= weight;
    return false;
  };

  if (take(mix_.no_change_weight)) {
    return Op{OpKind::Modify,   target.id,      location.side,
              OrderType::Limit, location.price, location.qty};
  }
  if (take(mix_.reduce_weight)) {
    const Qty qty = location.qty > 1
                        ? static_cast<Qty>(1 + below(static_cast<std::uint64_t>(location.qty - 1)))
                        : location.qty;
    model_update_qty(target.id, qty);
    return Op{OpKind::Modify, target.id, location.side, OrderType::Limit, location.price, qty};
  }
  if (take(mix_.increase_weight)) {
    const Qty qty = location.qty + static_cast<Qty>(1 + below(50));
    model_update_qty(target.id, qty);
    return Op{OpKind::Modify, target.id, location.side, OrderType::Limit, location.price, qty};
  }
  if (take(mix_.move_weight)) {
    const Price price =
        mix_.preset == OpMixPreset::LadderStress
            ? ladder_boundary_price(static_cast<std::uint32_t>(below(kLadderBoundaryCount)))
            : choose_move_price(location.price);
    const Qty qty = static_cast<Qty>(1 + below(100));
    model_remove(target.id, false);
    model_add(location.side, price, target.id, qty);
    return Op{OpKind::Modify, target.id, location.side, OrderType::Limit, price, qty};
  }

  const Price price = below(2) == 0 ? config_.min_price - 1 : config_.max_price + 1;
  const Qty qty = static_cast<Qty>(1 + below(100));
  return Op{OpKind::Modify, target.id, location.side, OrderType::Limit, price, qty};
}

Price OpGenerator::choose_submit_price(Side side) {
  if (mix_.preset == OpMixPreset::LadderStress) {
    return ladder_boundary_price(static_cast<std::uint32_t>(below(kLadderBoundaryCount)));
  }
  if (chance(mix_.crossing_weight)) {
    if (side == Side::Buy && !ask_levels_.empty()) {
      return ask_levels_.begin()->first;
    }
    if (side == Side::Sell && !bid_levels_.empty()) {
      return bid_levels_.begin()->first;
    }
  }
  if (chance(mix_.edge_price_weight)) {
    return choose_edge_price();
  }
  const auto band = static_cast<Price>(mix_.mid_half_width);
  const Price lo = std::max(mid_ - band, config_.min_price - 2);
  const Price hi = std::min(mid_ + band, config_.max_price + 2);
  return uniform_price(lo, hi);
}

Price OpGenerator::choose_move_price(Price old_price) {
  const auto band = static_cast<Price>(mix_.mid_half_width);
  const Price lo = std::max(mid_ - band, config_.min_price);
  const Price hi = std::min(mid_ + band, config_.max_price);
  Price price = lo <= hi ? uniform_price(lo, hi) : mid_;
  if (price == old_price) {
    if (price < config_.max_price) {
      price += 1;
    } else if (price > config_.min_price) {
      price -= 1;
    }
  }
  return price;
}

Price OpGenerator::choose_edge_price() {
  const Price options[6] = {config_.min_price,     config_.max_price,     config_.min_price - 1,
                            config_.max_price + 1, config_.min_price - 2, config_.max_price + 2};
  return options[static_cast<std::size_t>(below(6))];
}

Price OpGenerator::ladder_boundary_price(std::uint32_t index) {
  const std::uint64_t span = static_cast<std::uint64_t>(config_.max_price - config_.min_price);
  const std::size_t slot = static_cast<std::size_t>(index) % kLadderBoundaryCount;
  std::uint64_t offset = kLadderBoundaryOffsets[slot];
  if (slot == kLadderBoundaryCount - 1) {
    offset = span;
  }
  offset = std::min(offset, span);
  const std::int64_t jitter = static_cast<std::int64_t>(below(5)) - 2;
  const std::int64_t price = config_.min_price + static_cast<std::int64_t>(offset) + jitter;
  return std::clamp<Price>(price, config_.min_price, config_.max_price);
}

Price OpGenerator::drain_refill_price(Side side) {
  // Join the modelled best level so the add/cancel pair targets the top of the
  // book. Those prices were themselves drawn from the boundary clusters, and
  // when the side is empty the fallback boundary price becomes the best level.
  if (side == Side::Buy && !bid_levels_.empty()) {
    return bid_levels_.begin()->first;
  }
  if (side == Side::Sell && !ask_levels_.empty()) {
    return ask_levels_.begin()->first;
  }
  return ladder_boundary_price(ladder_offset_);
}

Qty OpGenerator::choose_invalid_qty() {
  if (below(3) == 0) {
    return 0;
  }
  return -static_cast<Qty>(1 + below(1000));
}

OpGenerator::Target OpGenerator::pick_target() {
  if (live_ids_.empty() || chance(mix_.unknown_id_weight)) {
    return Target{next_unknown_id_++, false};
  }

  const auto pick = static_cast<std::uint32_t>(below(100));
  if (pick < 25) {
    const OrderId id = pick_head_at_best();
    if (id != 0) {
      return Target{id, true};
    }
  } else if (pick < 50) {
    const OrderId id = pick_tail_at_best();
    if (id != 0) {
      return Target{id, true};
    }
  }
  return Target{live_ids_[static_cast<std::size_t>(below(live_ids_.size()))], true};
}

OrderId OpGenerator::pick_head_at_best() {
  if (below(2) == 0) {
    if (bid_levels_.empty()) {
      return 0;
    }
    return bid_levels_.begin()->second.front();
  }
  if (ask_levels_.empty()) {
    return 0;
  }
  return ask_levels_.begin()->second.front();
}

OrderId OpGenerator::pick_tail_at_best() {
  if (below(2) == 0) {
    if (bid_levels_.empty()) {
      return 0;
    }
    return bid_levels_.begin()->second.back();
  }
  if (ask_levels_.empty()) {
    return 0;
  }
  return ask_levels_.begin()->second.back();
}

void OpGenerator::model_add(Side side, Price price, OrderId id, Qty qty) {
  std::list<OrderId>::iterator order;
  if (side == Side::Buy) {
    auto& queue = bid_levels_[price];
    queue.push_back(id);
    order = std::prev(queue.end());
  } else {
    auto& queue = ask_levels_[price];
    queue.push_back(id);
    order = std::prev(queue.end());
  }
  model_.emplace(id, ModelLocation{side, price, qty, order});
  live_index_.emplace(id, live_ids_.size());
  live_ids_.push_back(id);
}

void OpGenerator::model_remove(OrderId id, bool reusable) {
  const auto found = model_.find(id);
  if (found == model_.end()) {
    return;
  }
  const ModelLocation location = found->second;
  if (location.side == Side::Buy) {
    const auto level = bid_levels_.find(location.price);
    level->second.erase(location.order);
    if (level->second.empty()) {
      bid_levels_.erase(level);
    }
  } else {
    const auto level = ask_levels_.find(location.price);
    level->second.erase(location.order);
    if (level->second.empty()) {
      ask_levels_.erase(level);
    }
  }
  model_.erase(found);

  const auto index = live_index_.find(id);
  const OrderId moved = live_ids_.back();
  live_ids_[index->second] = moved;
  live_index_[moved] = index->second;
  live_ids_.pop_back();
  live_index_.erase(index);

  if (reusable) {
    reusable_ids_.push_back(id);
    if (reusable_ids_.size() > kMaxReusableIds) {
      reusable_ids_.erase(reusable_ids_.begin());
    }
  }
}

void OpGenerator::model_update_qty(OrderId id, Qty qty) {
  const auto found = model_.find(id);
  if (found != model_.end()) {
    found->second.qty = qty;
  }
}

}  // namespace lob::opgen
