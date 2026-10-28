// Python bindings for the matchbook-cpp engine.
//
// The module deliberately exposes only existing C++ functionality: the
// OrderBook API, the frozen enums and configuration, a listener adapter and
// the flow-file reader/replayer. No engine behaviour is reimplemented here.

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "lob/config.hpp"
#include "lob/order_book.hpp"
#include "lob/types.hpp"
#include "reference/reference_book.hpp"
#include "tools/flowfile/flowfile.hpp"
#include "tools/flowfile/replay_config.hpp"

namespace py = pybind11;

namespace {

using lob::OrderId;
using lob::OrderType;
using lob::Price;
using lob::Qty;
using lob::RejectReason;
using lob::Side;
using lob::flowfile::FlowOp;
using lob::flowfile::FlowRecord;

// ---------------------------------------------------------------------------
// Listener bridge
// ---------------------------------------------------------------------------

/// Forwards engine events to a Python object. Callbacks are resolved once at
/// construction, so an object that implements only some of them works: the
/// missing ones are skipped. Every event crosses the language boundary and
/// holds the GIL, which is why callbacks are documented as slow.
class PythonListener final : public lob::Listener {
 public:
  explicit PythonListener(py::object target) : target_(std::move(target)) {
    accept_ = lookup("on_accept");
    reject_ = lookup("on_reject");
    trade_ = lookup("on_trade");
    cancel_ = lookup("on_cancel");
    modify_ = lookup("on_modify");
    book_update_ = lookup("on_book_update");
  }

  void on_accept(OrderId id) override { invoke(accept_, id); }

  void on_reject(OrderId id, RejectReason reason) override { invoke(reject_, id, reason); }

  void on_trade(OrderId maker_id, OrderId taker_id, Price price, Qty qty) override {
    invoke(trade_, maker_id, taker_id, price, qty);
  }

  void on_cancel(OrderId id, Qty cancelled_qty) override { invoke(cancel_, id, cancelled_qty); }

  void on_modify(OrderId id, Price price, Qty new_qty) override {
    invoke(modify_, id, price, new_qty);
  }

  void on_book_update(Side side, Price price, Qty new_level_qty) override {
    invoke(book_update_, side, price, new_level_qty);
  }

 private:
  py::object lookup(const char* name) {
    if (!py::hasattr(target_, name)) {
      return py::object();
    }
    return target_.attr(name);
  }

  template <typename... Args>
  void invoke(const py::object& callback, const Args&... args) {
    if (!callback) {
      return;
    }
    callback(args...);
  }

  py::object target_;
  py::object accept_;
  py::object reject_;
  py::object trade_;
  py::object cancel_;
  py::object modify_;
  py::object book_update_;
};

// ---------------------------------------------------------------------------
// OrderBook wrapper
// ---------------------------------------------------------------------------

/// Owns the listener adapter and the engine so the adapter outlives the book
/// (declaration order is reversed at destruction) and the optional Python
/// listener stays alive for as long as the engine can call it.
class BoundOrderBook {
 public:
  BoundOrderBook(const lob::Config& config, py::object listener) {
    if (!lob::valid(config)) {
      throw std::invalid_argument(
          "invalid Config: need 1 <= min_price <= max_price and non-zero max_orders and "
          "max_levels");
    }
    if (listener.is_none()) {
      sink_ = &null_listener_;
    } else {
      listener_ = std::make_unique<PythonListener>(std::move(listener));
      sink_ = listener_.get();
    }
    book_ = std::make_unique<lob::OrderBook>(config, *sink_);
  }

  void submit(OrderId id, Side side, OrderType type, Price price, Qty qty) {
    book_->submit(id, side, type, price, qty);
  }

  void cancel(OrderId id) { book_->cancel(id); }

  void modify(OrderId id, Price new_price, Qty new_qty) { book_->modify(id, new_price, new_qty); }

  std::optional<Price> best_bid() const { return book_->best_bid(); }

  std::optional<Price> best_ask() const { return book_->best_ask(); }

  Qty level_qty(Side side, Price price) const { return book_->level_qty(side, price); }

  std::vector<lob::LevelInfo> top_levels(Side side, std::size_t count) const {
    std::vector<lob::LevelInfo> levels(count);
    levels.resize(book_->top_levels(side, levels));
    return levels;
  }

  std::size_t order_count() const { return book_->order_count(); }

  std::uint64_t digest() const { return book_->digest(); }

 private:
  lob::NullListener null_listener_;
  std::unique_ptr<PythonListener> listener_;
  lob::Listener* sink_ = nullptr;
  std::unique_ptr<lob::OrderBook> book_;
};

// ---------------------------------------------------------------------------
// Flow files and replay
// ---------------------------------------------------------------------------

/// One flow-file record as Python sees it. `op` is a word ("submit",
/// "cancel", "modify"); the remaining fields have the same meaning as on
/// disk. For cancels side, type and price are ignored; for modifies type is
/// ignored and price is the new price; a market submit ignores price.
struct FlowRecordView {
  std::string op;
  Side side;
  OrderType type;
  OrderId id;
  Price price;
  Qty qty;
  std::uint64_t ts_ns;

  friend bool operator==(const FlowRecordView&, const FlowRecordView&) = default;
};

std::string flow_op_name(FlowOp op) {
  switch (op) {
    case FlowOp::Submit:
      return "submit";
    case FlowOp::Cancel:
      return "cancel";
    case FlowOp::Modify:
      return "modify";
  }
  return "unknown";
}

FlowRecordView view_of(const FlowRecord& record) {
  return FlowRecordView{flow_op_name(record.op),
                        record.side,
                        record.type,
                        record.id,
                        record.price,
                        record.qty,
                        record.ts_ns};
}

bool has_csv_extension(const std::string& path) {
  return path.size() >= 4 && path.compare(path.size() - 4, 4, ".csv") == 0;
}

/// Reads a whole flow file (binary, or CSV when the path ends in ".csv").
std::vector<FlowRecordView> read_flow(const std::string& path) {
  std::vector<FlowRecordView> records;
  lob::flowfile::FlowErrorInfo error;
  auto append = [&records](const FlowRecord& record) {
    records.push_back(view_of(record));
    return true;
  };

  if (has_csv_extension(path)) {
    lob::flowfile::read_csv(path, append, error);
  } else {
    lob::flowfile::FlowReader reader;
    reader.open(path, error);
    FlowRecord record{};
    while (!error && reader.read(record, error)) {
      append(record);
    }
  }
  if (error) {
    throw std::runtime_error(path + ": " + error.message);
  }
  return records;
}

/// Counts every event and the traded quantity, like `lobsim replay`.
class CountingListener final : public lob::Listener {
 public:
  void on_accept(OrderId) override { ++accepts_; }
  void on_reject(OrderId, RejectReason) override { ++rejects_; }

  void on_trade(OrderId, OrderId, Price, Qty qty) override {
    ++trades_;
    traded_qty_ += qty;
  }

  void on_cancel(OrderId, Qty) override {}
  void on_modify(OrderId, Price, Qty) override {}
  void on_book_update(Side, Price, Qty) override {}

  std::uint64_t accepts() const { return accepts_; }
  std::uint64_t rejects() const { return rejects_; }
  std::uint64_t trades() const { return trades_; }
  Qty traded_qty() const { return traded_qty_; }

 private:
  std::uint64_t accepts_ = 0;
  std::uint64_t rejects_ = 0;
  std::uint64_t trades_ = 0;
  Qty traded_qty_ = 0;
};

struct ReplaySummary {
  std::uint64_t records = 0;
  std::uint64_t submits = 0;
  std::uint64_t cancels = 0;
  std::uint64_t modifies = 0;
  std::uint64_t accepts = 0;
  std::uint64_t rejects = 0;
  std::uint64_t trades = 0;
  Qty traded_qty = 0;
  std::size_t final_orders = 0;
  std::uint64_t digest = 0;
  double wall_ms = 0.0;
  double ns_per_record = 0.0;
};

template <typename Book>
ReplaySummary replay_with(const std::string& path, const lob::Config& config) {
  CountingListener listener;
  Book book(config, listener);

  ReplaySummary summary;
  auto apply = [&book, &summary](const FlowRecord& record) {
    switch (record.op) {
      case FlowOp::Submit:
        ++summary.submits;
        book.submit(record.id, record.side, record.type, record.price, record.qty);
        break;
      case FlowOp::Cancel:
        ++summary.cancels;
        book.cancel(record.id);
        break;
      case FlowOp::Modify:
        ++summary.modifies;
        book.modify(record.id, record.price, record.qty);
        break;
    }
    ++summary.records;
    return true;
  };

  lob::flowfile::FlowErrorInfo error;
  const auto start = std::chrono::steady_clock::now();
  if (has_csv_extension(path)) {
    lob::flowfile::read_csv(path, apply, error);
  } else {
    lob::flowfile::FlowReader reader;
    reader.open(path, error);
    FlowRecord record{};
    while (!error && reader.read(record, error)) {
      apply(record);
    }
  }
  const auto stop = std::chrono::steady_clock::now();

  if (error) {
    throw std::runtime_error(path + ": " + error.message);
  }

  const auto wall_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(stop - start).count();
  summary.accepts = listener.accepts();
  summary.rejects = listener.rejects();
  summary.trades = listener.trades();
  summary.traded_qty = listener.traded_qty();
  summary.final_orders = book.order_count();
  summary.digest = book.digest();
  summary.wall_ms = std::chrono::duration<double, std::milli>(stop - start).count();
  summary.ns_per_record = summary.records == 0
                              ? 0.0
                              : static_cast<double>(wall_ns) / static_cast<double>(summary.records);
  return summary;
}

py::dict summary_to_dict(const ReplaySummary& summary) {
  py::dict out;
  out["records"] = summary.records;
  out["submits"] = summary.submits;
  out["cancels"] = summary.cancels;
  out["modifies"] = summary.modifies;
  out["accepts"] = summary.accepts;
  out["rejects"] = summary.rejects;
  out["trades"] = summary.trades;
  out["traded_qty"] = summary.traded_qty;
  out["final_orders"] = summary.final_orders;
  out["digest"] = summary.digest;
  out["wall_ms"] = summary.wall_ms;
  out["ns_per_record"] = summary.ns_per_record;
  return out;
}

py::dict replay(const std::string& path, const std::string& engine,
                const std::optional<lob::Config>& config) {
  const lob::Config effective = config ? *config : lob::flowfile::default_replay_config();
  if (!lob::valid(effective)) {
    throw std::invalid_argument(
        "invalid Config: need 1 <= min_price <= max_price and non-zero max_orders and max_levels");
  }
  if (engine == "fast") {
    return summary_to_dict(replay_with<lob::OrderBook>(path, effective));
  }
  if (engine == "reference") {
    return summary_to_dict(replay_with<lob::ref::ReferenceBook>(path, effective));
  }
  throw std::invalid_argument("engine must be 'fast' or 'reference', got '" + engine + "'");
}

}  // namespace

PYBIND11_MODULE(_core, m) {
  m.doc() = R"doc(
Python bindings for the matchbook-cpp single-instrument limit order book.

The engine is the same C++20 implementation the CLI uses: integer prices in
ticks, integer quantities in lots, price-time priority and trades at the
resting price. Every state change is reported to an optional listener.
)doc";

  py::enum_<Side>(m, "Side", "Which side of the book an order belongs to.")
      .value("Buy", Side::Buy)
      .value("Sell", Side::Sell);

  py::enum_<OrderType>(m, "OrderType", R"doc(
Order type passed to submit().

The member names are the CamelCase form of the engine's to_string() words,
which are also the words the CSV flow format uses: ``Limit`` ("limit"),
``Market`` ("market"), ``IOC`` ("ioc"), ``FOK`` ("fok") and ``PostOnly``
("post_only"). The underlying integer values are the flow-file type byte:
limit 0, market 1, IOC 2, FOK 3, post-only 4.
)doc")
      .value("Limit", OrderType::Limit)
      .value("Market", OrderType::Market)
      .value("IOC", OrderType::IOC)
      .value("FOK", OrderType::FOK)
      .value("PostOnly", OrderType::PostOnly);

  py::enum_<RejectReason>(m, "RejectReason", R"doc(
Reason attached to a rejection event.

The member names are the CamelCase form of the engine's to_string() words:
``DuplicateId`` ("duplicate_id"), ``UnknownOrder`` ("unknown_order"),
``InvalidQuantity`` ("invalid_quantity"), ``InvalidPrice`` ("invalid_price"),
``CapacityExceeded`` ("capacity_exceeded"), ``ModifyNoChange``
("modify_no_change"), ``UnsupportedOrderType`` ("unsupported_order_type") and
``WouldCross`` ("would_cross").
)doc")
      .value("DuplicateId", RejectReason::DuplicateId)
      .value("UnknownOrder", RejectReason::UnknownOrder)
      .value("InvalidQuantity", RejectReason::InvalidQuantity)
      .value("InvalidPrice", RejectReason::InvalidPrice)
      .value("CapacityExceeded", RejectReason::CapacityExceeded)
      .value("ModifyNoChange", RejectReason::ModifyNoChange)
      .value("UnsupportedOrderType", RejectReason::UnsupportedOrderType)
      .value("WouldCross", RejectReason::WouldCross);

  py::class_<lob::Config>(m, "Config", R"doc(
Engine configuration: the price range and the order and level capacities.

Both engines take the same configuration and enforce the same limits.
)doc")
      .def(py::init([](Price min_price, Price max_price, std::uint32_t max_orders,
                       std::uint32_t max_levels) {
             return lob::Config{min_price, max_price, max_orders, max_levels};
           }),
           py::arg("min_price") = 1, py::arg("max_price") = 200000, py::arg("max_orders") = 1048576,
           py::arg("max_levels") = 200000)
      .def_readwrite("min_price", &lob::Config::min_price)
      .def_readwrite("max_price", &lob::Config::max_price)
      .def_readwrite("max_orders", &lob::Config::max_orders)
      .def_readwrite("max_levels", &lob::Config::max_levels)
      .def("__repr__", [](const lob::Config& config) {
        return "Config(min_price=" + std::to_string(config.min_price) +
               ", max_price=" + std::to_string(config.max_price) +
               ", max_orders=" + std::to_string(config.max_orders) +
               ", max_levels=" + std::to_string(config.max_levels) + ")";
      });

  py::class_<lob::LevelInfo>(m, "LevelInfo",
                             "One price level: price, total resting quantity and order count.")
      .def_readonly("price", &lob::LevelInfo::price)
      .def_readonly("qty", &lob::LevelInfo::qty)
      .def_readonly("order_count", &lob::LevelInfo::order_count)
      .def("__repr__",
           [](const lob::LevelInfo& level) {
             return "LevelInfo(price=" + std::to_string(level.price) +
                    ", qty=" + std::to_string(level.qty) +
                    ", order_count=" + std::to_string(level.order_count) + ")";
           })
      .def("__eq__", [](const lob::LevelInfo& lhs, const lob::LevelInfo& rhs) {
        return lhs.price == rhs.price && lhs.qty == rhs.qty && lhs.order_count == rhs.order_count;
      });

  py::class_<BoundOrderBook>(m, "OrderBook", R"doc(
Single-instrument, price-time-priority limit order book.

    book = pylob.OrderBook(pylob.Config())
    book.submit(1, pylob.Side.Sell, pylob.OrderType.Limit, 10_005, 3)
    book.submit(2, pylob.Side.Buy, pylob.OrderType.Limit, 10_010, 2)
    book.best_ask()   # 10005

Every state change is reported to the optional listener in the event order
defined by the architecture document: on_accept, on_reject, on_trade,
on_cancel, on_modify and on_book_update. A listener is any object that
implements the callbacks it needs; the ones it does not implement are skipped.
Callbacks run synchronously inside submit/cancel/modify and are much slower
than the no-listener path. An exception raised by a callback propagates out of
the call, but the operation is not rolled back: the book may have changed
partially.
)doc")
      .def(py::init<const lob::Config&, py::object>(), py::arg("config"),
           py::arg("listener") = py::none(),
           "Creates a book. Raises ValueError for an unusable configuration.")
      .def("submit", &BoundOrderBook::submit, py::arg("id"), py::arg("side"), py::arg("type"),
           py::arg("price"), py::arg("qty"),
           R"doc(Submits an order.

Validation, first failure wins: an engine that does not implement FOK or
post-only yet rejects those two types with UnsupportedOrderType before every
other check (docs/architecture.md section 4.1); then a non-positive quantity ->
InvalidQuantity; a limit, IOC, FOK or post-only price outside the configured
range -> InvalidPrice; an id that is already resting -> DuplicateId. A market
order ignores the price.

A limit remainder rests at the back of its level; market and IOC remainders
are cancelled. FOK (fill or kill) must fill completely against the crossing
liquidity or it is accepted and then cancelled in full without trading, and it
never rests. PostOnly adds liquidity only: it is rejected with WouldCross when
it would trade on arrival and otherwise rests exactly like a limit order.
)doc")
      .def("cancel", &BoundOrderBook::cancel, py::arg("id"),
           "Cancels a resting order. An unknown id is rejected with UnknownOrder.")
      .def("modify", &BoundOrderBook::modify, py::arg("id"), py::arg("new_price"),
           py::arg("new_qty"),
           R"doc(Modifies a resting order; new_qty is the new remaining quantity.

A same-price reduction keeps the queue position. Any other change is a cancel
followed by a new limit order at the back of the new level.
)doc")
      .def("best_bid", &BoundOrderBook::best_bid, "Best resting bid, or None.")
      .def("best_ask", &BoundOrderBook::best_ask, "Best resting ask, or None.")
      .def("level_qty", &BoundOrderBook::level_qty, py::arg("side"), py::arg("price"),
           "Total remaining quantity at a level (0 when the level is empty).")
      .def("top_levels", &BoundOrderBook::top_levels, py::arg("side"), py::arg("count"),
           "Up to count best levels on one side, best first.")
      .def("order_count", &BoundOrderBook::order_count, "Number of live resting orders.")
      .def("digest", &BoundOrderBook::digest,
           "XXH64 digest of the book in price-time order (see docs/architecture.md).");

  py::class_<FlowRecordView>(m, "FlowRecord", R"doc(
One record of a flow file.

op is "submit", "cancel" or "modify". For cancels side, type and price are
ignored; for modifies type is ignored and price is the new price; a market
submit ignores price. ts_ns is informational and never affects matching.
)doc")
      .def_readonly("op", &FlowRecordView::op)
      .def_readonly("side", &FlowRecordView::side)
      .def_readonly("type", &FlowRecordView::type)
      .def_readonly("id", &FlowRecordView::id)
      .def_readonly("price", &FlowRecordView::price)
      .def_readonly("qty", &FlowRecordView::qty)
      .def_readonly("ts_ns", &FlowRecordView::ts_ns)
      .def("__repr__",
           [](const FlowRecordView& record) {
             return "FlowRecord(op='" + record.op + "', id=" + std::to_string(record.id) +
                    ", price=" + std::to_string(record.price) +
                    ", qty=" + std::to_string(record.qty) + ")";
           })
      .def("__eq__",
           [](const FlowRecordView& lhs, const FlowRecordView& rhs) { return lhs == rhs; });

  m.def("read_flow", &read_flow, py::arg("path"), R"doc(
Reads a flow file into a list of FlowRecord objects.

A binary flow file is the default; a path ending in ".csv" selects the CSV
form. The whole file is loaded into memory, so this is meant for tools and
examples rather than huge flows.
)doc");

  m.def("replay", &replay, py::arg("path"), py::arg("engine") = "fast",
        py::arg("config") = py::none(), R"doc(
Replays a flow file and returns the summary dictionary `lobsim replay` prints.

Keys: records, submits, cancels, modifies, accepts, rejects, trades,
traded_qty, final_orders, digest, wall_ms, ns_per_record. The digest is always
included. config defaults to the flow tools' default configuration.
)doc");
}
