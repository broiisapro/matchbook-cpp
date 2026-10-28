#include "net/loadgen/endpoint.hpp"

#include <charconv>
#include <utility>

namespace lob::loadgen {
namespace {

bool parse_port(std::string_view text, std::uint16_t& out, std::string& error) {
  if (text.empty()) {
    error = "missing port";
    return false;
  }
  std::uint32_t value = 0;
  const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
  if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) {
    error = "port is not a number: '" + std::string(text) + "'";
    return false;
  }
  if (value == 0 || value > 65535) {
    error = "port out of range 1..65535: " + std::string(text);
    return false;
  }
  out = static_cast<std::uint16_t>(value);
  return true;
}

}  // namespace

bool parse_endpoint(std::string_view text, Endpoint& out, std::string& error) {
  if (text.empty()) {
    error = "empty endpoint";
    return false;
  }

  std::string_view host;
  std::string_view port;
  bool has_port = false;

  if (text.front() == '[') {
    const std::size_t close = text.find(']');
    if (close == std::string_view::npos) {
      error = "unterminated IPv6 literal: '" + std::string(text) + "'";
      return false;
    }
    host = text.substr(1, close - 1);
    const std::string_view rest = text.substr(close + 1);
    if (!rest.empty()) {
      if (rest.front() != ':') {
        error = "expected ':' after IPv6 literal: '" + std::string(text) + "'";
        return false;
      }
      port = rest.substr(1);
      has_port = true;
    }
  } else {
    const std::size_t colon = text.rfind(':');
    if (colon == std::string_view::npos) {
      host = text;
    } else {
      host = text.substr(0, colon);
      port = text.substr(colon + 1);
      has_port = true;
    }
  }

  if (host.empty()) {
    error = "missing host in '" + std::string(text) + "'";
    return false;
  }

  Endpoint parsed;
  parsed.host = std::string(host);
  parsed.port = kDefaultGatewayPort;
  if (has_port && !parse_port(port, parsed.port, error)) {
    return false;
  }
  out = std::move(parsed);
  return true;
}

}  // namespace lob::loadgen
