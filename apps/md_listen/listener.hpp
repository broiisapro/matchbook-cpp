#pragma once

// Command-line market-data listener (Phase 3).
//
// The listener is deliberately thin: it owns a FeedHandler, prints the top of
// the local book and the feed counters, and exits after the requested time. The
// logic lives in this header so tests can drive exactly the same code path the
// executable runs, without shelling out to the binary.

#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

#include "net/md_client/feed_handler.hpp"

namespace lob::md {

struct ListenerOptions {
  std::uint16_t udp_port = 9100;
  std::string bind_address = "0.0.0.0";
  std::string recovery_host = "127.0.0.1";
  std::uint16_t recovery_port = 9002;
  std::size_t top = 5;
  /// 0 runs until interrupted; otherwise the listener stops after this many
  /// seconds even while messages keep arriving.
  int seconds = 0;
  int poll_timeout_ms = 50;
  int print_interval_ms = 1000;
  /// Stops once this many live feed frames have been read. 0 disables the
  /// limit; it exists so tests finish deterministically instead of by wall
  /// clock.
  std::size_t stop_after_messages = 0;
};

inline std::string format_levels(const std::vector<Level>& levels) {
  if (levels.empty()) {
    return "-";
  }
  std::string text;
  for (const Level& level : levels) {
    if (!text.empty()) {
      text += ", ";
    }
    text += std::to_string(level.price);
    text += " x ";
    text += std::to_string(level.qty);
  }
  return text;
}

inline void print_book(std::ostream& out, const LevelBook& book, std::size_t depth) {
  out << "book bid: " << format_levels(book.top(Side::Buy, depth))
      << " | ask: " << format_levels(book.top(Side::Sell, depth)) << "\n";
}

inline void print_counters(std::ostream& out, const FeedCounters& counters) {
  out << "counters received=" << counters.received << " duplicates=" << counters.duplicates
      << " gaps=" << counters.gaps_detected << " recovered=" << counters.gaps_recovered
      << " resets=" << counters.resets << " malformed=" << counters.malformed << "\n";
}

/// The one-line summary contract from issue #124.
inline std::string summary_line(const FeedCounters& counters) {
  return "gaps: " + std::to_string(counters.gaps_detected) +
         " recovered: " + std::to_string(counters.gaps_recovered) +
         " resets: " + std::to_string(counters.resets);
}

/// Runs the listener until the time or message limit is reached. `on_bound` is
/// called with the bound UDP port after the socket is open; it exists so tests
/// can point an in-process publisher at the actual (ephemeral) port.
inline int run_listener(const ListenerOptions& options, std::ostream& out, std::ostream& err,
                        const std::function<void(std::uint16_t)>& on_bound = {}) {
  FeedHandlerConfig config;
  config.udp_port = options.udp_port;
  config.bind_address = options.bind_address;
  config.recovery_host = options.recovery_host;
  config.recovery_port = options.recovery_port;

  FeedHandler handler(config);
  std::string error;
  if (!handler.open(error)) {
    err << "lob_md_listen: " << error << "\n";
    return 1;
  }

  out << "listening on udp://" << options.bind_address << ":" << handler.udp_port() << " recovery "
      << options.recovery_host << ":" << options.recovery_port << "\n";
  if (on_bound) {
    on_bound(handler.udp_port());
  }

  using Clock = std::chrono::steady_clock;
  const auto started = Clock::now();
  auto next_print = started;
  while (true) {
    const auto now = Clock::now();
    if (options.seconds > 0 && now - started >= std::chrono::seconds(options.seconds)) {
      break;
    }
    if (options.stop_after_messages > 0 &&
        handler.counters().received >= options.stop_after_messages) {
      break;
    }

    if (now >= next_print) {
      print_book(out, handler.book(), options.top);
      print_counters(out, handler.counters());
      next_print = now + std::chrono::milliseconds(options.print_interval_ms);
    }
    handler.poll(options.poll_timeout_ms);
  }

  print_book(out, handler.book(), options.top);
  print_counters(out, handler.counters());
  out << summary_line(handler.counters()) << "\n";
  return 0;
}

inline bool parse_u16(std::string_view text, std::uint16_t& value) {
  unsigned parsed = 0;
  const auto result = std::from_chars(text.data(), text.data() + text.size(), parsed);
  if (result.ec != std::errc{} || result.ptr != text.data() + text.size() || parsed > 65535U) {
    return false;
  }
  value = static_cast<std::uint16_t>(parsed);
  return true;
}

inline bool parse_size(std::string_view text, std::size_t& value, std::size_t minimum) {
  std::size_t parsed = 0;
  const auto result = std::from_chars(text.data(), text.data() + text.size(), parsed);
  if (result.ec != std::errc{} || result.ptr != text.data() + text.size() || parsed < minimum) {
    return false;
  }
  value = parsed;
  return true;
}

inline bool parse_int(std::string_view text, int& value, int minimum) {
  int parsed = 0;
  const auto result = std::from_chars(text.data(), text.data() + text.size(), parsed);
  if (result.ec != std::errc{} || result.ptr != text.data() + text.size() || parsed < minimum) {
    return false;
  }
  value = parsed;
  return true;
}

/// `host:port` with the port after the last colon. IPv4 and names only; the
/// Phase 3 topology is unicast IPv4 (docs/architecture.md §14).
inline bool parse_endpoint(std::string_view text, std::string& host, std::uint16_t& port) {
  const std::size_t colon = text.rfind(':');
  if (colon == std::string_view::npos || colon == 0 || colon + 1 >= text.size()) {
    return false;
  }
  std::uint16_t parsed = 0;
  if (!parse_u16(text.substr(colon + 1), parsed) || parsed == 0) {
    return false;
  }
  host.assign(text.substr(0, colon));
  port = parsed;
  return true;
}

/// Parses the command line. `--help` is handled by main before this is called.
inline std::optional<ListenerOptions> parse_listener_options(int argc, char** argv,
                                                             std::string& error) {
  ListenerOptions options;
  for (int i = 1; i < argc; ++i) {
    const std::string_view argument = argv[i];
    auto value = [&](std::string_view name) -> std::optional<std::string_view> {
      if (i + 1 >= argc) {
        error = std::string(name) + " needs a value";
        return std::nullopt;
      }
      ++i;
      return std::string_view(argv[i]);
    };

    if (argument == "--udp-port") {
      const std::optional<std::string_view> text = value(argument);
      if (!text) {
        return std::nullopt;
      }
      if (!parse_u16(*text, options.udp_port)) {
        error = "invalid UDP port '" + std::string(*text) + "'";
        return std::nullopt;
      }
    } else if (argument == "--bind") {
      const std::optional<std::string_view> text = value(argument);
      if (!text) {
        return std::nullopt;
      }
      options.bind_address.assign(*text);
    } else if (argument == "--recovery") {
      const std::optional<std::string_view> text = value(argument);
      if (!text) {
        return std::nullopt;
      }
      if (!parse_endpoint(*text, options.recovery_host, options.recovery_port)) {
        error = "invalid recovery endpoint '" + std::string(*text) + "' (expected host:port)";
        return std::nullopt;
      }
    } else if (argument == "--top") {
      const std::optional<std::string_view> text = value(argument);
      if (!text) {
        return std::nullopt;
      }
      if (!parse_size(*text, options.top, 1)) {
        error = "invalid depth '" + std::string(*text) + "'";
        return std::nullopt;
      }
    } else if (argument == "--seconds") {
      const std::optional<std::string_view> text = value(argument);
      if (!text) {
        return std::nullopt;
      }
      if (!parse_int(*text, options.seconds, 0)) {
        error = "invalid duration '" + std::string(*text) + "'";
        return std::nullopt;
      }
    } else {
      error = "unknown option '" + std::string(argument) + "'";
      return std::nullopt;
    }
  }
  return options;
}

}  // namespace lob::md
