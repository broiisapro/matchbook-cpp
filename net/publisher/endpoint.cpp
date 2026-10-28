#include "net/publisher/endpoint.hpp"

#include <charconv>
#include <string>

namespace lob::publisher {

bool parse_endpoint(std::string_view text, Endpoint& out) {
  if (text.empty()) {
    return false;
  }

  std::string_view host;
  std::string_view port_text;

  if (text.front() == '[') {
    const std::size_t close = text.find(']');
    if (close == std::string_view::npos || close + 1 >= text.size() || text[close + 1] != ':') {
      return false;
    }
    host = text.substr(1, close - 1);
    port_text = text.substr(close + 2);
  } else {
    const std::size_t colon = text.rfind(':');
    if (colon == std::string_view::npos) {
      return false;
    }
    host = text.substr(0, colon);
    port_text = text.substr(colon + 1);
  }

  if (host.empty() || port_text.empty()) {
    return false;
  }

  unsigned int port = 0;
  const char* const begin = port_text.data();
  const char* const end = begin + port_text.size();
  const auto result = std::from_chars(begin, end, port);
  if (result.ec != std::errc() || result.ptr != end || port == 0 || port > 65535) {
    return false;
  }

  out.host = std::string(host);
  out.port = static_cast<std::uint16_t>(port);
  return true;
}

std::string to_string(const Endpoint& endpoint) {
  if (endpoint.host.find(':') != std::string::npos) {
    return "[" + endpoint.host + "]:" + std::to_string(endpoint.port);
  }
  return endpoint.host + ":" + std::to_string(endpoint.port);
}

}  // namespace lob::publisher
