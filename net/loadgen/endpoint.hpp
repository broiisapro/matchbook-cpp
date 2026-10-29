#pragma once

// host:port parsing shared by the load-gen CLI and the TCP transport. The
// gateway's default port is 9000 (decision C54); a bare host is accepted and
// filled in with that port.

#include <cstdint>
#include <string>
#include <string_view>

namespace lob::loadgen {

inline constexpr std::uint16_t kDefaultGatewayPort = 9000;
inline constexpr std::uint16_t kDefaultRecoveryPort = 9002;

struct Endpoint {
  std::string host;
  std::uint16_t port = kDefaultGatewayPort;

  friend bool operator==(const Endpoint&, const Endpoint&) = default;
};

/// Parses "host:port" or "host". IPv6 literals must be bracketed ("[::1]:9000")
/// so the colon inside the address is unambiguous. Returns false with a
/// description in `error` for a missing host, a non-numeric port or a port
/// outside 1..65535.
bool parse_endpoint(std::string_view text, Endpoint& out, std::string& error);

/// Same as parse_endpoint, with `default_port` used when the text has no port.
bool parse_endpoint_with_default(std::string_view text, std::uint16_t default_port, Endpoint& out,
                                 std::string& error);

}  // namespace lob::loadgen
