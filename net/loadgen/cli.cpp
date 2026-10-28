#include "net/loadgen/cli.hpp"

#include <charconv>
#include <cmath>
#include <cstdlib>
#include <string_view>

namespace lob::loadgen {
namespace {

bool parse_u64(std::string_view text, std::uint64_t& out) {
  if (text.empty()) {
    return false;
  }
  std::uint64_t value = 0;
  const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
  if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) {
    return false;
  }
  out = value;
  return true;
}

bool parse_i64(std::string_view text, lob::Price& out) {
  if (text.empty()) {
    return false;
  }
  std::int64_t value = 0;
  const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
  if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) {
    return false;
  }
  out = value;
  return true;
}

bool parse_rate(std::string_view text, double& out) {
  if (text.empty()) {
    return false;
  }
  const std::string owned(text);
  char* end = nullptr;
  const double value = std::strtod(owned.c_str(), &end);
  if (end != owned.c_str() + owned.size() || !std::isfinite(value) || value < 0.0) {
    return false;
  }
  out = value;
  return true;
}

std::string missing_value(std::string_view option) {
  return "option '" + std::string(option) + "' needs a value";
}

}  // namespace

std::string usage() {
  return "usage: lob_loadgen --gateway HOST[:PORT] --orders M --report PATH [options]\n"
         "\n"
         "  --gateway HOST[:PORT]  order-entry gateway (default port 9000), required\n"
         "  --orders M             total requests to send, required\n"
         "  --sessions N           concurrent sessions, one connection each (default 1)\n"
         "  --rate R               target requests/second across all sessions (0 or absent = "
         "unlimited)\n"
         "  --seed S               seed for the generated order flows (default 1)\n"
         "  --report PATH          JSON report destination, required\n"
         "  --timeout-ms N         per-request response timeout in ms (default 5000)\n"
         "  --min-price P          lowest generated limit price (default 1)\n"
         "  --max-price P          highest generated limit price (default 200000)\n"
         "  --help, -h             print this text\n";
}

CliStatus parse_cli(int argc, char** argv, CliOptions& out, std::string& error) {
  CliOptions options;
  bool saw_gateway = false;
  bool saw_orders = false;
  bool saw_report = false;

  for (int index = 1; index < argc; ++index) {
    const std::string_view argument(argv[index]);
    if (argument == "--help" || argument == "-h") {
      out = CliOptions{};
      out.help = true;
      return CliStatus::Help;
    }

    const auto value_ahead = [&](std::string_view option) -> std::string_view {
      if (index + 1 >= argc) {
        error = missing_value(option);
        return {};
      }
      ++index;
      const std::string_view value(argv[index]);
      if (value.empty()) {
        error = missing_value(option);
      }
      return value;
    };

    const auto flag = [&]() -> CliStatus {
      if (argument == "--gateway") {
        const std::string_view value = value_ahead(argument);
        if (value.empty()) {
          return CliStatus::Error;
        }
        if (!parse_endpoint(value, options.gateway, error)) {
          return CliStatus::Error;
        }
        options.gateway_text = std::string(value);
        saw_gateway = true;
        return CliStatus::Ok;
      }
      if (argument == "--orders") {
        const std::string_view value = value_ahead(argument);
        if (value.empty()) {
          return CliStatus::Error;
        }
        if (!parse_u64(value, options.orders) || options.orders == 0) {
          error = "--orders must be a positive number: '" + std::string(value) + "'";
          return CliStatus::Error;
        }
        saw_orders = true;
        return CliStatus::Ok;
      }
      if (argument == "--sessions") {
        const std::string_view value = value_ahead(argument);
        if (value.empty()) {
          return CliStatus::Error;
        }
        std::uint64_t sessions = 0;
        if (!parse_u64(value, sessions) || sessions == 0 || sessions > 1024) {
          error = "--sessions must be between 1 and 1024: '" + std::string(value) + "'";
          return CliStatus::Error;
        }
        options.sessions = static_cast<std::uint32_t>(sessions);
        return CliStatus::Ok;
      }
      if (argument == "--rate") {
        const std::string_view value = value_ahead(argument);
        if (value.empty()) {
          return CliStatus::Error;
        }
        if (!parse_rate(value, options.rate)) {
          error = "--rate must be a non-negative number: '" + std::string(value) + "'";
          return CliStatus::Error;
        }
        return CliStatus::Ok;
      }
      if (argument == "--seed") {
        const std::string_view value = value_ahead(argument);
        if (value.empty()) {
          return CliStatus::Error;
        }
        if (!parse_u64(value, options.seed)) {
          error = "--seed must be an unsigned integer: '" + std::string(value) + "'";
          return CliStatus::Error;
        }
        return CliStatus::Ok;
      }
      if (argument == "--report") {
        const std::string_view value = value_ahead(argument);
        if (value.empty()) {
          return CliStatus::Error;
        }
        options.report_path = std::string(value);
        saw_report = true;
        return CliStatus::Ok;
      }
      if (argument == "--timeout-ms") {
        const std::string_view value = value_ahead(argument);
        if (value.empty()) {
          return CliStatus::Error;
        }
        std::uint64_t milliseconds = 0;
        if (!parse_u64(value, milliseconds) || milliseconds == 0) {
          error = "--timeout-ms must be a positive number: '" + std::string(value) + "'";
          return CliStatus::Error;
        }
        options.response_timeout = std::chrono::milliseconds(milliseconds);
        return CliStatus::Ok;
      }
      if (argument == "--min-price") {
        const std::string_view value = value_ahead(argument);
        if (value.empty()) {
          return CliStatus::Error;
        }
        if (!parse_i64(value, options.min_price)) {
          error = "--min-price must be an integer: '" + std::string(value) + "'";
          return CliStatus::Error;
        }
        return CliStatus::Ok;
      }
      if (argument == "--max-price") {
        const std::string_view value = value_ahead(argument);
        if (value.empty()) {
          return CliStatus::Error;
        }
        if (!parse_i64(value, options.max_price)) {
          error = "--max-price must be an integer: '" + std::string(value) + "'";
          return CliStatus::Error;
        }
        return CliStatus::Ok;
      }
      error = "unknown option: '" + std::string(argument) + "'";
      return CliStatus::Error;
    };

    if (flag() != CliStatus::Ok) {
      return CliStatus::Error;
    }
  }

  if (!saw_gateway) {
    error = "--gateway is required";
    return CliStatus::Error;
  }
  if (!saw_orders) {
    error = "--orders is required";
    return CliStatus::Error;
  }
  if (!saw_report) {
    error = "--report is required";
    return CliStatus::Error;
  }
  if (options.min_price > options.max_price) {
    error = "--min-price must not exceed --max-price";
    return CliStatus::Error;
  }

  out = options;
  return CliStatus::Ok;
}

}  // namespace lob::loadgen
