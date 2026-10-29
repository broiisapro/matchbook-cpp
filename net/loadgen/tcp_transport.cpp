#include "net/loadgen/tcp_transport.hpp"

#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

namespace lob::loadgen {
namespace {

#ifdef MSG_NOSIGNAL
constexpr int kSendFlags = MSG_NOSIGNAL;
#else
constexpr int kSendFlags = 0;
#endif

bool set_no_sigpipe(int fd) {
#ifdef SO_NOSIGPIPE
  int on = 1;
  return setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on)) == 0;
#else
  (void)fd;
  return true;
#endif
}

void set_tcp_nodelay(int fd) {
  int on = 1;
  (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
}

std::string errno_text(const char* what) { return std::string(what) + ": " + std::strerror(errno); }

}  // namespace

TcpTransport::TcpTransport(int fd) : fd_(fd) {}

TcpTransport::~TcpTransport() {
  if (fd_ >= 0) {
    ::close(fd_);
  }
}

std::unique_ptr<TcpTransport> TcpTransport::connect(const Endpoint& endpoint, std::string& error) {
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;

  addrinfo* addresses = nullptr;
  const std::string port = std::to_string(endpoint.port);
  const int resolved = ::getaddrinfo(endpoint.host.c_str(), port.c_str(), &hints, &addresses);
  if (resolved != 0) {
    error = "resolve " + endpoint.host + ":" + port + ": " + ::gai_strerror(resolved);
    return nullptr;
  }

  int fd = -1;
  std::string last_error;
  for (addrinfo* address = addresses; address != nullptr; address = address->ai_next) {
    fd = ::socket(address->ai_family, address->ai_socktype, address->ai_protocol);
    if (fd < 0) {
      last_error = errno_text("socket");
      continue;
    }
    if (!set_no_sigpipe(fd)) {
      last_error = errno_text("setsockopt(SO_NOSIGPIPE)");
      ::close(fd);
      fd = -1;
      continue;
    }
    if (::connect(fd, address->ai_addr, address->ai_addrlen) == 0) {
      break;
    }
    last_error = errno_text("connect");
    ::close(fd);
    fd = -1;
  }
  ::freeaddrinfo(addresses);

  if (fd < 0) {
    error = "connect " + endpoint.host + ":" + port + ": " +
            (last_error.empty() ? "no usable address" : last_error);
    return nullptr;
  }
  set_tcp_nodelay(fd);
  return std::unique_ptr<TcpTransport>(new TcpTransport(fd));
}

bool TcpTransport::send(std::span<const std::byte> frame) {
  std::size_t offset = 0;
  while (offset < frame.size()) {
    const ssize_t written = ::send(fd_, frame.data() + offset, frame.size() - offset, kSendFlags);
    if (written < 0) {
      if (errno == EINTR) {
        continue;
      }
      error_ = errno_text("send");
      return false;
    }
    if (written == 0) {
      error_ = "send: connection closed";
      return false;
    }
    offset += static_cast<std::size_t>(written);
  }
  return true;
}

bool TcpTransport::receive(lob::net::FrameSink& sink) {
  ssize_t count = 0;
  do {
    count = ::recv(fd_, buffer_.data(), buffer_.size(), 0);
  } while (count < 0 && errno == EINTR);

  if (count == 0) {
    error_ = "connection closed by the gateway";
    return false;
  }
  if (count < 0) {
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      return true;
    }
    error_ = errno_text("recv");
    return false;
  }

  const auto bytes = std::span<const std::byte>(buffer_.data(), static_cast<std::size_t>(count));
  const lob::net::PushResult result = framer_.push(bytes, sink);
  if (result.result == lob::net::FramerResult::Error) {
    error_ = "protocol error: " + std::string(lob::net::to_string(framer_.error()));
    return false;
  }
  return true;
}

}  // namespace lob::loadgen
