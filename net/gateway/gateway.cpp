#include "net/gateway/gateway.hpp"

#include <unistd.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <thread>
#include <utility>

#include "lob/config.hpp"
#include "net/gateway/gateway_state.hpp"

namespace lob::net {
namespace {

/// The array ladder indexes every tick in [min_price, max_price]; refuse a
/// range it cannot represent before the engine thread constructs the book.
[[maybe_unused]] std::uint64_t price_range(const Config& book) {
  return static_cast<std::uint64_t>(book.max_price - book.min_price) + 1;
}

}  // namespace

namespace detail {

GatewayState::GatewayState(GatewayOptions options_in) : options(std::move(options_in)) {
  if (!valid(options.book)) {
    throw std::invalid_argument("gateway: invalid book configuration");
  }
#if defined(LOB_PRICE_LADDER_ARRAY)
  if (price_range(options.book) > (std::uint64_t{1} << 24)) {
    throw std::invalid_argument("gateway: price range exceeds 2^24 ticks");
  }
#endif
  open_sockets();
}

GatewayState::~GatewayState() { close_sockets(); }

void GatewayState::wake() {
  // One outstanding wake-up is enough: the network thread drains both rings on
  // every pass, so a second byte would only add a syscall.
  if (wake_pending.exchange(true, std::memory_order_acq_rel)) {
    return;
  }
  if (wake_write < 0) {
    return;
  }
  const std::byte byte{0};
  // A full pipe already guarantees a wake-up, so a short or failed write is
  // harmless here.
  [[maybe_unused]] const ssize_t written = ::write(wake_write, &byte, 1);
}

void GatewayState::push_response(std::uint32_t session, std::span<const std::byte> frame) {
  ResponseFrame response{};
  response.session = session;
  response.size = static_cast<std::uint16_t>(frame.size());
  std::copy(frame.begin(), frame.end(), response.bytes.begin());
  // The network thread drains this ring continuously; the wait is bounded by
  // one poll pass on the other side and never drops a response.
  while (!responses.try_push(response)) {
    std::this_thread::yield();
  }
  wake();
}

void GatewayState::publish_md(std::span<const std::byte> frame, Seq seq) {
  MdFrame message{};
  message.size = static_cast<std::uint16_t>(frame.size());
  std::copy(frame.begin(), frame.end(), message.bytes.begin());
  while (!md.try_push(message)) {
    std::this_thread::yield();
  }
  md_published.store(seq, std::memory_order_release);
  wake();
}

}  // namespace detail

Gateway::Gateway(GatewayOptions options)
    : state_(std::make_unique<detail::GatewayState>(std::move(options))) {}

Gateway::~Gateway() {
  stop();
  state_->close_sockets();
}

void Gateway::start() {
  detail::GatewayState& state = *state_;
  if (state.started) {
    return;
  }
  if (state.entry_listener < 0) {
    throw std::logic_error("gateway: cannot restart a stopped gateway");
  }
  state.stop_requested.store(false, std::memory_order_release);
  state.engine_done.store(false, std::memory_order_release);
  state.network_thread = std::thread([&state] { state.run_network(); });
  state.engine_thread = std::thread([&state] { state.run_engine(); });
  state.started = true;
}

void Gateway::stop() {
  detail::GatewayState& state = *state_;
  if (!state.started) {
    return;
  }
  state.stop_requested.store(true, std::memory_order_release);
  state.wake();
  // The engine goes first: it drains the commands already queued and pushes
  // their responses, while the network thread keeps draining both rings.
  if (state.engine_thread.joinable()) {
    state.engine_thread.join();
  }
  if (state.network_thread.joinable()) {
    state.network_thread.join();
  }
  state.close_sockets();
  state.started = false;
}

std::uint16_t Gateway::order_entry_port() const noexcept { return state_->entry_port; }

std::uint16_t Gateway::md_port() const noexcept { return state_->md_port; }

std::uint64_t Gateway::md_sequence_published() const noexcept {
  return state_->md_published.load(std::memory_order_acquire);
}

std::uint64_t Gateway::md_sequence_forwarded() const noexcept {
  return state_->md_forwarded.load(std::memory_order_acquire);
}

std::size_t Gateway::subscriber_count() const noexcept {
  return state_->subscribers.load(std::memory_order_acquire);
}

std::size_t Gateway::session_count() const noexcept {
  return state_->sessions.load(std::memory_order_acquire);
}

}  // namespace lob::net
