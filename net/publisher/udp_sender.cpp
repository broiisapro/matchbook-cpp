#include "net/publisher/udp_sender.hpp"

#include <arpa/inet.h>
#include <unistd.h>

#include <cerrno>

#include "net/publisher/socket.hpp"

namespace lob::publisher {
namespace {

bool is_multicast(const sockaddr_storage& address) {
  if (address.ss_family == AF_INET) {
    const auto* ipv4 = reinterpret_cast<const sockaddr_in*>(&address);
    return IN_MULTICAST(ntohl(ipv4->sin_addr.s_addr)) != 0;
  }
  if (address.ss_family == AF_INET6) {
    const auto* ipv6 = reinterpret_cast<const sockaddr_in6*>(&address);
    return IN6_IS_ADDR_MULTICAST(&ipv6->sin6_addr) != 0;
  }
  return false;
}

}  // namespace

UdpSender::~UdpSender() { close(); }

bool UdpSender::open(const std::vector<Endpoint>& destinations,
                     const std::optional<Endpoint>& multicast_group, std::string& error) {
  close();
  for (const Endpoint& destination : destinations) {
    if (!add_destination(destination, error)) {
      close();
      return false;
    }
  }
  if (multicast_group.has_value() && !add_destination(*multicast_group, error)) {
    close();
    return false;
  }
  return true;
}

bool UdpSender::add_destination(const Endpoint& endpoint, std::string& error) {
  Destination destination;
  if (!resolve_endpoint(endpoint, SOCK_DGRAM, destination.address, destination.length, error)) {
    return false;
  }

  const auto* family_address = reinterpret_cast<const sockaddr*>(&destination.address);
  destination.fd = ::socket(family_address->sa_family, SOCK_DGRAM, IPPROTO_UDP);
  if (destination.fd < 0) {
    error =
        "cannot create a UDP socket for " + to_string(endpoint) + ": " + socket_error_text(errno);
    return false;
  }

  if (is_multicast(destination.address)) {
    const unsigned char ttl = 1;  // stay on the local network, like the demo
    ::setsockopt(destination.fd,
                 destination.address.ss_family == AF_INET ? IPPROTO_IP : IPPROTO_IPV6,
                 destination.address.ss_family == AF_INET ? IP_MULTICAST_TTL : IPV6_MULTICAST_HOPS,
                 &ttl, sizeof(ttl));
  }

  destinations_.push_back(destination);
  return true;
}

std::size_t UdpSender::send(std::span<const std::byte> datagram) {
  std::size_t failures = 0;
  for (const Destination& destination : destinations_) {
    const ssize_t written =
        ::sendto(destination.fd, datagram.data(), datagram.size(), 0,
                 reinterpret_cast<const sockaddr*>(&destination.address), destination.length);
    if (written < 0 || static_cast<std::size_t>(written) != datagram.size()) {
      ++failures;
    }
  }
  return failures;
}

void UdpSender::close() {
  for (Destination& destination : destinations_) {
    if (destination.fd >= 0) {
      ::close(destination.fd);
      destination.fd = -1;
    }
  }
  destinations_.clear();
}

}  // namespace lob::publisher
