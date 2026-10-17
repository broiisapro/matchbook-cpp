#include "bench/support.hpp"

#include <atomic>

#include "lob/order_book.hpp"

namespace lob::bench {
namespace {

std::atomic<int> g_harness_failures{0};

}  // namespace

EngineState engine_state() {
  static const EngineState state = [] {
    NullListener listener;
    OrderBook resting(bench_config(), listener);
    resting.submit(1, Side::Buy, OrderType::Limit, bench_config().min_price, 1);
    if (resting.order_count() != 1) {
      return EngineState::Stub;
    }
    OrderBook crossing(bench_config(), listener);
    crossing.submit(1, Side::Sell, OrderType::Limit, 1001, 1);
    crossing.submit(2, Side::Buy, OrderType::Limit, 1001, 1);
    return crossing.order_count() == 0 ? EngineState::Ready : EngineState::RestOnly;
  }();
  return state;
}

int harness_failures() { return g_harness_failures.load(); }

void note_harness_failure() { g_harness_failures.fetch_add(1); }

}  // namespace lob::bench
