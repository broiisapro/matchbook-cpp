#pragma once

#include <cstddef>
#include <vector>

#include "lob/config.hpp"
#include "lob/listener.hpp"
#include "lob/types.hpp"

namespace lob::bench {

/// Configuration every suite runs with. The deep maker quantities keep levels
/// alive across a whole timed run, so fixtures rarely need a refill and the
/// pool has room for the widest fixture (the cancel pool and the non-crossing
/// add drain threshold sit well inside max_orders).
inline Config bench_config() {
  Config config;
  config.min_price = 1;
  config.max_price = 100'000;
  config.max_orders = 1U << 18U;  // 262144
  config.max_levels = 64;
  return config;
}

/// Listener for setup-time sanity checks: counts every callback and logs
/// trades. Timed loops use NullListener instead.
class CountingListener final : public Listener {
 public:
  struct Trade {
    OrderId maker_id;
    OrderId taker_id;
    Price price;
    Qty qty;
  };

  void on_accept(OrderId) override { ++accepts; }
  void on_reject(OrderId, RejectReason) override { ++rejects; }
  void on_trade(OrderId maker_id, OrderId taker_id, Price price, Qty qty) override {
    ++trades;
    trade_log.push_back(Trade{maker_id, taker_id, price, qty});
  }
  void on_cancel(OrderId, Qty) override { ++cancels; }
  void on_modify(OrderId, Price, Qty) override { ++modifies; }
  void on_book_update(Side, Price, Qty) override { ++updates; }

  void reset() {
    accepts = rejects = trades = cancels = modifies = updates = 0;
    trade_log.clear();
  }

  std::size_t accepts = 0;
  std::size_t rejects = 0;
  std::size_t trades = 0;
  std::size_t cancels = 0;
  std::size_t modifies = 0;
  std::size_t updates = 0;
  std::vector<Trade> trade_log;
};

/// True when the engine under test actually keeps state: a limit buy at the
/// price floor must rest. The Epic 0 stubs accept the call but stay empty.
/// The probe runs once per process and is cached.
bool engine_implemented();

/// Number of sanity checks that failed so far across all benchmarks.
int harness_failures();

/// Records a failed sanity check; bench_main exits nonzero afterwards so a
/// benchmark that silently stopped exercising its intended path cannot pass.
void note_harness_failure();

}  // namespace lob::bench
