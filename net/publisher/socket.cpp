#include "net/publisher/socket.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <string>

namespace lob::publisher {
namespace {

std::string gai_error_text(int code) { return std::string(gai_strerror(code)); }

}  // namespace

bool resolve_endpoint(const Endpoint& endpoint, int socket_type, sockaddr_storage& out,
                      socklen_t& out_length, std::string& error) {
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = socket_type;
  hints.ai_protocol = socket_type == SOCK_DGRAM ? IPPROTO_UDP : IPPROTO_TCP;

  addrinfo* results = nullptr;
  const std::string port = std::to_string(endpoint.port);
  const int rc = ::getaddrinfo(endpoint.host.c_str(), port.c_str(), &hints, &results);
  if (rc != 0) {
    error = "cannot resolve " + endpoint.host + ": " + gai_error_text(rc);
    return false;
  }
  if (results == nullptr) {
    error = "cannot resolve " + endpoint.host + ": no address";
    return false;
  }

  std::memcpy(&out, results->ai_addr, results->ai_addrlen);
  out_length = static_cast<socklen_t>(results->ai_addrlen);
  ::freeaddrinfo(results);
  return true;
}

int create_tcp_listener(const Endpoint& bind_endpoint, std::string& error) {
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_PASSIVE;

  const char* host = bind_endpoint.host.empty() ? nullptr : bind_endpoint.host.c_str();
  const std::string port = std::to_string(bind_endpoint.port);

  addrinfo* results = nullptr;
  const int rc = ::getaddrinfo(host, port.c_str(), &hints, &results);
  if (rc != 0) {
    error = "cannot resolve recovery bind address: " + gai_error_text(rc);
    return -1;
  }

  int fd = -1;
  for (addrinfo* entry = results; entry != nullptr; entry = entry->ai_next) {
    fd = ::socket(entry->ai_family, entry->ai_socktype, entry->ai_protocol);
    if (fd < 0) {
      continue;
    }
    const int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    if (::bind(fd, entry->ai_addr, entry->ai_addrlen) == 0 && ::listen(fd, 64) == 0) {
      break;
    }
    ::close(fd);
    fd = -1;
  }
  ::freeaddrinfo(results);

  if (fd < 0) {
    error = "cannot listen for recovery on " + to_string(bind_endpoint) + ": " +
            socket_error_text(errno);
    return -1;
  }
  if (!make_nonblocking(fd)) {
    error = "cannot make the recovery listener non-blocking: " + socket_error_text(errno);
    ::close(fd);
    return -1;
  }
  return fd;
}

int create_tcp_socket(const sockaddr_storage& address, std::string& error) {
  const auto* family_address = reinterpret_cast<const sockaddr*>(&address);
  const int fd = ::socket(family_address->sa_family, SOCK_STREAM, IPPROTO_TCP);
  if (fd < 0) {
    error = "cannot create a TCP socket: " + socket_error_text(errno);
    return -1;
  }
  if (!make_nonblocking(fd)) {
    error = "cannot make the TCP socket non-blocking: " + socket_error_text(errno);
    ::close(fd);
    return -1;
  }
  suppress_sigpipe(fd);
  return fd;
}

std::uint16_t bound_port(int fd) {
  sockaddr_storage address{};
  socklen_t length = sizeof(address);
  if (::getsockname(fd, reinterpret_cast<sockaddr*>(&address), &length) != 0) {
    return 0;
  }
  if (address.ss_family == AF_INET) {
    const auto* ipv4 = reinterpret_cast<const sockaddr_in*>(&address);
    return ntohs(ipv4->sin_port);
  }
  if (address.ss_family == AF_INET6) {
    const auto* ipv6 = reinterpret_cast<const sockaddr_in6*>(&address);
    return ntohs(ipv6->sin6_port);
  }
  return 0;
}

bool make_nonblocking(int fd) {
  const int flags = ::fcntl(fd, F_GETFL, 0);
  if (flags < 0) {
    return false;
  }
  return ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

void suppress_sigpipe(int fd) {
#ifdef SO_NOSIGPIPE
  const int one = 1;
  ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#else
  (void)fd;
#endif
}

std::string socket_error_text(int error) { return std::string(std::strerror(error)); }

}  // namespace lob::publisher
