#include "net/publisher/publisher.hpp"

#include <poll.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <limits>
#include <utility>

#include "net/protocol/codec.hpp"
#include "net/publisher/socket.hpp"

namespace lob::publisher {

using net::decode_header;
using net::DecodeStatus;
using net::FramerResult;
using net::Header;
using net::PushResult;

namespace {

constexpr std::size_t kReadBufferBytes = 64 * 1024;

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
}

Publisher::~Publisher() {
  if (gateway_fd_ >= 0) {
    ::close(gateway_fd_);
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
  fds.reserve(1);
  const bool has_gateway = gateway_fd_ >= 0;
  if (has_gateway) {
    const short events = gateway_state_ == GatewayState::Connecting ? POLLOUT : POLLIN;
    fds.push_back(pollfd{gateway_fd_, events, 0});
  }

  const int wait_ms = poll_timeout_ms(timeout);
  const int ready =
      ::poll(fds.empty() ? nullptr : fds.data(), static_cast<nfds_t>(fds.size()), wait_ms);
  if (ready < 0) {
    return;  // EINTR and other transient poll failures: try again next time
  }

  if (has_gateway && fds[0].revents != 0) {
    if (gateway_state_ == GatewayState::Connecting) {
      finish_gateway_connect();
    } else {
      read_gateway();
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

bool Publisher::on_frame(std::span<const std::byte> frame) { return on_gateway_frame(frame); }

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

  store_.append(header.seq, frame);
  ++stats_.messages_published;

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

void Publisher::log(const char* text, const Endpoint& endpoint) {
  if (config_.log_connections) {
    std::fprintf(stderr, "lob_publisher: %s (%s)\n", text, to_string(endpoint).c_str());
  }
}

}  // namespace lob::publisher
