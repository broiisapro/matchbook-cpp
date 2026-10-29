#pragma once

// Command-line surface of lob_loadgen. Parsing lives in net/loadgen so the
// tests can exercise every flag without running the binary.

#include <chrono>
#include <cstdint>
#include <string>

#include "lob/types.hpp"
#include "net/loadgen/endpoint.hpp"

namespace lob::loadgen {

struct CliOptions {
  Endpoint gateway;
  std::string gateway_text;
  std::uint32_t sessions = 1;
  std::uint64_t orders = 0;
  double rate = 0.0;
  std::uint64_t seed = 1;
  std::string report_path;
  std::chrono::milliseconds response_timeout{5000};
  lob::Price min_price = 1;
  lob::Price max_price = 200000;
  /// UDP port of the publisher's market-data stream; 0 disables md counting.
  std::uint16_t md_udp_port = 0;
  /// Local address the market-data socket binds to. 0.0.0.0 receives on every
  /// interface, which keeps loopback reachable (host runs) and also works on a
  /// container bridge, where the publisher sends to the container's own
  /// address.
  std::string md_bind{"0.0.0.0"};
  /// Publisher recovery service used to fill market-data gaps.
  Endpoint recovery{"127.0.0.1", kDefaultRecoveryPort};
  /// After the last order response, how long the market-data monitor may keep
  /// draining to catch up with everything the stream announced.
  std::chrono::milliseconds md_settle{2000};
  bool help = false;
};

enum class CliStatus { Ok, Help, Error };

/// Parses argv (excluding argv[0]). On Error, `error` describes the first
/// problem and `out` is unspecified. `--help` returns CliStatus::Help with
/// `out.help` set.
CliStatus parse_cli(int argc, char** argv, CliOptions& out, std::string& error);

/// Usage text, one option per line.
std::string usage();

}  // namespace lob::loadgen
