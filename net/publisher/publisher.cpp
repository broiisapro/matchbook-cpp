#include "net/publisher/publisher.hpp"

#include <poll.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <limits>
#include <utility>

#include "net/protocol/codec.hpp"
#include "net/publisher/socket.hpp"

namespace lob::publisher {

using net::decode;
using net::decode_header;
using net::DecodeStatus;
using net::encode;
using net::FramerResult;
using net::Header;
using net::MessageType;
using net::PushResult;
using net::RecoveryRequest;
using net::SequenceReset;

namespace {

constexpr std::size_t kReadBufferBytes = 64 * 1024;
/// A recovery client can queue one full retransmit window; a few dozen such
/// clients are plenty for a demo and keep a misbehaving subscriber from
/// exhausting memory.
constexpr std::size_t kMaxRecoveryClients = 64;

}  // namespace

Publisher::Publisher(PublisherConfig config)
    : config_(std::move(config)),
      store_(config_.retransmit_capacity),
      packer_(config_.max_datagram_bytes),
      read_buffer_(kReadBufferBytes) {
  if (config_.reconnect_initial.count() <= 0) {
    config_.reconnect_initial = std::chrono::milliseconds(1);
  }
  if (config_.reconnect_max < config_.reconnect_initial) {
    config_.reconnect_max = config_.reconnect_initial;
  }
  if (config_.connect_timeout.count() <= 0) {
    config_.connect_timeout = std::chrono::milliseconds(2000);
  }
  max_pending_recovery_bytes_ =
      (config_.retransmit_capacity + 1) * static_cast<std::size_t>(kMaxWireSize);
}

Publisher::~Publisher() {
  if (gateway_fd_ >= 0) {
    ::close(gateway_fd_);
  }
  for (RecoveryClient& client : recovery_clients_) {
    if (client.fd >= 0) {
      ::close(client.fd);
    }
  }
  if (recovery_listener_ >= 0) {
    ::close(recovery_listener_);
  }
  udp_.close();
}

bool Publisher::open(std::string& error) {
  if (opened_) {
    return true;
  }
  if (config_.udp_destinations.empty() && !config_.multicast_group.has_value()) {
    error = "at least one UDP destination is required";
    return false;
  }
  if (!udp_.open(config_.udp_destinations, config_.multicast_group, error)) {
    return false;
  }
  if (!resolve_endpoint(config_.gateway, SOCK_STREAM, gateway_address_, gateway_address_length_,
                        error)) {
    udp_.close();
    return false;
  }

  const Endpoint bind_endpoint{config_.recovery_bind_host, config_.recovery_port};
  recovery_listener_ = create_tcp_listener(bind_endpoint, error);
  if (recovery_listener_ < 0) {
    udp_.close();
    return false;
  }
  recovery_port_ = bound_port(recovery_listener_);

  next_connect_ = std::chrono::steady_clock::now();
  backoff_ = config_.reconnect_initial;
  opened_ = true;
  return true;
}

void Publisher::poll_once(std::chrono::milliseconds timeout) {
  if (!opened_) {
    return;
  }

  const auto now = std::chrono::steady_clock::now();
  if (gateway_state_ == GatewayState::Disconnected && now >= next_connect_) {
    attempt_gateway_connect();
  } else if (gateway_state_ == GatewayState::Connecting && now >= connect_deadline_) {
    drop_gateway();
  }

  std::vector<pollfd> fds;
  fds.reserve(2 + recovery_clients_.size());
  fds.push_back(pollfd{recovery_listener_, POLLIN, 0});
  const std::size_t polled_clients = recovery_clients_.size();
  const bool has_gateway = gateway_fd_ >= 0;
  if (has_gateway) {
    const short events = gateway_state_ == GatewayState::Connecting ? POLLOUT : POLLIN;
    fds.push_back(pollfd{gateway_fd_, events, 0});
  }
  const std::size_t client_base = fds.size();
  for (const RecoveryClient& client : recovery_clients_) {
    const short events =
        client.output_offset < client.output.size() ? static_cast<short>(POLLIN | POLLOUT) : POLLIN;
    fds.push_back(pollfd{client.fd, events, 0});
  }

  const int wait_ms = poll_timeout_ms(timeout);
  const int ready = ::poll(fds.data(), static_cast<nfds_t>(fds.size()), wait_ms);
  if (ready < 0) {
    return;  // EINTR and other transient poll failures: try again next time
  }

  if (fds[0].revents != 0) {
    accept_recovery_clients();
  }

  if (has_gateway && fds[1].revents != 0) {
    if (gateway_state_ == GatewayState::Connecting) {
      finish_gateway_connect();
    } else {
      read_gateway();
    }
  }

  for (std::size_t i = polled_clients; i-- > 0;) {
    const short revents = fds[client_base + i].revents;
    if (revents == 0) {
      continue;
    }
    if (!service_recovery_client(i, revents)) {
      ::close(recovery_clients_[i].fd);
      recovery_clients_.erase(recovery_clients_.begin() + static_cast<std::ptrdiff_t>(i));
    }
  }
}

void Publisher::run() {
  while (!stop_.load(std::memory_order_relaxed)) {
    poll_once(std::chrono::milliseconds(200));
  }
}

int Publisher::poll_timeout_ms(std::chrono::milliseconds requested) const {
  const auto now = std::chrono::steady_clock::now();
  using Duration = std::chrono::steady_clock::duration;
  Duration limit =
      requested.count() < 0 ? Duration::max() / 2 : std::chrono::duration_cast<Duration>(requested);
  if (gateway_state_ == GatewayState::Disconnected) {
    limit = std::min(limit, next_connect_ - now);
  } else if (gateway_state_ == GatewayState::Connecting) {
    limit = std::min(limit, connect_deadline_ - now);
  }
  const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(limit).count();
  if (milliseconds <= 0) {
    return 0;
  }
  return static_cast<int>(std::min<std::int64_t>(milliseconds, std::numeric_limits<int>::max()));
}

void Publisher::attempt_gateway_connect() {
  std::string error;
  const int fd = create_tcp_socket(gateway_address_, error);
  if (fd < 0) {
    schedule_reconnect();
    return;
  }

  const auto* address = reinterpret_cast<const sockaddr*>(&gateway_address_);
  if (::connect(fd, address, gateway_address_length_) == 0) {
    gateway_fd_ = fd;
    gateway_state_ = GatewayState::Connected;
    gateway_framer_.reset();
    backoff_ = config_.reconnect_initial;
    ++stats_.gateway_connections;
    log("connected to gateway", config_.gateway);
    return;
  }
  if (errno == EINPROGRESS) {
    gateway_fd_ = fd;
    gateway_state_ = GatewayState::Connecting;
    connect_deadline_ = std::chrono::steady_clock::now() + config_.connect_timeout;
    return;
  }

  ::close(fd);
  schedule_reconnect();
}

void Publisher::finish_gateway_connect() {
  int error = 0;
  socklen_t length = sizeof(error);
  const bool connected = gateway_fd_ >= 0 &&
                         ::getsockopt(gateway_fd_, SOL_SOCKET, SO_ERROR, &error, &length) == 0 &&
                         error == 0;
  if (!connected) {
    if (gateway_fd_ >= 0) {
      ::close(gateway_fd_);
      gateway_fd_ = -1;
    }
    gateway_state_ = GatewayState::Disconnected;
    schedule_reconnect();
    return;
  }
  gateway_state_ = GatewayState::Connected;
  gateway_framer_.reset();
  backoff_ = config_.reconnect_initial;
  ++stats_.gateway_connections;
  log("connected to gateway", config_.gateway);
}

void Publisher::drop_gateway() {
  if (gateway_state_ == GatewayState::Disconnected && gateway_fd_ < 0) {
    return;
  }
  flush_datagrams();
  if (gateway_fd_ >= 0) {
    ::close(gateway_fd_);
    gateway_fd_ = -1;
  }
  const bool was_connected = gateway_state_ == GatewayState::Connected;
  gateway_state_ = GatewayState::Disconnected;
  gateway_framer_.reset();
  if (was_connected) {
    ++stats_.gateway_disconnects;
    log("lost the gateway; reconnecting", config_.gateway);
  }
  schedule_reconnect();
}

void Publisher::schedule_reconnect() {
  next_connect_ = std::chrono::steady_clock::now() + backoff_;
  backoff_ = std::min(backoff_ * 2, config_.reconnect_max);
}

bool Publisher::on_frame(std::span<const std::byte> frame) {
  if (recovery_target_ != nullptr) {
    return on_recovery_frame(*recovery_target_, frame);
  }
  return on_gateway_frame(frame);
}

void Publisher::read_gateway() {
  for (;;) {
    const ssize_t received = ::recv(gateway_fd_, read_buffer_.data(), read_buffer_.size(), 0);
    if (received > 0) {
      const auto span =
          std::span<const std::byte>(read_buffer_.data(), static_cast<std::size_t>(received));
      const PushResult result = gateway_framer_.push(span, *this);
      if (result.result == FramerResult::Error) {
        ++stats_.protocol_errors;
        drop_gateway();
        return;
      }
      if (result.result == FramerResult::Stopped) {
        drop_gateway();
        return;
      }
      continue;
    }
    if (received == 0) {
      drop_gateway();
      return;
    }
    if (errno == EINTR) {
      continue;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      break;
    }
    drop_gateway();
    return;
  }
  flush_datagrams();
}

bool Publisher::on_gateway_frame(std::span<const std::byte> frame) {
  Header header;
  if (decode_header(frame, header) != DecodeStatus::Ok) {
    ++stats_.protocol_errors;
    return true;
  }

  // A heartbeat repeats the sequence number of the last real event, so it is
  // forwarded like any other frame but never enters the retransmit ring: the
  // ring already holds the event it points at, and storing a duplicate (or, if
  // that event has aged out, a stale number) would corrupt the recovery view
  // of the stream.
  if (header.type == MessageType::Heartbeat) {
    ++stats_.heartbeats_forwarded;
  } else {
    store_.append(header.seq, frame);
    ++stats_.messages_published;
  }

  if (!packer_.add(frame)) {
    flush_datagrams();
    if (!packer_.add(frame)) {
      ++stats_.oversized_frames;
    }
  }
  return true;
}

void Publisher::flush_datagrams() {
  if (packer_.empty()) {
    return;
  }
  stats_.send_failures += udp_.send(packer_.data());
  ++stats_.datagrams_sent;
  packer_.clear();
}

void Publisher::accept_recovery_clients() {
  for (;;) {
    sockaddr_storage address{};
    socklen_t length = sizeof(address);
    const int fd = ::accept(recovery_listener_, reinterpret_cast<sockaddr*>(&address), &length);
    if (fd < 0) {
      if (errno == EINTR) {
        continue;
      }
      return;
    }
    if (!make_nonblocking(fd)) {
      ::close(fd);
      continue;
    }
    if (recovery_clients_.size() >= kMaxRecoveryClients) {
      ::close(fd);
      continue;
    }
    suppress_sigpipe(fd);
    recovery_clients_.emplace_back();
    recovery_clients_.back().fd = fd;
  }
}

bool Publisher::service_recovery_client(std::size_t index, short revents) {
  RecoveryClient& client = recovery_clients_[index];

  if ((revents & POLLOUT) != 0 && !flush_recovery_output(client)) {
    return false;
  }

  if ((revents & POLLIN) != 0) {
    for (;;) {
      const ssize_t received = ::recv(client.fd, read_buffer_.data(), read_buffer_.size(), 0);
      if (received > 0) {
        recovery_target_ = &client;
        const auto span =
            std::span<const std::byte>(read_buffer_.data(), static_cast<std::size_t>(received));
        const PushResult result = client.framer.push(span, *this);
        recovery_target_ = nullptr;
        if (result.result == FramerResult::Error) {
          ++stats_.protocol_errors;
          return false;
        }
        if (result.result == FramerResult::Stopped) {
          return false;
        }
        continue;
      }
      if (received == 0) {
        return false;
      }
      if (errno == EINTR) {
        continue;
      }
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        break;
      }
      return false;
    }
  }

  if ((revents & (POLLERR | POLLNVAL)) != 0) {
    return false;
  }
  if ((revents & POLLHUP) != 0 && (revents & POLLIN) == 0) {
    return false;
  }
  return true;
}

bool Publisher::flush_recovery_output(RecoveryClient& client) {
  while (client.output_offset < client.output.size()) {
    const std::byte* data = client.output.data() + client.output_offset;
    const std::size_t remaining = client.output.size() - client.output_offset;
    const ssize_t written = ::send(client.fd, data, remaining, kSendFlags);
    if (written > 0) {
      client.output_offset += static_cast<std::size_t>(written);
      continue;
    }
    if (written < 0 && errno == EINTR) {
      continue;
    }
    if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      return true;
    }
    return false;
  }
  client.output.clear();
  client.output_offset = 0;
  return true;
}

bool Publisher::queue_recovery_bytes(RecoveryClient& client, const std::byte* data,
                                     std::size_t size) {
  const std::size_t pending = client.output.size() - client.output_offset;
  if (pending + size > max_pending_recovery_bytes_) {
    return false;  // the subscriber is not draining; do not grow without bound
  }
  client.output.insert(client.output.end(), data, data + size);
  return true;
}

bool Publisher::on_recovery_frame(RecoveryClient& client, std::span<const std::byte> frame) {
  RecoveryRequest request;
  Seq seq = 0;
  if (decode(frame, request, seq) != DecodeStatus::Ok) {
    ++stats_.protocol_errors;
    ++stats_.rejected_recovery_requests;
    return false;
  }
  ++stats_.recovery_requests;

  if (request.from_seq == 0 || request.to_seq < request.from_seq) {
    ++stats_.rejected_recovery_requests;
    return false;
  }
  // A request that starts after the oldest held frame can never be answered
  // with more than the store holds, so a span beyond its capacity is malformed.
  // A request that starts before the store is a real gap and gets a
  // SequenceReset instead of being rejected.
  if (!store_.empty() && request.from_seq >= store_.oldest() &&
      request.to_seq - request.from_seq >= static_cast<Seq>(store_.capacity())) {
    ++stats_.rejected_recovery_requests;
    return false;
  }

  const RecoveryPlan plan = store_.plan(request.from_seq, request.to_seq);
  switch (plan.kind) {
    case RecoveryPlan::Kind::Nothing:
      return true;
    case RecoveryPlan::Kind::Reset: {
      std::array<std::byte, SequenceReset::kWireSize> buffer{};
      const std::size_t written = encode(SequenceReset{plan.next_seq}, plan.next_seq, buffer);
      if (written == 0) {
        return false;
      }
      ++stats_.sequence_resets;
      return queue_recovery_bytes(client, buffer.data(), written);
    }
    case RecoveryPlan::Kind::Replay: {
      for (Seq current = plan.from; current <= plan.to; ++current) {
        const auto stored = store_.frame(current);
        if (!stored.has_value()) {
          continue;  // a gap in the held range: skip the missing sequence
        }
        // Replayed frames keep their market-data sequence in the header; the
        // subscriber uses it to slot them into the gap it is filling.
        if (!queue_recovery_bytes(client, stored->data(), stored->size())) {
          return false;
        }
      }
      return true;
    }
  }
  return false;
}

void Publisher::log(const char* text, const Endpoint& endpoint) {
  if (config_.log_connections) {
    std::fprintf(stderr, "lob_publisher: %s (%s)\n", text, to_string(endpoint).c_str());
  }
}

}  // namespace lob::publisher
