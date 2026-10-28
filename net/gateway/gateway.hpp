#pragma once

// Order-entry gateway (Phase 3, decision C54).
//
// One TCP listener carries client sessions (NewOrder, Cancel, Modify,
// Heartbeat); a second listener carries the market-data event stream
// (BookUpdate, Trade). A network thread owns every socket and a single engine
// thread owns the order book; the two exchange fixed-size messages through
// lock-free SPSC rings, so the engine thread never touches a socket and never
// allocates per message.
//
// One accepted connection is one session, with no logon message in v2.0. The
// engine order id is `(session_id << 40) | client_order_id`; a client order id
// that does not fit in 40 bits is rejected with RejectReason::CapacityExceeded.

#include <cstddef>
#include <cstdint>
#include <memory>

#include "lob/config.hpp"

namespace lob::net {

namespace detail {
struct GatewayState;
}

/// Gateway configuration.
struct GatewayOptions {
  /// TCP port for order entry; 0 lets the kernel choose one.
  std::uint16_t port = 9000;
  /// TCP port for the market-data event stream; 0 lets the kernel choose one.
  std::uint16_t md_port = 9001;
  /// Engine limits. The default matches the replay default (decision C24).
  Config book{1, 200000, 1048576, 200000};
};

/// A running gateway: listeners opened at construction, threads started by
/// start() and stopped by stop() (also called from the destructor).
///
/// Ports are bound in the constructor, so a caller that passes port 0 can read
/// the chosen ports before start() and connect as soon as start() returns.
class Gateway {
 public:
  /// Opens both listening sockets. Throws std::invalid_argument when the book
  /// configuration is unusable, and std::runtime_error when a socket cannot be
  /// opened, bound or listened on.
  explicit Gateway(GatewayOptions options);

  Gateway(const Gateway&) = delete;
  Gateway& operator=(const Gateway&) = delete;
  Gateway(Gateway&) = delete;
  Gateway& operator=(Gateway&&) = delete;

  /// Stops the threads if they are running and closes the listeners.
  ~Gateway();

  /// Starts the engine and network threads. Idempotent while already started.
  void start();

  /// Requests a graceful shutdown: the network thread keeps draining the rings
  /// until the engine thread has finished the commands already queued, then
  /// both threads are joined and the listeners are closed. Idempotent.
  void stop();

  /// Port order entry is listening on (the kernel's choice when the option was
  /// 0). Valid until the gateway is stopped.
  std::uint16_t order_entry_port() const noexcept;

  /// Port the market-data stream is listening on.
  std::uint16_t md_port() const noexcept;

  /// Highest market-data sequence number the engine has published so far. The
  /// stream starts at 1 and is gap-free.
  std::uint64_t md_sequence_published() const noexcept;

  /// Highest market-data sequence number the network thread has forwarded to
  /// subscribers (or dropped because there were none). It can lag the
  /// published value by the number of frames still in the engine-to-network
  /// ring.
  std::uint64_t md_sequence_forwarded() const noexcept;

  /// Connections currently receiving market data.
  std::size_t subscriber_count() const noexcept;

  /// Order-entry sessions currently connected.
  std::size_t session_count() const noexcept;

 private:
  std::unique_ptr<detail::GatewayState> state_;
};

}  // namespace lob::net
