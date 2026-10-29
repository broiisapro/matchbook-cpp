#include <cerrno>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <string_view>
#include <thread>

#include "net/gateway/gateway.hpp"

namespace {

constexpr std::string_view kUsage =
    "usage: lob_gateway [options]\n"
    "\n"
    "TCP order-entry gateway with one engine thread and a market-data event\n"
    "stream. One connection is one session; there is no logon message.\n"
    "\n"
    "options:\n"
    "  --port P            order-entry TCP port (default 9000, 0 = ephemeral)\n"
    "  --md-port P         market-data TCP port (default 9001, 0 = ephemeral)\n"
    "  --min-price P       lowest accepted price, in ticks (default 1)\n"
    "  --max-price P       highest accepted price, in ticks (default 200000)\n"
    "  --max-orders N      live order capacity (default 1048576)\n"
    "  --max-levels N      price-level capacity per side (default 200000)\n"
    "  --help              print this help and exit\n";

volatile std::sig_atomic_t g_stop = 0;

void handle_signal(int) { g_stop = 1; }

bool parse_i64(std::string_view text, std::int64_t& out) {
  if (text.empty()) {
    return false;
  }
  const auto result = std::from_chars(text.data(), text.data() + text.size(), out);
  return result.ec == std::errc{} && result.ptr == text.data() + text.size();
}

bool parse_u64(std::string_view text, std::uint64_t& out) {
  if (text.empty()) {
    return false;
  }
  const auto result = std::from_chars(text.data(), text.data() + text.size(), out);
  return result.ec == std::errc{} && result.ptr == text.data() + text.size();
}

bool parse_port(std::string_view text, std::uint16_t& out) {
  std::uint64_t parsed = 0;
  if (!parse_u64(text, parsed) || parsed > 65535) {
    return false;
  }
  out = static_cast<std::uint16_t>(parsed);
  return true;
}

bool parse_args(int argc, char** argv, lob::net::GatewayOptions& options, bool& help) {
  for (int i = 1; i < argc; ++i) {
    const std::string_view flag = argv[i];
    if (flag == "--help" || flag == "-h") {
      help = true;
      continue;
    }
    if (i + 1 >= argc) {
      std::fprintf(stderr, "lob_gateway: '%s' needs a value\n", argv[i]);
      return false;
    }
    const std::string_view value = argv[++i];
    if (flag == "--port") {
      if (!parse_port(value, options.port)) {
        std::fputs("lob_gateway: --port needs an integer in [0, 65535]\n", stderr);
        return false;
      }
    } else if (flag == "--md-port") {
      if (!parse_port(value, options.md_port)) {
        std::fputs("lob_gateway: --md-port needs an integer in [0, 65535]\n", stderr);
        return false;
      }
    } else if (flag == "--min-price") {
      if (!parse_i64(value, options.book.min_price)) {
        std::fputs("lob_gateway: --min-price needs an integer\n", stderr);
        return false;
      }
    } else if (flag == "--max-price") {
      if (!parse_i64(value, options.book.max_price)) {
        std::fputs("lob_gateway: --max-price needs an integer\n", stderr);
        return false;
      }
    } else if (flag == "--max-orders") {
      std::uint64_t parsed = 0;
      if (!parse_u64(value, parsed) || parsed > UINT32_MAX) {
        std::fputs("lob_gateway: --max-orders needs an integer in [1, 2^32-1]\n", stderr);
        return false;
      }
      options.book.max_orders = static_cast<std::uint32_t>(parsed);
    } else if (flag == "--max-levels") {
      std::uint64_t parsed = 0;
      if (!parse_u64(value, parsed) || parsed > UINT32_MAX) {
        std::fputs("lob_gateway: --max-levels needs an integer in [1, 2^32-1]\n", stderr);
        return false;
      }
      options.book.max_levels = static_cast<std::uint32_t>(parsed);
    } else {
      std::fprintf(stderr, "lob_gateway: unknown option '%s'\n", argv[i - 1]);
      return false;
    }
  }
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  lob::net::GatewayOptions options;
  bool help = false;
  if (!parse_args(argc, argv, options, help)) {
    std::fputs(kUsage.data(), stderr);
    return 2;
  }
  if (help) {
    std::fputs(kUsage.data(), stdout);
    return 0;
  }

  try {
    lob::net::Gateway gateway(options);
    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);
    gateway.start();
    std::printf("lob_gateway: order entry on 0.0.0.0:%u, market data on 0.0.0.0:%u\n",
                static_cast<unsigned>(gateway.order_entry_port()),
                static_cast<unsigned>(gateway.md_port()));
    std::fflush(stdout);

    while (g_stop == 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    gateway.stop();
    std::fputs("lob_gateway: stopped\n", stdout);
  } catch (const std::exception& error) {
    std::fprintf(stderr, "lob_gateway: %s\n", error.what());
    return 1;
  }
  return 0;
}
