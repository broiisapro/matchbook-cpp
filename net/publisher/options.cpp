#include "net/publisher/options.hpp"

#include <charconv>
#include <cstdint>
#include <limits>
#include <utility>

namespace lob::publisher {
namespace {

constexpr std::size_t kMaxRetransmit = 1U << 20;

bool parse_unsigned(std::string_view text, std::uint64_t max, std::uint64_t& out) {
  if (text.empty()) {
    return false;
  }
  std::uint64_t value = 0;
  const char* const begin = text.data();
  const char* const end = begin + text.size();
  const auto result = std::from_chars(begin, end, value);
  if (result.ec != std::errc() || result.ptr != end || value > max) {
    return false;
  }
  out = value;
  return true;
}

bool require_value(int argc, int& index, std::string_view option, std::string& error) {
  if (index + 1 >= argc) {
    error = std::string(option) + " requires a value";
    return false;
  }
  ++index;
  return true;
}

}  // namespace

std::string_view publisher_usage() {
  return "usage: lob_publisher [options]\n"
         "\n"
         "  --gateway host:port      market-data event stream to read\n"
         "                           (default 127.0.0.1:9001)\n"
         "  --udp-dest host:port     UDP destination; repeat for several receivers\n"
         "  --mcast-group addr:port  extra multicast destination (send only)\n"
         "  --recovery-port port     TCP recovery service port (default 9002,\n"
         "                           0 asks the kernel for an ephemeral port)\n"
         "  --retransmit n           messages kept for recovery\n"
         "                           (default 65536, maximum 1048576)\n"
         "  --help                   show this message\n";
}

bool parse_publisher_options(int argc, char** argv, PublisherOptions& options, std::string& error) {
  PublisherOptions parsed;
  parsed.config.gateway = Endpoint{"127.0.0.1", 9001};
  parsed.config.recovery_port = 9002;
  parsed.config.retransmit_capacity = 65536;

  for (int index = 1; index < argc; ++index) {
    const std::string_view option = argv[index];
    if (option == "--help" || option == "-h") {
      parsed.help = true;
      options = std::move(parsed);
      return true;
    }
    if (option == "--gateway" || option == "--udp-dest" || option == "--mcast-group" ||
        option == "--recovery-port" || option == "--retransmit") {
      if (!require_value(argc, index, option, error)) {
        return false;
      }
      const std::string_view value = argv[index];

      if (option == "--gateway") {
        if (!parse_endpoint(value, parsed.config.gateway)) {
          error = "invalid --gateway value '" + std::string(value) + "' (expected host:port)";
          return false;
        }
      } else if (option == "--udp-dest") {
        Endpoint destination;
        if (!parse_endpoint(value, destination)) {
          error = "invalid --udp-dest value '" + std::string(value) + "' (expected host:port)";
          return false;
        }
        parsed.config.udp_destinations.push_back(std::move(destination));
      } else if (option == "--mcast-group") {
        Endpoint group;
        if (!parse_endpoint(value, group)) {
          error = "invalid --mcast-group value '" + std::string(value) + "' (expected host:port)";
          return false;
        }
        parsed.config.multicast_group = std::move(group);
      } else if (option == "--recovery-port") {
        std::uint64_t port = 0;
        if (!parse_unsigned(value, 65535, port)) {
          error = "invalid --recovery-port value '" + std::string(value) + "'";
          return false;
        }
        parsed.config.recovery_port = static_cast<std::uint16_t>(port);
      } else {
        std::uint64_t count = 0;
        if (!parse_unsigned(value, kMaxRetransmit, count) || count == 0) {
          error = "invalid --retransmit value '" + std::string(value) + "' (1.." +
                  std::to_string(kMaxRetransmit) + ")";
          return false;
        }
        parsed.config.retransmit_capacity = static_cast<std::size_t>(count);
      }
      continue;
    }

    error = "unknown option '" + std::string(option) + "'";
    return false;
  }

  if (parsed.config.udp_destinations.empty() && !parsed.config.multicast_group.has_value()) {
    error = "at least one --udp-dest is required";
    return false;
  }

  options = std::move(parsed);
  return true;
}

}  // namespace lob::publisher
