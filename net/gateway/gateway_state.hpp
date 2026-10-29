#pragma once

// Internal state shared by the gateway's two threads. Not a public API: the
// layout exists so the engine and network implementations can live in
// separate translation units while the public header stays small.

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <thread>
#include <type_traits>

#include "lob/spsc_ring.hpp"
#include "lob/types.hpp"
#include "net/gateway/gateway.hpp"
#include "net/protocol/messages.hpp"

namespace lob::net::detail {

/// Depth of the command ring (network -> engine). Each entry is one decoded
/// client command, so this bounds how far a client can run ahead of the book.
inline constexpr std::size_t kCommandRingCapacity = std::size_t{1} << 14;

/// Depth of the response ring (engine -> network). A single command can emit
/// more than one response (a sweep emits a Fill per maker plus the taker's
/// remainder), so this is larger than the command ring.
inline constexpr std::size_t kResponseRingCapacity = std::size_t{1} << 16;

/// Depth of the market-data ring (engine -> network). Slow subscribers are
/// dropped by the network thread; the engine never skips an event.
inline constexpr std::size_t kMdRingCapacity = std::size_t{1} << 16;

/// Client order ids are the low 40 bits of an engine order id.
inline constexpr std::uint64_t kMaxClientOrderId = (std::uint64_t{1} << 40) - 1;

/// Concurrent order-entry sessions; also the width of the response lookup.
inline constexpr std::uint32_t kMaxSessions = 1024;

/// Concurrent market-data subscribers.
inline constexpr std::uint32_t kMaxSubscribers = 256;

/// Session ids occupy the high 24 bits of an engine order id.
inline constexpr std::uint32_t kFirstSessionId = 1;
inline constexpr std::uint32_t kMaxSessionId = (std::uint32_t{1} << 24) - 1;

/// Bytes of decoded client input held while the command ring is full. A client
/// that sends faster than the engine drains is disconnected past this bound.
inline constexpr std::size_t kInputBufferLimit = std::size_t{1} << 20;

/// Bytes queued for one order-entry session before it is disconnected. A
/// client that reads its responses never comes close.
inline constexpr std::size_t kEntryOutputLimit = std::size_t{4} << 20;

/// Bytes queued for one market-data subscriber before it is dropped. With the
/// subscriber socket's 64 KiB send buffer this is the documented "slow
/// subscriber" bound (docs/architecture.md section 15).
inline constexpr std::size_t kMdOutputLimit = std::size_t{1} << 20;

/// One decoded client command. Fixed size and trivially copyable so the ring
/// can move it as bytes.
enum class CommandKind : std::uint8_t { Submit = 0, Cancel = 1, Modify = 2 };

struct Command {
  CommandKind kind = CommandKind::Submit;
  Side side = Side::Buy;
  OrderType type = OrderType::Limit;
  std::uint32_t session = 0;
  OrderId order_id = 0;
  Price price = 0;
  Qty qty = 0;
};

static_assert(std::is_trivially_copyable_v<Command>);
static_assert(sizeof(Command) <= 64);

/// An encoded frame for one order-entry session. The engine encodes it with a
/// zero header sequence; the network thread stamps the per-connection sequence
/// number in the order it appends frames to that session's socket.
struct ResponseFrame {
  std::uint32_t session = 0;
  std::uint16_t size = 0;
  std::uint16_t reserved = 0;
  std::array<std::byte, kMaxWireSize> bytes{};
};

static_assert(std::is_trivially_copyable_v<ResponseFrame>);

/// An encoded market-data frame. Its header sequence number is the stream
/// sequence (1, 2, 3, ...) and is forwarded unchanged to every subscriber.
struct MdFrame {
  std::uint16_t size = 0;
  std::uint16_t reserved = 0;
  std::array<std::byte, kMaxWireSize> bytes{};
};

static_assert(std::is_trivially_copyable_v<MdFrame>);

/// State owned by the Gateway object. The rings are members, so the whole
/// struct is a few megabytes and must be heap-allocated by the caller.
struct GatewayState {
  explicit GatewayState(GatewayOptions options);
  ~GatewayState();

  GatewayState(const GatewayState&) = delete;
  GatewayState& operator=(const GatewayState&) = delete;

  GatewayOptions options;

  // Listening sockets and the self-pipe the engine thread uses to wake the
  // network thread. All are opened before start() and closed by
  // close_sockets(), which is idempotent.
  int entry_listener = -1;
  int md_listener = -1;
  int wake_read = -1;
  int wake_write = -1;
  std::uint16_t entry_port = 0;
  std::uint16_t md_port = 0;

  std::atomic<bool> wake_pending{false};
  std::atomic<bool> stop_requested{false};
  std::atomic<bool> engine_done{false};
  std::atomic<std::uint64_t> md_published{0};
  std::atomic<std::uint64_t> md_forwarded{0};
  std::atomic<std::size_t> subscribers{0};
  std::atomic<std::size_t> sessions{0};

  /// Main-thread flag: threads exist and have not been joined yet.
  bool started = false;
  std::thread engine_thread;
  std::thread network_thread;

  SpscRing<Command, kCommandRingCapacity> commands;
  SpscRing<ResponseFrame, kResponseRingCapacity> responses;
  SpscRing<MdFrame, kMdRingCapacity> md;

  /// Opens both listeners and the wake pipe; throws on failure.
  void open_sockets();
  /// Closes the listeners and the wake pipe; safe to call more than once.
  void close_sockets();

  /// Wakes the network thread without a syscall per event: the pipe is written
  /// only when it is not already known to hold a wake-up.
  void wake();

  /// Engine thread: drives the book from the command ring.
  void run_engine();

  /// Network thread: sockets, framing and fan-out.
  void run_network();

  /// Engine thread only. Blocks until the response ring has a slot.
  void push_response(std::uint32_t session, std::span<const std::byte> frame);

  /// Engine thread only. Blocks until the market-data ring has a slot.
  void publish_md(std::span<const std::byte> frame, Seq seq);
};

}  // namespace lob::net::detail
