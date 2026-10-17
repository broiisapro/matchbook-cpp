#include "lob/order_book.hpp"

#include "lob/digest.hpp"

namespace lob {

OrderBook::OrderBook(const Config& config, Listener& listener)
    : config_(config), listener_(listener) {}

void OrderBook::submit(OrderId, Side, OrderType, Price, Qty) {}

void OrderBook::cancel(OrderId) {}

void OrderBook::modify(OrderId, Price, Qty) {}

std::optional<Price> OrderBook::best_bid() const { return std::nullopt; }

std::optional<Price> OrderBook::best_ask() const { return std::nullopt; }

Qty OrderBook::level_qty(Side, Price) const { return 0; }

std::size_t OrderBook::top_levels(Side, std::span<LevelInfo>) const { return 0; }

std::size_t OrderBook::order_count() const { return 0; }

std::uint64_t OrderBook::digest() const {
  DigestBuilder builder;
  return builder.finish();
}

}  // namespace lob
