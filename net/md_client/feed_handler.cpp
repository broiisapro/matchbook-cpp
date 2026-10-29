#include "net/md_client/feed_handler.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <limits>
#include <string_view>
#include <utility>

#include "net/protocol/codec.hpp"

namespace lob::md {
namespace {

using Clock = std::chrono::steady_clock;

int milliseconds_until(Clock::time_point deadline) {
  const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now());
  if (left.count() <= 0) {
    return 0;
  }
  return static_cast<int>(std::min<std::int64_t>(left.count(), std::numeric_limits<int>::max()));
}

int wait_for(int fd, short events, int timeout_ms) {
  pollfd entry{};
  entry.fd = fd;
  entry.events = events;
  int result = 0;
  do {
    result = ::poll(&entry, 1, timeout_ms);
  } while (result < 0 && errno == EINTR);
  return result;
}

void set_non_blocking(int fd) {
  const int flags = ::fcntl(fd, F_GETFL, 0);
  if (flags >= 0) {
    ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  }
}

void set_no_sigpipe(int fd) {
#ifdef SO_NOSIGPIPE
  const int on = 1;
  ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
#else
  (void)fd;
#endif
}

std::string socket_error(std::string_view prefix) {
  return std::string(prefix) + ": " + std::strerror(errno);
}

/// Resolves a numeric or named IPv4 host. Phase 3 publishes over unicast IPv4
/// (docs/architecture.md §14); IPv6 is not part of the demo topology yet.
bool resolve_ipv4(const std::string& host, std::uint16_t port, int socktype, bool passive,
                  addrinfo** result, std::string& error) {
  addrinfo hints{};
  hints.ai_family = AF_INET;
  hints.ai_socktype = socktype;
  hints.ai_flags = passive ? AI_PASSIVE : 0;
  const std::string service = std::to_string(port);
  const char* node = host.empty() ? nullptr : host.c_str();
  const int rc = ::getaddrinfo(node, service.c_str(), &hints, result);
  if (rc != 0) {
    error =
        std::string("cannot resolve '") + (host.empty() ? "*" : host) + "': " + ::gai_strerror(rc);
    return false;
  }
  return true;
}

int connect_tcp(const std::string& host, std::uint16_t port, int timeout_ms) {
  addrinfo* addresses = nullptr;
  std::string error;
  if (!resolve_ipv4(host, port, SOCK_STREAM, false, &addresses, error)) {
    return -1;
  }
  int connected = -1;
  for (addrinfo* candidate = addresses; candidate != nullptr; candidate = candidate->ai_next) {
    const int fd = ::socket(candidate->ai_family, candidate->ai_socktype, candidate->ai_protocol);
    if (fd < 0) {
      continue;
    }
    set_no_sigpipe(fd);
    set_non_blocking(fd);
    int rc = ::connect(fd, candidate->ai_addr, candidate->ai_addrlen);
    if (rc < 0 && errno == EINPROGRESS) {
      rc = wait_for(fd, POLLOUT, timeout_ms);
      if (rc > 0) {
        int status = 0;
        socklen_t length = sizeof(status);
        if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &status, &length) != 0 || status != 0) {
          rc = -1;
        } else {
          rc = 1;
        }
      }
    }
    if (rc > 0) {
      connected = fd;
      break;
    }
    ::close(fd);
  }
  ::freeaddrinfo(addresses);
  return connected;
}

int send_flags() {
#ifdef MSG_NOSIGNAL
  return MSG_NOSIGNAL;
#else
  return 0;
#endif
}

bool send_all(int fd, std::span<const std::byte> bytes, int timeout_ms) {
  const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
  std::size_t sent = 0;
  while (sent < bytes.size()) {
    const ssize_t n = ::send(fd, bytes.data() + sent, bytes.size() - sent, send_flags());
    if (n > 0) {
      sent += static_cast<std::size_t>(n);
      continue;
    }
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
      const int remaining = milliseconds_until(deadline);
      if (remaining <= 0 || wait_for(fd, POLLOUT, remaining) <= 0) {
        return false;
      }
      continue;
    }
    return false;
  }
  return true;
}

}  // namespace

void LevelBook::apply(const net::BookUpdate& update) {
  std::map<Price, Qty>& levels = update.side == Side::Buy ? bids_ : asks_;
  if (update.qty == 0) {
    levels.erase(update.price);
    return;
  }
  levels[update.price] = update.qty;
}

void LevelBook::clear() {
  bids_.clear();
  asks_.clear();
}

std::optional<Qty> LevelBook::quantity_at(Side side, Price price) const {
  const std::map<Price, Qty>& levels = side == Side::Buy ? bids_ : asks_;
  const auto found = levels.find(price);
  if (found == levels.end()) {
    return std::nullopt;
  }
  return found->second;
}

std::vector<Level> LevelBook::top(Side side, std::size_t count) const {
  std::vector<Level> result;
  result.reserve(std::min(count, side == Side::Buy ? bids_.size() : asks_.size()));
  if (side == Side::Buy) {
    for (auto it = bids_.rbegin(); it != bids_.rend() && result.size() < count; ++it) {
      result.push_back(Level{it->first, it->second});
    }
  } else {
    for (auto it = asks_.begin(); it != asks_.end() && result.size() < count; ++it) {
      result.push_back(Level{it->first, it->second});
    }
  }
  return result;
}

FeedHandler::FeedHandler(FeedHandlerConfig config) : config_(std::move(config)) {
  if (config_.max_recovery_range == 0) {
    config_.max_recovery_range = 1;
  }
}

FeedHandler::~FeedHandler() { close(); }

bool FeedHandler::open(std::string& error) {
  if (fd_ >= 0) {
    return true;
  }
  addrinfo* addresses = nullptr;
  if (!resolve_ipv4(config_.bind_address, config_.udp_port, SOCK_DGRAM, true, &addresses, error)) {
    return false;
  }
  int fd = -1;
  for (addrinfo* candidate = addresses; candidate != nullptr; candidate = candidate->ai_next) {
    fd = ::socket(candidate->ai_family, candidate->ai_socktype, candidate->ai_protocol);
    if (fd < 0) {
      continue;
    }
    const int reuse = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    if (::bind(fd, candidate->ai_addr, candidate->ai_addrlen) == 0) {
      break;
    }
    ::close(fd);
    fd = -1;
  }
  ::freeaddrinfo(addresses);
  if (fd < 0) {
    error = socket_error("cannot bind the UDP feed socket");
    return false;
  }
  set_non_blocking(fd);

  sockaddr_in bound{};
  socklen_t length = sizeof(bound);
  if (::getsockname(fd, reinterpret_cast<sockaddr*>(&bound), &length) != 0) {
    error = socket_error("cannot read the bound UDP port");
    ::close(fd);
    return false;
  }
  fd_ = fd;
  bound_port_ = ntohs(bound.sin_port);
  return true;
}

void FeedHandler::close() {
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
}

std::size_t FeedHandler::poll(int timeout_ms) {
  if (fd_ < 0) {
    return 0;
  }
  if (wait_for(fd_, POLLIN, std::max(0, timeout_ms)) <= 0) {
    return 0;
  }
  std::array<std::byte, 4096> buffer{};
  const ssize_t received = ::recv(fd_, buffer.data(), buffer.size(), 0);
  if (received <= 0) {
    return 0;
  }
  process_datagram(std::span<const std::byte>(buffer.data(), static_cast<std::size_t>(received)));
  return 1;
}

void FeedHandler::process_datagram(std::span<const std::byte> datagram) {
  ++counters_.datagrams;
  if (recovery_retry_in_ > 0) {
    --recovery_retry_in_;
  }

  bool malformed = false;
  std::size_t offset = 0;
  while (offset < datagram.size()) {
    if (datagram.size() - offset < net::kHeaderSize) {
      malformed = true;
      break;
    }
    net::Header header;
    if (net::decode_header(datagram.subspan(offset), header) != net::DecodeStatus::Ok) {
      malformed = true;
      break;
    }
    const std::size_t frame_size = net::kHeaderSize + header.length;
    if (datagram.size() - offset < frame_size) {
      malformed = true;
      break;
    }
    ++counters_.received;
    handle_frame(datagram.subspan(offset, frame_size));
    offset += frame_size;
  }
  if (malformed) {
    ++counters_.malformed;
  }
  maybe_recover();
}

void FeedHandler::handle_frame(std::span<const std::byte> frame) {
  net::Header header;
  if (net::decode_header(frame, header) != net::DecodeStatus::Ok) {
    ++counters_.malformed;
    return;
  }
  switch (header.type) {
    case net::MessageType::BookUpdate: {
      net::BookUpdate update;
      net::Seq seq = 0;
      if (net::decode(frame, update, seq) != net::DecodeStatus::Ok) {
        ++counters_.malformed;
        return;
      }
      accept_sequenced(header.seq, frame);
      return;
    }
    case net::MessageType::Trade: {
      net::Trade trade;
      net::Seq seq = 0;
      if (net::decode(frame, trade, seq) != net::DecodeStatus::Ok) {
        ++counters_.malformed;
        return;
      }
      accept_sequenced(header.seq, frame);
      return;
    }
    case net::MessageType::SequenceReset: {
      net::SequenceReset reset;
      net::Seq seq = 0;
      if (net::decode(frame, reset, seq) != net::DecodeStatus::Ok) {
        ++counters_.malformed;
        return;
      }
      if (reset.next_seq > expected_seq_) {
        ++counters_.resets;
        apply_reset(reset.next_seq);
      }
      return;
    }
    case net::MessageType::Heartbeat: {
      net::Heartbeat heartbeat;
      net::Seq seq = 0;
      if (net::decode(frame, heartbeat, seq) != net::DecodeStatus::Ok) {
        ++counters_.malformed;
        return;
      }
      ++counters_.heartbeats;
      // A heartbeat repeats the last published sequence number; it carries no
      // event, so it never touches the book. Above the last applied number it
      // proves that everything between was published and did not arrive, which
      // is exactly the trailing gap a lost final datagram leaves behind.
      const net::Seq carried = header.seq;
      if (carried > highest_known_seq_) {
        highest_known_seq_ = carried;
      }
      if (carried >= expected_seq_ && !gap_open_) {
        gap_open_ = true;
        ++counters_.gaps_detected;
      }
      return;
    }
    default:
      // The event stream carries BookUpdate, Trade, SequenceReset and the idle
      // Heartbeat only.
      ++counters_.malformed;
      return;
  }
}

void FeedHandler::accept_sequenced(net::Seq seq, std::span<const std::byte> frame) {
  if (seq > highest_known_seq_) {
    highest_known_seq_ = seq;
  }
  if (seq < expected_seq_) {
    ++counters_.duplicates;
    return;
  }
  if (seq == expected_seq_) {
    apply_frame_now(frame);
    ++expected_seq_;
    drain_buffered();
    return;
  }
  if (buffered_.size() >= config_.max_buffered_messages) {
    ++counters_.dropped;
    return;
  }
  if (!buffered_.emplace(seq, std::vector<std::byte>(frame.begin(), frame.end())).second) {
    ++counters_.duplicates;
    return;
  }
  if (!gap_open_) {
    gap_open_ = true;
    ++counters_.gaps_detected;
  }
}

void FeedHandler::apply_frame_now(std::span<const std::byte> frame) {
  net::Header header;
  if (net::decode_header(frame, header) != net::DecodeStatus::Ok) {
    ++counters_.malformed;
    return;
  }
  if (header.type == net::MessageType::BookUpdate) {
    net::BookUpdate update;
    net::Seq seq = 0;
    if (net::decode(frame, update, seq) != net::DecodeStatus::Ok) {
      ++counters_.malformed;
      return;
    }
    book_.apply(update);
  } else if (header.type == net::MessageType::Trade) {
    net::Trade trade;
    net::Seq seq = 0;
    if (net::decode(frame, trade, seq) != net::DecodeStatus::Ok) {
      ++counters_.malformed;
      return;
    }
    ++counters_.trades;
  }
}

void FeedHandler::apply_reset(net::Seq next_seq) {
  expected_seq_ = next_seq;
  // A reset names the next number the stream will use, so everything below it
  // is settled (delivered or permanently skipped) and cannot be a gap.
  const net::Seq settled = next_seq > 0 ? next_seq - 1 : 0;
  if (settled > highest_known_seq_) {
    highest_known_seq_ = settled;
  }
  drain_buffered();
}

void FeedHandler::drain_buffered() {
  while (true) {
    const auto found = buffered_.find(expected_seq_);
    if (found == buffered_.end()) {
      break;
    }
    std::vector<std::byte> frame = std::move(found->second);
    buffered_.erase(found);
    apply_frame_now(frame);
    ++expected_seq_;
  }
  // A SequenceReset can jump over frames that were buffered but never became
  // contiguous; those are stale and must not be applied later.
  while (!buffered_.empty() && buffered_.begin()->first < expected_seq_) {
    buffered_.erase(buffered_.begin());
  }
  // A heartbeat can leave a gap with nothing buffered (the missing events never
  // arrived). That hole stays open until the book catches up with everything
  // the stream has announced.
  if (expected_seq_ > highest_known()) {
    gap_open_ = false;
    recovery_retry_in_ = 0;
  }
}

net::Seq FeedHandler::highest_known() const {
  net::Seq highest = highest_known_seq_;
  if (!buffered_.empty() && buffered_.rbegin()->first > highest) {
    highest = buffered_.rbegin()->first;
  }
  return highest;
}

void FeedHandler::maybe_recover() {
  if (!config_.auto_recovery || !gap_open_ || recovery_retry_in_ > 0) {
    return;
  }

  bool applied = false;
  // A huge hole is recovered in bounded chunks, so one recovery response can
  // never be unbounded. The loop also bounds the synchronous work per
  // datagram. The hole may be empty of buffered frames when a heartbeat opened
  // it, which is why the bound comes from highest_known().
  for (int attempt = 0; attempt < 16; ++attempt) {
    const net::Seq from = expected_seq_;
    const net::Seq highest = highest_known();
    if (highest < from) {
      break;
    }
    const net::Seq widest = static_cast<net::Seq>(config_.max_recovery_range);
    const net::Seq to = highest - from >= widest ? from + widest - 1 : highest;
    if (!fetch_range(from, to)) {
      ++counters_.recovery_failures;
      recovery_retry_in_ = 64;
      return;
    }
    applied = true;
    drain_buffered();
    if (expected_seq_ <= to) {
      // The service answered but did not fill the range (partial response or
      // a reset that points into the hole). Retry after a few datagrams.
      recovery_retry_in_ = 64;
      break;
    }
    if (expected_seq_ > highest_known()) {
      break;
    }
  }

  if (applied && expected_seq_ > highest_known()) {
    ++counters_.gaps_recovered;
    gap_open_ = false;
  }
}

bool FeedHandler::fetch_range(net::Seq from, net::Seq to) {
  ++counters_.recovery_requests;
  const int fd =
      connect_tcp(config_.recovery_host, config_.recovery_port, config_.recovery_timeout_ms);
  if (fd < 0) {
    return false;
  }

  std::array<std::byte, net::kMaxWireSize> wire{};
  const net::RecoveryRequest request{from, to};
  const std::size_t size = net::encode(request, ++recovery_request_seq_, wire);
  if (size == 0 ||
      !send_all(fd, std::span<const std::byte>(wire.data(), size), config_.recovery_timeout_ms)) {
    ::close(fd);
    return false;
  }

  const auto deadline = Clock::now() + std::chrono::milliseconds(config_.recovery_timeout_ms);
  std::vector<std::byte> pending;
  std::array<std::byte, 4096> chunk{};
  bool received_any = false;
  bool done = false;

  while (!done) {
    std::size_t offset = 0;
    while (pending.size() - offset >= net::kHeaderSize) {
      net::Header header;
      if (net::decode_header(std::span<const std::byte>(pending).subspan(offset), header) !=
          net::DecodeStatus::Ok) {
        ++counters_.malformed;
        done = true;
        break;
      }
      const std::size_t frame_size = net::kHeaderSize + header.length;
      if (pending.size() - offset < frame_size) {
        break;
      }
      ++counters_.recovery_messages;
      handle_frame(std::span<const std::byte>(pending).subspan(offset, frame_size));
      received_any = true;
      offset += frame_size;
      if (expected_seq_ > to) {
        done = true;
        break;
      }
    }
    pending.erase(pending.begin(), pending.begin() + static_cast<std::ptrdiff_t>(offset));
    if (done) {
      break;
    }
    const int remaining = milliseconds_until(deadline);
    if (remaining <= 0 || wait_for(fd, POLLIN, remaining) <= 0) {
      break;
    }
    const ssize_t bytes = ::recv(fd, chunk.data(), chunk.size(), 0);
    if (bytes <= 0) {
      break;
    }
    pending.insert(pending.end(), chunk.begin(),
                   chunk.begin() + static_cast<std::ptrdiff_t>(bytes));
    if (pending.size() > 64 * 1024) {
      ++counters_.malformed;
      break;
    }
  }

  ::close(fd);
  return received_any;
}

}  // namespace lob::md
