#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <thread>

#include "lob/listener.hpp"
#include "lob/order_book.hpp"
#include "lob/types.hpp"
#include "net/gateway/gateway_state.hpp"
#include "net/protocol/codec.hpp"
#include "net/protocol/messages.hpp"

namespace lob::net::detail {
namespace {

constexpr OrderId kClientMask = kMaxClientOrderId;

constexpr std::uint32_t session_of(OrderId id) { return static_cast<std::uint32_t>(id >> 40); }

constexpr OrderId client_of(OrderId id) { return id & kClientMask; }

/// Turns book events into the two outgoing streams. It runs on the engine
/// thread only: every push is fixed-size and allocation-free.
class EngineListener final : public Listener {
 public:
  explicit EngineListener(GatewayState& state) : state_(state) {}

  void on_accept(OrderId id) override {
    Ack message;
    message.order_id = client_of(id);
    emit(session_of(id), message);
  }

  void on_reject(OrderId id, RejectReason reason) override {
    Reject message;
    message.order_id = client_of(id);
    message.reason = reason;
    emit(session_of(id), message);
  }

  void on_trade(OrderId maker_id, OrderId taker_id, Price price, Qty qty) override {
    const std::uint32_t maker_session = session_of(maker_id);
    const std::uint32_t taker_session = session_of(taker_id);
    Fill fill;
    // Responses carry client order ids: the low 40 bits of the engine id are
    // exactly what the owning session sent, so each side can match on them.
    fill.maker_id = client_of(maker_id);
    fill.taker_id = client_of(taker_id);
    fill.price = price;
    fill.qty = qty;
    emit(maker_session, fill);
    if (taker_session != maker_session) {
      emit(taker_session, fill);
    }

    Trade trade;
    trade.maker_id = maker_id;
    trade.taker_id = taker_id;
    trade.price = price;
    trade.qty = qty;
    publish(trade);
  }

  void on_cancel(OrderId id, Qty cancelled_qty) override {
    Cancelled message;
    message.order_id = client_of(id);
    message.qty = cancelled_qty;
    emit(session_of(id), message);
  }

  void on_modify(OrderId id, Price price, Qty new_qty) override {
    // The wire protocol has no Modified message. An in-place reduce is
    // acknowledged with an Ack for the same order so a client always gets a
    // response to a command that changed the book; the new level quantity is
    // on the market-data stream. See docs/architecture.md section 15.
    static_cast<void>(price);
    static_cast<void>(new_qty);
    Ack message;
    message.order_id = client_of(id);
    emit(session_of(id), message);
  }

  void on_book_update(Side side, Price price, Qty new_level_qty) override {
    BookUpdate update;
    update.price = price;
    update.qty = new_level_qty;
    update.side = side;
    publish(update);
  }

 private:
  template <class Message>
  void emit(std::uint32_t session, const Message& message) {
    std::array<std::byte, kMaxWireSize> buffer{};
    const std::size_t size = encode(message, 0, buffer);
    state_.push_response(session, std::span<const std::byte>(buffer.data(), size));
  }

  template <class Message>
  void publish(const Message& message) {
    const Seq seq = ++seq_;
    std::array<std::byte, kMaxWireSize> buffer{};
    const std::size_t size = encode(message, seq, buffer);
    state_.publish_md(std::span<const std::byte>(buffer.data(), size), seq);
  }

  GatewayState& state_;
  Seq seq_ = 0;
};

void apply(OrderBook& book, const Command& command) {
  switch (command.kind) {
    case CommandKind::Submit:
      book.submit(command.order_id, command.side, command.type, command.price, command.qty);
      break;
    case CommandKind::Cancel:
      book.cancel(command.order_id);
      break;
    case CommandKind::Modify:
      book.modify(command.order_id, command.price, command.qty);
      break;
  }
}

}  // namespace

void GatewayState::run_engine() {
  EngineListener listener(*this);
  OrderBook book(options.book, listener);
  Command command{};
  // Yield briefly, then sleep, so an idle gateway does not spin a core flat
  // while a new command is still picked up within tens of microseconds.
  std::uint32_t idle_rounds = 0;
  for (;;) {
    if (commands.try_pop(command)) {
      apply(book, command);
      idle_rounds = 0;
      continue;
    }
    if (stop_requested.load(std::memory_order_acquire)) {
      // Finish what the network thread already accepted before shutting down.
      if (!commands.try_pop(command)) {
        break;
      }
      apply(book, command);
      continue;
    }
    if (idle_rounds < 2000) {
      ++idle_rounds;
      std::this_thread::yield();
    } else {
      std::this_thread::sleep_for(std::chrono::microseconds(20));
    }
  }
  engine_done.store(true, std::memory_order_release);
  wake();
}

}  // namespace lob::net::detail
