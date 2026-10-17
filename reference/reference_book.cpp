#include "reference/reference_book.hpp"

#include "lob/digest.hpp"

namespace lob::ref {

ReferenceBook::ReferenceBook(const Config& config, Listener& listener)
    : config_(config), listener_(listener) {}

void ReferenceBook::submit(OrderId, Side, OrderType, Price, Qty) {}

void ReferenceBook::cancel(OrderId) {}

void ReferenceBook::modify(OrderId, Price, Qty) {}

std::optional<Price> ReferenceBook::best_bid() const { return std::nullopt; }

std::optional<Price> ReferenceBook::best_ask() const { return std::nullopt; }

Qty ReferenceBook::level_qty(Side, Price) const { return 0; }

std::size_t ReferenceBook::top_levels(Side, std::span<LevelInfo>) const { return 0; }

std::size_t ReferenceBook::order_count() const { return 0; }

std::uint64_t ReferenceBook::digest() const {
  DigestBuilder builder;
  return builder.finish();
}

}  // namespace lob::ref
