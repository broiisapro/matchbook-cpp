#pragma once

#include <cstdint>
#include <ostream>
#include <variant>
#include <vector>

#include "lob/listener.hpp"
#include "lob/types.hpp"

namespace lob::test {

/// Recorded listener events. Each struct is comparable and printable, so tests
/// (including the differential test) can compare event streams directly.

struct Accept {
  OrderId id;
  friend bool operator==(const Accept&, const Accept&) = default;
};

struct Reject {
  OrderId id;
  RejectReason reason;
  friend bool operator==(const Reject&, const Reject&) = default;
};

struct Trade {
  OrderId maker_id;
  OrderId taker_id;
  Price price;
  Qty qty;
  friend bool operator==(const Trade&, const Trade&) = default;
};

struct Cancel {
  OrderId id;
  Qty cancelled_qty;
  friend bool operator==(const Cancel&, const Cancel&) = default;
};

struct Modify {
  OrderId id;
  Price price;
  Qty new_qty;
  friend bool operator==(const Modify&, const Modify&) = default;
};

struct BookUpdate {
  Side side;
  Price price;
  Qty new_level_qty;
  friend bool operator==(const BookUpdate&, const BookUpdate&) = default;
};

using Event = std::variant<Accept, Reject, Trade, Cancel, Modify, BookUpdate>;

inline std::ostream& operator<<(std::ostream& out, const Accept& event) {
  return out << "Accept{id=" << event.id << "}";
}

inline std::ostream& operator<<(std::ostream& out, const Reject& event) {
  return out << "Reject{id=" << event.id << ", reason=" << lob::to_string(event.reason) << "}";
}

inline std::ostream& operator<<(std::ostream& out, const Trade& event) {
  return out << "Trade{maker=" << event.maker_id << ", taker=" << event.taker_id
             << ", price=" << event.price << ", qty=" << event.qty << "}";
}

inline std::ostream& operator<<(std::ostream& out, const Cancel& event) {
  return out << "Cancel{id=" << event.id << ", qty=" << event.cancelled_qty << "}";
}

inline std::ostream& operator<<(std::ostream& out, const Modify& event) {
  return out << "Modify{id=" << event.id << ", price=" << event.price << ", qty=" << event.new_qty
             << "}";
}

inline std::ostream& operator<<(std::ostream& out, const BookUpdate& event) {
  return out << "BookUpdate{side=" << lob::to_string(event.side) << ", price=" << event.price
             << ", qty=" << event.new_level_qty << "}";
}

inline std::ostream& operator<<(std::ostream& out, const Event& event) {
  std::visit([&out](const auto& value) { out << value; }, event);
  return out;
}

/// Appends every listener callback to events, in call order.
class RecordingListener final : public Listener {
 public:
  void on_accept(OrderId id) override { events.push_back(Accept{id}); }

  void on_reject(OrderId id, RejectReason reason) override { events.push_back(Reject{id, reason}); }

  void on_trade(OrderId maker_id, OrderId taker_id, Price price, Qty qty) override {
    events.push_back(Trade{maker_id, taker_id, price, qty});
  }

  void on_cancel(OrderId id, Qty cancelled_qty) override {
    events.push_back(Cancel{id, cancelled_qty});
  }

  void on_modify(OrderId id, Price price, Qty new_qty) override {
    events.push_back(Modify{id, price, new_qty});
  }

  void on_book_update(Side side, Price price, Qty new_level_qty) override {
    events.push_back(BookUpdate{side, price, new_level_qty});
  }

  void clear() { events.clear(); }

  std::vector<Event> events;
};

}  // namespace lob::test
