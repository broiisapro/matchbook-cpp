#include <benchmark/benchmark.h>

#include <cstddef>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

#include "bench/latency.hpp"
#include "bench/support.hpp"
#include "lob/listener.hpp"
#include "lob/order_book.hpp"
#include "lob/types.hpp"

namespace lob::bench {
namespace {

using lob::OrderBook;

/// Maker quantity for levels that must stay alive for a whole timed run: the
/// backstop third level of the crossing fixture (only ever partially filled)
/// and the deep bid/ask of the mixed fixture.
constexpr Qty kDeepQty = 1'000'000'000'000LL;

constexpr Price kAskBase = 1001;  // crossing fixtures use 1001, 1002, 1003
constexpr Price kHugeBid = 999;   // market sells consume this level
constexpr Qty kTakeQty = 10;      // per level for crossing orders
constexpr int kAskLevels = 3;     // ask levels in the crossing fixture

/// Small makers per swept level in the timed crossing fixture. A take consumes
/// exactly one maker per visited level; the pools refill (paused) before they
/// run out, so every timed take sweeps the advertised number of levels.
constexpr std::size_t kMakerPool = 4096;

/// Empty when the condition holds, the problem text otherwise; returning a
/// string lets a failed check say which invariant broke.
std::string describe(bool condition, const std::string& problem) {
  return condition ? std::string() : problem;
}

/// A limit buy below every ask must rest without trading.
std::string sanity_add_non_crossing() {
  const Config config = bench_config();
  CountingListener listener;
  OrderBook book(config, listener);
  book.submit(1, Side::Sell, OrderType::Limit, 9999, kDeepQty);  // ask far above
  listener.reset();
  book.submit(2, Side::Buy, OrderType::Limit, config.min_price, 1);
  if (const std::string problem =
          describe(listener.accepts == 1 && listener.trades == 0 && listener.rejects == 0 &&
                       listener.updates == 1 && listener.cancels == 0 && book.order_count() == 2 &&
                       book.best_bid() == config.min_price,
                   "a non-crossing limit buy must rest without trading");
      !problem.empty()) {
    return problem;
  }
  book.cancel(2);
  return describe(listener.cancels == 1 && book.order_count() == 1,
                  "cancelling the resting order must remove it");
}

/// A buy of `levels * kTakeQty` lots at the last swept price must trade one
/// maker per level, best first, at the resting price, and never rest itself.
/// Levels below the last held a single maker each, so sweeping consumes them;
/// the last level holds a deep maker and survives the partial fill.
std::string sanity_crossing(int levels, bool market) {
  const Config config = bench_config();
  CountingListener listener;
  OrderBook book(config, listener);
  OrderId next_id = 1;
  for (int level = 0; level + 1 < kAskLevels; ++level) {
    book.submit(next_id++, Side::Sell, OrderType::Limit, kAskBase + level, kTakeQty);
  }
  book.submit(next_id++, Side::Sell, OrderType::Limit, kAskBase + kAskLevels - 1, kDeepQty);
  listener.reset();

  book.submit(next_id++, Side::Buy, market ? OrderType::Market : OrderType::Limit,
              market ? 0 : kAskBase + levels - 1, kTakeQty * levels);
  const auto expected = static_cast<std::size_t>(levels);
  if (const std::string problem =
          describe(listener.accepts == 1 && listener.rejects == 0 && listener.trades == expected &&
                       listener.trade_log.size() == expected && listener.cancels == 0,
                   "the crossing order must trade exactly one maker per level");
      !problem.empty()) {
    return problem;
  }
  for (int level = 0; level < levels; ++level) {
    const CountingListener::Trade& trade = listener.trade_log[static_cast<std::size_t>(level)];
    if (const std::string problem =
            describe(trade.maker_id == static_cast<OrderId>(level + 1) &&
                         trade.price == kAskBase + level && trade.qty == kTakeQty,
                     "trades must walk the levels best first at the resting price");
        !problem.empty()) {
      return problem;
    }
  }
  return describe(book.best_ask() == kAskBase + (levels == 1 ? 1 : 2),
                  "the fully filled taker must not rest and the deeper asks must survive");
}

/// A resting order must leave exactly one cancel event and no trades.
std::string sanity_cancel() {
  const Config config = bench_config();
  CountingListener listener;
  OrderBook book(config, listener);
  book.submit(1, Side::Buy, OrderType::Limit, config.min_price, 5);
  listener.reset();
  book.cancel(1);
  return describe(listener.cancels == 1 && listener.accepts == 0 && listener.trades == 0 &&
                      listener.rejects == 0 && listener.updates == 1 &&
                      book.level_qty(Side::Buy, config.min_price) == 0 && book.order_count() == 0,
                  "cancelling a resting order must emit one cancel and empty the level");
}

/// Maker levels used by the crossing and market benchmarks. Levels 1001 and
/// 1002 hold pools of small makers whose heads every take consumes; level 1003
/// holds one deep maker that survives partial fills. The pools are topped up
/// outside the timed region before they can run dry, so every timed take walks
/// exactly `levels` distinct price levels.
class CrossingFixture {
 public:
  CrossingFixture(Listener& listener, int levels, bool market)
      : levels_(levels), market_(market), book_(bench_config(), listener) {
    for (int level = 0; level < swept_levels(); ++level) {
      add_pool(level);
    }
    book_.submit(next_id_++, Side::Sell, OrderType::Limit, kAskBase + kAskLevels - 1, kDeepQty);
  }

  void take() {
    book_.submit(next_id_++, Side::Buy, market_ ? OrderType::Market : OrderType::Limit,
                 market_ ? 0 : kAskBase + levels_ - 1, kTakeQty * levels_);
    --remaining_;
  }

  /// True when the head of a swept pool may run out inside the next record_op
  /// call (which performs up to kBatchSize takes in batch16 mode).
  bool needs_refill() const { return remaining_ < kBatchSize; }

  void refill() {
    for (int level = 0; level < swept_levels(); ++level) {
      add_pool(level);
    }
    remaining_ += kMakerPool;
  }

 private:
  int levels_;
  bool market_;
  OrderBook book_;
  OrderId next_id_ = 1;
  std::size_t remaining_ = kMakerPool;

  /// Levels 1001 and 1002 hold pools; the last level is the deep maker.
  int swept_levels() const { return levels_ < kAskLevels - 1 ? levels_ : kAskLevels - 1; }

  void add_pool(int level) {
    for (std::size_t i = 0; i < kMakerPool; ++i) {
      book_.submit(next_id_++, Side::Sell, OrderType::Limit, kAskBase + level, kTakeQty);
    }
  }
};

/// Non-crossing adds at the price floor. The book is drained outside the timed
/// region once the live count approaches max_orders, so every measured add
/// rests and no add is ever capacity-cancelled.
class AddOrderFixture {
 public:
  explicit AddOrderFixture(Listener& listener) : book_(bench_config(), listener) {
    book_.submit(next_id_++, Side::Sell, OrderType::Limit, 9999, kDeepQty);
    add();  // start with one resting order so the level exists
  }

  void add() {
    const OrderId id = next_id_++;
    book_.submit(id, Side::Buy, OrderType::Limit, bench_config().min_price, 1);
    live_.push_back(id);
  }

  bool needs_drain() const { return live_.size() + kHeadroom >= bench_config().max_orders; }

  void drain() {
    for (const OrderId id : live_) {
      book_.cancel(id);
    }
    live_.clear();
  }

 private:
  static constexpr std::size_t kHeadroom = 1024;
  OrderBook book_;
  OrderId next_id_ = 1;
  std::vector<OrderId> live_;
};

/// A pool of resting orders cancelled one at a time. The pool is rebuilt
/// outside the timed region; the headroom covers a whole batch16 batch in one
/// record_op call.
class CancelFixture {
 public:
  explicit CancelFixture(Listener& listener) : book_(bench_config(), listener) { refill(); }

  void refill() {
    ids_.clear();
    index_ = 0;
    for (std::size_t i = 0; i < kResting; ++i) {
      const OrderId id = next_id_++;
      book_.submit(id, Side::Buy, OrderType::Limit, bench_config().min_price, 1);
      ids_.push_back(id);
    }
  }

  bool needs_refill() const { return index_ + kBatchSize > ids_.size(); }

  void cancel_one() { book_.cancel(ids_[index_++]); }

 private:
  static constexpr std::size_t kResting = 65'536;
  OrderBook book_;
  OrderId next_id_ = 1;
  std::vector<OrderId> ids_;
  std::size_t index_ = 0;
};

/// A seeded mix: non-crossing adds (40 %), cancels of live add orders (25 %),
/// crossing limits into a deep ask (20 %), market sells into a deep bid
/// (15 %). The fixed seed makes every run and repetition exercise the same mix.
class MixedFlowFixture {
 public:
  explicit MixedFlowFixture(Listener& listener)
      : book_(bench_config(), listener), rng_(20261017ULL) {
    book_.submit(next_id_++, Side::Buy, OrderType::Limit, kHugeBid, kDeepQty);
    book_.submit(next_id_++, Side::Sell, OrderType::Limit, kAskBase, kDeepQty);
  }

  void step() {
    const std::uint64_t roll = rng_() % 100;
    if (roll < 40) {
      add();
    } else if (roll < 65 && !live_.empty()) {
      const std::size_t index = static_cast<std::size_t>(rng_() % live_.size());
      const OrderId id = live_[index];
      live_[index] = live_.back();
      live_.pop_back();
      book_.cancel(id);
    } else if (roll < 85) {
      book_.submit(next_id_++, Side::Buy, OrderType::Limit, kAskBase, 5);
    } else {
      book_.submit(next_id_++, Side::Sell, OrderType::Market, 0, 5);
    }
  }

  bool needs_drain() const { return live_.size() + kHeadroom >= bench_config().max_orders; }

  void drain() {
    for (const OrderId id : live_) {
      book_.cancel(id);
    }
    live_.clear();
  }

  std::size_t order_count() const { return book_.order_count(); }

 private:
  static constexpr std::size_t kHeadroom = 1024;
  OrderBook book_;
  OrderId next_id_ = 1;
  std::mt19937_64 rng_;
  std::vector<OrderId> live_;

  void add() {
    const OrderId id = next_id_++;
    book_.submit(id, Side::Buy, OrderType::Limit, bench_config().min_price, 1);
    live_.push_back(id);
  }
};

/// Runs 1000 mixed steps on a fresh book and checks that every action type
/// happened and no order was rejected.
std::string sanity_mixed_flow() {
  CountingListener listener;
  MixedFlowFixture fixture(listener);
  for (int i = 0; i < 1000; ++i) {
    fixture.step();
  }
  return describe(listener.accepts > 0 && listener.trades > 0 && listener.cancels > 0 &&
                      listener.rejects == 0 && fixture.order_count() > 3,
                  "the mixed flow must rest, trade and cancel orders");
}

/// Adds the latency percentiles and the mode flag to the benchmark result.
void report_latency(benchmark::State& state, const LatencyRecorder& recorder) {
  const Percentiles p = recorder.percentiles();
  state.counters["p50_ns"] = static_cast<double>(p.p50_ns);
  state.counters["p90_ns"] = static_cast<double>(p.p90_ns);
  state.counters["p99_ns"] = static_cast<double>(p.p99_ns);
  state.counters["p999_ns"] = static_cast<double>(p.p999_ns);
  state.counters["max_ns"] = static_cast<double>(p.max_ns);
  state.counters["mode"] = recorder.mode() == LatencyMode::Batch16 ? 1.0 : 0.0;
  state.SetItemsProcessed(static_cast<std::int64_t>(recorder.sample_count()) *
                          recorder.ops_per_sample());
}

/// Common front matter: skip what the engine cannot do yet, fail loudly when a
/// sanity check does not hold.
bool setup_or_skip(benchmark::State& state, bool needs_matching,
                   const std::string& sanity_problem) {
  const EngineState engine = engine_state();
  if (engine == EngineState::Stub) {
    state.SkipWithMessage("engine not implemented");
    return false;
  }
  if (needs_matching && engine != EngineState::Ready) {
    state.SkipWithMessage("engine not implemented: matching");
    return false;
  }
  if (!sanity_problem.empty()) {
    note_harness_failure();
    state.SkipWithError("sanity check failed: " + sanity_problem);
    return false;
  }
  return true;
}

void AddOrder_NonCrossing(benchmark::State& state) {
  if (!setup_or_skip(state, /*needs_matching=*/false, sanity_add_non_crossing())) {
    return;
  }
  NullListener listener;
  AddOrderFixture fixture(listener);
  LatencyRecorder recorder;
  for (auto _ : state) {
    if (fixture.needs_drain()) {
      state.PauseTiming();
      fixture.drain();
      state.ResumeTiming();
    }
    recorder.record_op([&fixture] { fixture.add(); });
  }
  report_latency(state, recorder);
}

void crossing(benchmark::State& state, bool market) {
  const int levels = static_cast<int>(state.range(0));
  if (!setup_or_skip(state, /*needs_matching=*/true, sanity_crossing(levels, market))) {
    return;
  }
  NullListener listener;
  CrossingFixture fixture(listener, levels, market);
  LatencyRecorder recorder;
  for (auto _ : state) {
    if (fixture.needs_refill()) {
      state.PauseTiming();
      fixture.refill();
      state.ResumeTiming();
    }
    recorder.record_op([&fixture] { fixture.take(); });
  }
  report_latency(state, recorder);
}

void AddOrder_Crossing(benchmark::State& state) { crossing(state, false); }

void Market_Sweep(benchmark::State& state) { crossing(state, true); }

void Cancel_Resting(benchmark::State& state) {
  if (!setup_or_skip(state, /*needs_matching=*/false, sanity_cancel())) {
    return;
  }
  NullListener listener;
  CancelFixture fixture(listener);
  LatencyRecorder recorder;
  for (auto _ : state) {
    if (fixture.needs_refill()) {
      state.PauseTiming();
      fixture.refill();
      state.ResumeTiming();
    }
    recorder.record_op([&fixture] { fixture.cancel_one(); });
  }
  report_latency(state, recorder);
}

void MixedFlow(benchmark::State& state) {
  if (!setup_or_skip(state, /*needs_matching=*/true, sanity_mixed_flow())) {
    return;
  }
  NullListener listener;
  MixedFlowFixture fixture(listener);
  LatencyRecorder recorder;
  for (auto _ : state) {
    if (fixture.needs_drain()) {
      state.PauseTiming();
      fixture.drain();
      state.ResumeTiming();
    }
    recorder.record_op([&fixture] { fixture.step(); });
  }
  report_latency(state, recorder);
}

BENCHMARK(AddOrder_NonCrossing)->Unit(benchmark::kNanosecond);
BENCHMARK(AddOrder_Crossing)->Arg(1)->Arg(2)->Arg(3)->Unit(benchmark::kNanosecond);
BENCHMARK(Cancel_Resting)->Unit(benchmark::kNanosecond);
BENCHMARK(Market_Sweep)->Arg(1)->Arg(2)->Arg(3)->Unit(benchmark::kNanosecond);
BENCHMARK(MixedFlow)->Unit(benchmark::kNanosecond);

}  // namespace
}  // namespace lob::bench
