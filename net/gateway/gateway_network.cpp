// Network thread for the order-entry gateway: listeners, per-connection
// framing, response fan-out and the market-data stream. Everything here runs
// on one thread (the "network thread" of decision C54); the only shared state
// with the engine thread is the three SPSC rings and the atomics in
// GatewayState.

#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "lob/types.hpp"
#include "net/gateway/gateway_state.hpp"
#include "net/protocol/codec.hpp"
#include "net/protocol/framer.hpp"
#include "net/protocol/messages.hpp"

namespace lob::net::detail {
namespace {

constexpr std::size_t kReadChunk = 8192;

/// Send buffer requested from the kernel on a market-data subscriber socket.
/// Keeping it small makes the "slow subscriber" bound (kMdOutputLimit) the
/// dominant one instead of an autotuned kernel buffer.
constexpr int kSubscriberSendBuffer = 64 * 1024;

/// Smallest possible frame: a heartbeat, header plus an 8-byte body. Used to
/// bound how many commands one input buffer can produce.
constexpr std::size_t kMinWireFrame = kHeaderSize + 8;

#if defined(MSG_NOSIGNAL)
constexpr int kSendFlags = MSG_NOSIGNAL;
#else
constexpr int kSendFlags = 0;
#endif

bool set_nonblocking(int fd) {
  const int flags = ::fcntl(fd, F_GETFL, 0);
  return flags >= 0 && ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

int open_listener(std::uint16_t port, std::uint16_t& bound_port) {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    throw std::runtime_error(std::string("gateway: socket: ") + std::strerror(errno));
  }
  const int one = 1;
  ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);

  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_ANY);
  address.sin_port = htons(port);
  if (::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof address) != 0) {
    const int error = errno;
    ::close(fd);
    throw std::runtime_error(std::string("gateway: bind: ") + std::strerror(error));
  }
  if (::listen(fd, 256) != 0) {
    const int error = errno;
    ::close(fd);
    throw std::runtime_error(std::string("gateway: listen: ") + std::strerror(error));
  }
  if (!set_nonblocking(fd)) {
    const int error = errno;
    ::close(fd);
    throw std::runtime_error(std::string("gateway: non-blocking: ") + std::strerror(error));
  }

  sockaddr_in local{};
  socklen_t length = sizeof local;
  if (::getsockname(fd, reinterpret_cast<sockaddr*>(&local), &length) != 0) {
    const int error = errno;
    ::close(fd);
    throw std::runtime_error(std::string("gateway: getsockname: ") + std::strerror(error));
  }
  bound_port = ntohs(local.sin_port);
  return fd;
}

enum class ConnKind : std::uint8_t { Entry = 0, Md = 1 };

struct Connection {
  int fd = -1;
  ConnKind kind = ConnKind::Entry;
  std::uint32_t session = 0;

  /// Sequence number stamped on the next frame appended for this session.
  Seq out_seq = 0;

  StreamFramer framer;
  std::vector<std::byte> in;   ///< received bytes the framer has not consumed
  std::vector<std::byte> out;  ///< frames waiting for the socket
  std::size_t out_offset = 0;  ///< bytes of `out` already written
  bool closing = false;

  std::size_t pending() const { return out.size() - out_offset; }
};

struct NetworkState {
  GatewayState* gateway = nullptr;
  std::vector<std::unique_ptr<Connection>> connections;
  std::unordered_map<std::uint32_t, Connection*> sessions;
  std::uint32_t next_session = kFirstSessionId;

  std::vector<pollfd> poll_fds;
  struct Target {
    enum class Kind : std::uint8_t { Wake = 0, EntryListener = 1, MdListener = 2, Connection = 3 };
    Kind kind = Kind::Wake;
    Connection* connection = nullptr;
  };
  std::vector<Target> poll_targets;
};

/// Reads the stream sequence number out of a market-data frame header.
Seq frame_seq(std::span<const std::byte> frame) {
  Seq seq = 0;
  for (std::size_t i = 0; i < 8; ++i) {
    seq |= static_cast<Seq>(std::to_integer<std::uint8_t>(frame[kHeaderSeqOffset + i])) << (8 * i);
  }
  return seq;
}

void close_connection(NetworkState& net, Connection& connection) {
  if (connection.closing) {
    return;
  }
  connection.closing = true;
  if (connection.fd >= 0) {
    ::close(connection.fd);
    connection.fd = -1;
  }
  if (connection.kind == ConnKind::Entry) {
    net.sessions.erase(connection.session);
    net.gateway->sessions.fetch_sub(1, std::memory_order_relaxed);
  } else {
    net.gateway->subscribers.fetch_sub(1, std::memory_order_relaxed);
  }
}

/// Appends one order-entry frame, stamping the per-connection sequence number.
void append_patched_frame(NetworkState& net, Connection& connection,
                          std::span<const std::byte> frame, std::size_t limit) {
  if (connection.closing) {
    return;
  }
  if (connection.pending() + frame.size() > limit) {
    close_connection(net, connection);
    return;
  }
  const std::size_t offset = connection.out.size();
  connection.out.insert(connection.out.end(), frame.begin(), frame.end());
  const Seq seq = ++connection.out_seq;
  for (std::size_t i = 0; i < 8; ++i) {
    connection.out[offset + kHeaderSeqOffset + i] = static_cast<std::byte>((seq >> (8 * i)) & 0xff);
  }
}

/// Appends one market-data frame unchanged: its header already carries the
/// gap-free stream sequence number.
void append_stream_frame(NetworkState& net, Connection& connection,
                         std::span<const std::byte> frame, std::size_t limit) {
  if (connection.closing) {
    return;
  }
  if (connection.pending() + frame.size() > limit) {
    // A subscriber that fell this far behind is dropped rather than allowed to
    // slow the engine down. kMdOutputLimit is the documented bound.
    close_connection(net, connection);
    return;
  }
  connection.out.insert(connection.out.end(), frame.begin(), frame.end());
}

void flush_connection(NetworkState& net, Connection& connection) {
  while (connection.out_offset < connection.out.size()) {
    const ssize_t written = ::send(connection.fd, connection.out.data() + connection.out_offset,
                                   connection.out.size() - connection.out_offset, kSendFlags);
    if (written > 0) {
      connection.out_offset += static_cast<std::size_t>(written);
      continue;
    }
    if (written < 0 && errno == EINTR) {
      continue;
    }
    if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      break;
    }
    close_connection(net, connection);
    return;
  }
  if (connection.out_offset == connection.out.size()) {
    connection.out.clear();
    connection.out_offset = 0;
  } else if (connection.out_offset >= 65536) {
    connection.out.erase(
        connection.out.begin(),
        connection.out.begin() + static_cast<std::ptrdiff_t>(connection.out_offset));
    connection.out_offset = 0;
  }
}

void reject_direct(NetworkState& net, Connection& connection, OrderId client_id,
                   RejectReason reason) {
  Reject message;
  message.order_id = client_id;
  message.reason = reason;
  std::array<std::byte, Reject::kWireSize> buffer{};
  const std::size_t size = encode(message, 0, buffer);
  append_patched_frame(net, connection, std::span<const std::byte>(buffer.data(), size),
                       kEntryOutputLimit);
}

/// Decodes client frames into engine commands. Heartbeats are echoed here
/// because they need no book state; every other message becomes one Command.
class CommandSink final : public FrameSink {
 public:
  CommandSink(NetworkState& net, Connection& connection) : net_(net), connection_(connection) {}

  bool on_frame(std::span<const std::byte> frame) override {
    Header header;
    if (decode_header(frame, header) != DecodeStatus::Ok) {
      return fail();
    }
    Seq seq = 0;
    switch (header.type) {
      case MessageType::NewOrder:
        return new_order(frame, seq);
      case MessageType::Cancel:
        return cancel(frame, seq);
      case MessageType::Modify:
        return modify(frame, seq);
      case MessageType::Heartbeat:
        return heartbeat(frame, seq);
      default:
        // Ack/Reject/Fill/Cancelled/BookUpdate/Trade/SequenceReset and
        // RecoveryRequest are not order-entry requests: a client sending one
        // is speaking the wrong dialect.
        return fail();
    }
  }

  bool failed() const { return failed_; }

 private:
  bool fail() {
    failed_ = true;
    return false;
  }

  bool connection_id_ok(OrderId client_id) {
    if (client_id <= kMaxClientOrderId) {
      return true;
    }
    // The id cannot name an order without colliding with another session's
    // id space. Reject it and keep the session open.
    reject_direct(net_, connection_, client_id, RejectReason::CapacityExceeded);
    return false;
  }

  bool push(const Command& command) {
    // feed() guarantees a free slot for every frame it hands the framer, so
    // this wait never starts a deadlock against a full response ring.
    while (!net_.gateway->commands.try_push(command)) {
      std::this_thread::yield();
    }
    return true;
  }

  bool new_order(std::span<const std::byte> frame, Seq seq) {
    NewOrder message;
    if (decode(frame, message, seq) != DecodeStatus::Ok) {
      return fail();
    }
    if (!connection_id_ok(message.order_id)) {
      return true;
    }
    Command command;
    command.kind = CommandKind::Submit;
    command.session = connection_.session;
    command.side = message.side;
    command.type = message.type;
    command.order_id = (static_cast<OrderId>(connection_.session) << 40) | message.order_id;
    command.price = message.price;
    command.qty = message.qty;
    return push(command);
  }

  bool cancel(std::span<const std::byte> frame, Seq seq) {
    Cancel message;
    if (decode(frame, message, seq) != DecodeStatus::Ok) {
      return fail();
    }
    if (!connection_id_ok(message.order_id)) {
      return true;
    }
    Command command;
    command.kind = CommandKind::Cancel;
    command.session = connection_.session;
    command.order_id = (static_cast<OrderId>(connection_.session) << 40) | message.order_id;
    return push(command);
  }

  bool modify(std::span<const std::byte> frame, Seq seq) {
    Modify message;
    if (decode(frame, message, seq) != DecodeStatus::Ok) {
      return fail();
    }
    if (!connection_id_ok(message.order_id)) {
      return true;
    }
    Command command;
    command.kind = CommandKind::Modify;
    command.session = connection_.session;
    command.order_id = (static_cast<OrderId>(connection_.session) << 40) | message.order_id;
    command.price = message.price;
    command.qty = message.qty;
    return push(command);
  }

  bool heartbeat(std::span<const std::byte> frame, Seq seq) {
    Heartbeat message;
    if (decode(frame, message, seq) != DecodeStatus::Ok) {
      return fail();
    }
    std::array<std::byte, Heartbeat::kWireSize> buffer{};
    const std::size_t size = encode(message, 0, buffer);
    append_patched_frame(net_, connection_, std::span<const std::byte>(buffer.data(), size),
                         kEntryOutputLimit);
    return true;
  }

  NetworkState& net_;
  Connection& connection_;
  bool failed_ = false;
};

/// Hands buffered input to the framer. Returns false when the command ring is
/// too full to accept everything one framer call could produce; the bytes stay
/// buffered and are retried on the next pass.
bool feed(NetworkState& net, Connection& connection) {
  if (connection.in.empty()) {
    return true;
  }
  const std::size_t max_frames = connection.in.size() / kMinWireFrame + 4;
  if (net.gateway->commands.size_approx() + max_frames > kCommandRingCapacity) {
    return false;
  }
  CommandSink sink(net, connection);
  const PushResult result = connection.framer.push(connection.in, sink);
  if (result.consumed != 0) {
    connection.in.erase(connection.in.begin(),
                        connection.in.begin() + static_cast<std::ptrdiff_t>(result.consumed));
  }
  if (result.result == FramerResult::Error || sink.failed()) {
    close_connection(net, connection);
    return false;
  }
  return result.result != FramerResult::Stopped;
}

void read_connection(NetworkState& net, Connection& connection) {
  std::array<std::byte, kReadChunk> buffer{};
  for (;;) {
    const ssize_t received = ::recv(connection.fd, buffer.data(), buffer.size(), 0);
    if (received > 0) {
      if (connection.kind == ConnKind::Md) {
        // Subscribers send nothing meaningful; discard with the socket open so
        // a stray write neither breaks framing nor keeps the connection alive.
        continue;
      }
      connection.in.insert(connection.in.end(), buffer.begin(), buffer.begin() + received);
      if (connection.in.size() > kInputBufferLimit) {
        close_connection(net, connection);
        return;
      }
      if (!feed(net, connection)) {
        return;
      }
      continue;
    }
    if (received == 0) {
      close_connection(net, connection);
      return;
    }
    if (errno == EINTR) {
      continue;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      return;
    }
    close_connection(net, connection);
    return;
  }
}

void handle_connection(NetworkState& net, Connection& connection, short revents) {
  if (connection.closing) {
    return;
  }
  if ((revents & (POLLERR | POLLNVAL)) != 0) {
    close_connection(net, connection);
    return;
  }
  if ((revents & POLLIN) != 0) {
    read_connection(net, connection);
    if (connection.closing) {
      return;
    }
  }
  if ((revents & POLLOUT) != 0) {
    flush_connection(net, connection);
    if (connection.closing) {
      return;
    }
  }
  if ((revents & POLLHUP) != 0) {
    read_connection(net, connection);
    if (!connection.closing) {
      close_connection(net, connection);
    }
  }
}

void configure_accepted(int fd, ConnKind kind) {
  const int one = 1;
  ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
#if defined(SO_NOSIGPIPE)
  ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
  if (kind == ConnKind::Md) {
    int size = kSubscriberSendBuffer;
    ::setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &size, sizeof size);
  }
}

void accept_connections(NetworkState& net, int listener, ConnKind kind) {
  for (;;) {
    sockaddr_in address{};
    socklen_t length = sizeof address;
    const int fd = ::accept(listener, reinterpret_cast<sockaddr*>(&address), &length);
    if (fd < 0) {
      // EAGAIN ends the burst; any other error is transient enough to retry on
      // the next readable event.
      return;
    }
    if (kind == ConnKind::Entry) {
      if (net.gateway->sessions.load(std::memory_order_relaxed) >= kMaxSessions ||
          net.next_session > kMaxSessionId) {
        ::close(fd);
        continue;
      }
    } else if (net.gateway->subscribers.load(std::memory_order_relaxed) >= kMaxSubscribers) {
      ::close(fd);
      continue;
    }
    if (!set_nonblocking(fd)) {
      ::close(fd);
      continue;
    }
    configure_accepted(fd, kind);

    auto connection = std::make_unique<Connection>();
    connection->fd = fd;
    connection->kind = kind;
    if (kind == ConnKind::Entry) {
      connection->session = net.next_session++;
      net.sessions.emplace(connection->session, connection.get());
      net.gateway->sessions.fetch_add(1, std::memory_order_relaxed);
    } else {
      net.gateway->subscribers.fetch_add(1, std::memory_order_relaxed);
    }
    net.connections.push_back(std::move(connection));
  }
}

void drain_wake(GatewayState& gateway) {
  std::byte buffer[64];
  for (;;) {
    const ssize_t read_bytes = ::read(gateway.wake_read, buffer, sizeof buffer);
    if (read_bytes > 0) {
      continue;
    }
    if (read_bytes < 0 && errno == EINTR) {
      continue;
    }
    break;
  }
  gateway.wake_pending.store(false, std::memory_order_release);
}

void drain_responses(NetworkState& net) {
  ResponseFrame response{};
  while (net.gateway->responses.try_pop(response)) {
    const auto found = net.sessions.find(response.session);
    if (found == net.sessions.end()) {
      continue;
    }
    append_patched_frame(net, *found->second,
                         std::span<const std::byte>(response.bytes.data(), response.size),
                         kEntryOutputLimit);
  }
}

void drain_md(NetworkState& net) {
  MdFrame message{};
  while (net.gateway->md.try_pop(message)) {
    const std::span<const std::byte> frame(message.bytes.data(), message.size);
    for (const auto& connection : net.connections) {
      if (connection->kind == ConnKind::Md && !connection->closing) {
        append_stream_frame(net, *connection, frame, kMdOutputLimit);
      }
    }
    net.gateway->md_forwarded.store(frame_seq(frame), std::memory_order_release);
  }
}

void reap_connections(NetworkState& net) {
  net.connections.erase(std::remove_if(net.connections.begin(), net.connections.end(),
                                       [](const std::unique_ptr<Connection>& connection) {
                                         return connection->closing;
                                       }),
                        net.connections.end());
}

}  // namespace

void GatewayState::open_sockets() {
  try {
    int pipe_fds[2] = {-1, -1};
    if (::pipe(pipe_fds) != 0) {
      throw std::runtime_error(std::string("gateway: pipe: ") + std::strerror(errno));
    }
    wake_read = pipe_fds[0];
    wake_write = pipe_fds[1];
    if (!set_nonblocking(wake_read) || !set_nonblocking(wake_write)) {
      throw std::runtime_error(std::string("gateway: non-blocking pipe: ") + std::strerror(errno));
    }
    entry_listener = open_listener(options.port, entry_port);
    md_listener = open_listener(options.md_port, md_port);
  } catch (...) {
    close_sockets();
    throw;
  }
}

void GatewayState::close_sockets() {
  const auto close_fd = [](int& fd) {
    if (fd >= 0) {
      ::close(fd);
      fd = -1;
    }
  };
  close_fd(entry_listener);
  close_fd(md_listener);
  close_fd(wake_read);
  close_fd(wake_write);
}

void GatewayState::run_network() {
  NetworkState net;
  net.gateway = this;

  for (;;) {
    const bool stopping = stop_requested.load(std::memory_order_acquire);
    // Retry input that was held back while the command ring was full. Once a
    // shutdown starts no new command is accepted, so the engine cannot exit
    // while commands are still being queued behind it.
    if (!stopping) {
      for (const auto& connection : net.connections) {
        if (!connection->closing && !connection->in.empty() &&
            connection->kind == ConnKind::Entry) {
          feed(net, *connection);
        }
      }
    }
    drain_md(net);
    drain_responses(net);

    if (stopping && engine_done.load(std::memory_order_acquire) && md.size_approx() == 0 &&
        responses.size_approx() == 0) {
      break;
    }

    net.poll_fds.clear();
    net.poll_targets.clear();
    const auto add = [&net](int fd, short events, NetworkState::Target::Kind kind,
                            Connection* connection) {
      net.poll_fds.push_back(pollfd{fd, events, 0});
      net.poll_targets.push_back(NetworkState::Target{kind, connection});
    };
    add(wake_read, POLLIN, NetworkState::Target::Kind::Wake, nullptr);
    if (!stopping) {
      add(entry_listener, POLLIN, NetworkState::Target::Kind::EntryListener, nullptr);
      add(md_listener, POLLIN, NetworkState::Target::Kind::MdListener, nullptr);
    }
    for (const auto& connection : net.connections) {
      if (connection->closing) {
        continue;
      }
      // While stopping, only flush what is already queued: a client that
      // keeps sending must not push commands at an engine that already left.
      short events = stopping ? 0 : POLLIN;
      if (connection->pending() > 0) {
        events |= POLLOUT;
      }
      add(connection->fd, events, NetworkState::Target::Kind::Connection, connection.get());
    }

    const int timeout_ms = stop_requested.load(std::memory_order_acquire) ? 2 : 20;
    const int ready = ::poll(net.poll_fds.data(), net.poll_fds.size(), timeout_ms);
    if (ready < 0) {
      if (errno == EINTR) {
        continue;
      }
      break;
    }
    if (ready > 0) {
      for (std::size_t index = 0; index < net.poll_fds.size(); ++index) {
        const short revents = net.poll_fds[index].revents;
        if (revents == 0) {
          continue;
        }
        const NetworkState::Target& target = net.poll_targets[index];
        switch (target.kind) {
          case NetworkState::Target::Kind::Wake:
            drain_wake(*this);
            break;
          case NetworkState::Target::Kind::EntryListener:
            accept_connections(net, entry_listener, ConnKind::Entry);
            break;
          case NetworkState::Target::Kind::MdListener:
            accept_connections(net, md_listener, ConnKind::Md);
            break;
          case NetworkState::Target::Kind::Connection:
            handle_connection(net, *target.connection, revents);
            break;
        }
      }
    }
    reap_connections(net);
  }

  // Best-effort final flush of whatever the engine produced during shutdown.
  for (const auto& connection : net.connections) {
    if (!connection->closing) {
      flush_connection(net, *connection);
    }
  }
  for (const auto& connection : net.connections) {
    close_connection(net, *connection);
  }
  net.connections.clear();
  net.sessions.clear();
}

}  // namespace lob::net::detail
