#include "bench/support.hpp"

#include <atomic>

#include "lob/order_book.hpp"

namespace lob::bench {
namespace {

std::atomic<int> g_harness_failures{0};

}  // namespace

bool engine_implemented() {
  static const bool implemented = [] {
    NullListener listener;
    OrderBook book(bench_config(), listener);
    book.submit(1, Side::Buy, OrderType::Limit, bench_config().min_price, 1);
    return book.order_count() == 1;
  }();
  return implemented;
}

int harness_failures() { return g_harness_failures.load(); }

void note_harness_failure() { g_harness_failures.fetch_add(1); }

}  // namespace lob::bench
