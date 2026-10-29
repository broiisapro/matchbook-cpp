#pragma once

// Host/port pair used by every publisher socket, plus the little text parser
// the command line shares with the tests. The parser understands "host:port"
// and the bracketed IPv6 form "[addr]:port".

#include <cstdint>
#include <string>
#include <string_view>

namespace lob::publisher {

struct Endpoint {
  std::string host;
  std::uint16_t port = 0;

  friend bool operator==(const Endpoint&, const Endpoint&) = default;
};

/// Parses "host:port" or "[host]:port". The host must not be empty and the port
/// must be in [1, 65535]. Returns false and leaves `out` untouched on failure.
bool parse_endpoint(std::string_view text, Endpoint& out);

/// Renders an endpoint in the form parse_endpoint accepts.
std::string to_string(const Endpoint& endpoint);

}  // namespace lob::publisher
