#pragma once

// Sends one datagram to every configured destination. Each destination keeps
// its own UDP socket and resolved address, so IPv4 and IPv6 destinations can be
// mixed and a multicast group can be added next to unicast receivers.

#include <sys/socket.h>

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "net/publisher/endpoint.hpp"

namespace lob::publisher {

class UdpSender {
 public:
  UdpSender() = default;
  ~UdpSender();

  UdpSender(const UdpSender&) = delete;
  UdpSender& operator=(const UdpSender&) = delete;

  /// Resolves and opens one socket per destination. Returns false, fills
  /// `error` and closes anything already open on failure.
  bool open(const std::vector<Endpoint>& destinations,
            const std::optional<Endpoint>& multicast_group, std::string& error);

  /// Sends `datagram` to every destination. Returns the number of destinations
  /// that could not be served; a single failed send does not stop the others.
  std::size_t send(std::span<const std::byte> datagram);

  std::size_t destination_count() const { return destinations_.size(); }

  void close();

 private:
  struct Destination {
    int fd = -1;
    sockaddr_storage address{};
    socklen_t length = 0;
  };

  bool add_destination(const Endpoint& endpoint, std::string& error);

  std::vector<Destination> destinations_;
};

}  // namespace lob::publisher
