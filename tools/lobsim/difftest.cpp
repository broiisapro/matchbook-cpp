#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "lob/config.hpp"
#include "lob/order_book.hpp"
#include "lob/types.hpp"
#include "reference/reference_book.hpp"
#include "tests/support/recording_listener.hpp"
#include "tools/flowfile/replay_config.hpp"
#include "tools/lobsim/commands.hpp"
#include "tools/opgen/differential.hpp"
#include "tools/opgen/opgen.hpp"

namespace lobsim {
namespace {

using lob::Config;
using lob::OrderBook;
using lob::OrderId;
using lob::OrderType;
using lob::Price;
using lob::Qty;
using lob::Side;
using lob::opgen::apply_op;
using lob::opgen::DiffResult;
using lob::opgen::ladder_stress_mix;
using lob::opgen::Op;
using lob::opgen::OpGenerator;
using lob::opgen::OpKind;
using lob::opgen::OpMix;
using lob::opgen::run_differential;
using lob::opgen::wide_mix;
using lob::ref::ReferenceBook;

enum class EngineKind { Fast, Reference };

/// Named configuration presets: the small narrow default and the flow tools'
/// wide replay config.
enum class ConfigKind { Narrow, Wide };

/// Default differential config. It is deliberately much smaller than the
/// flow-tool default replay config: a max_orders of 512 and a max_levels of 16
/// over prices 1..500 make a default run hit capacity cancels and level
/// capacity, and the generator's mid-price walk plus its edge prices exercise
/// both ends of the price range and the just-outside rejects. Overridable with
/// the four config flags.
Config default_diff_config() { return Config{1, 500, 512, 16}; }

/// Shrinking is time-boxed; the report says when the budget ran out.
constexpr double kShrinkSeconds = 30.0;

constexpr std::string_view kUsage =
    "usage: lobsim difftest --ops N --seed S [options]\n"
    "\n"
    "Runs one generated operation stream through two engines. After every\n"
    "operation the listener event streams must be identical and the two book\n"
    "digests must match. Defaults are fast vs reference with the narrow config\n"
    "(min-price 1, max-price 500, max-orders 512, max-levels 16) chosen so\n"
    "capacity cancels and price-range edges are actually hit.\n"
    "\n"
    "required:\n"
    "  --ops N              number of operations (N >= 0)\n"
    "  --seed S             64-bit seed for the operation generator\n"
    "\n"
    "options:\n"
    "  --engine-a E         fast | reference (default fast)\n"
    "  --engine-b E         fast | reference (default reference)\n"
    "  --config C           narrow | wide (default narrow); wide uses the flow\n"
    "                       tools' replay config (prices 1..200000, 1048576\n"
    "                       orders, 200000 levels) and, unless --mix is given,\n"
    "                       the wide generator mix that reaches hundreds of\n"
    "                       levels deep\n"
    "  --mix M              default | ladder_stress | wide (default: default\n"
    "                       for --config narrow, wide for --config wide);\n"
    "                       ladder_stress clusters prices at the ladder bitmap\n"
    "                       boundaries, sweeps a side empty and drains/refills\n"
    "                       the best level\n"
    "  --min-price P        override the config minimum price\n"
    "  --max-price P        override the config maximum price\n"
    "  --max-orders N       override the config maximum live orders\n"
    "  --max-levels N       override the config maximum levels per side\n"
    "  --progress           print progress lines to stderr\n"
    "  --help               print this message\n"
    "\n"
    "exit status: 0 no mismatches, 1 mismatch, 2 usage error\n";

struct DiffOptions {
  std::uint64_t ops = 0;
  std::uint64_t seed = 0;
  EngineKind engine_a = EngineKind::Fast;
  EngineKind engine_b = EngineKind::Reference;
  ConfigKind config_kind = ConfigKind::Narrow;
  Config config = default_diff_config();
  OpMix mix{};
  bool have_mix = false;
  bool progress = false;
  bool help = false;
};

template <class Integer>
bool parse_integer(std::string_view text, Integer& out) {
  if (text.empty()) {
    return false;
  }
  const auto result = std::from_chars(text.data(), text.data() + text.size(), out);
  return result.ec == std::errc{} && result.ptr == text.data() + text.size();
}

bool parse_engine(std::string_view text, EngineKind& out) {
  if (text == "fast") {
    out = EngineKind::Fast;
    return true;
  }
  if (text == "reference") {
    out = EngineKind::Reference;
    return true;
  }
  return false;
}

bool parse_mix(std::string_view text, OpMix& out) {
  if (text == "default") {
    out = OpMix{};
    return true;
  }
  if (text == "ladder_stress") {
    out = ladder_stress_mix();
    return true;
  }
  if (text == "wide") {
    out = wide_mix();
    return true;
  }
  return false;
}

bool parse_config(std::string_view text, ConfigKind& out) {
  if (text == "narrow") {
    out = ConfigKind::Narrow;
    return true;
  }
  if (text == "wide") {
    out = ConfigKind::Wide;
    return true;
  }
  return false;
}

bool parse_args(int argc, char** argv, DiffOptions& options) {
  bool have_ops = false;
  bool have_seed = false;
  std::optional<Price> min_price;
  std::optional<Price> max_price;
  std::optional<std::uint32_t> max_orders;
  std::optional<std::uint32_t> max_levels;

  for (int i = 1; i < argc; ++i) {
    const std::string_view flag = argv[i];
    if (flag.empty() || flag.front() != '-') {
      std::fprintf(stderr, "difftest: unexpected argument '%s'\n", argv[i]);
      return false;
    }
    if (flag == "--progress") {
      options.progress = true;
      continue;
    }
    if (flag == "--help" || flag == "-h") {
      options.help = true;
      return true;
    }
    if (i + 1 >= argc) {
      std::fprintf(stderr, "difftest: '%s' needs a value\n", argv[i]);
      return false;
    }
    const std::string_view value = argv[++i];

    if (flag == "--ops") {
      have_ops = parse_integer(value, options.ops);
      if (!have_ops) {
        std::fprintf(stderr, "difftest: --ops needs a non-negative integer, got '%s'\n",
                     std::string(value).c_str());
        return false;
      }
    } else if (flag == "--seed") {
      have_seed = parse_integer(value, options.seed);
      if (!have_seed) {
        std::fprintf(stderr, "difftest: --seed needs an unsigned integer, got '%s'\n",
                     std::string(value).c_str());
        return false;
      }
    } else if (flag == "--engine-a") {
      if (!parse_engine(value, options.engine_a)) {
        std::fprintf(stderr, "difftest: --engine-a must be fast or reference, got '%s'\n",
                     std::string(value).c_str());
        return false;
      }
    } else if (flag == "--engine-b") {
      if (!parse_engine(value, options.engine_b)) {
        std::fprintf(stderr, "difftest: --engine-b must be fast or reference, got '%s'\n",
                     std::string(value).c_str());
        return false;
      }
    } else if (flag == "--config") {
      if (!parse_config(value, options.config_kind)) {
        std::fprintf(stderr, "difftest: --config must be narrow or wide, got '%s'\n",
                     std::string(value).c_str());
        return false;
      }
    } else if (flag == "--mix") {
      if (!parse_mix(value, options.mix)) {
        std::fprintf(stderr, "difftest: --mix must be default, ladder_stress or wide, got '%s'\n",
                     std::string(value).c_str());
        return false;
      }
      options.have_mix = true;
    } else if (flag == "--min-price") {
      if (!parse_integer(value, min_price.emplace())) {
        std::fprintf(stderr, "difftest: --min-price needs an integer, got '%s'\n",
                     std::string(value).c_str());
        return false;
      }
    } else if (flag == "--max-price") {
      if (!parse_integer(value, max_price.emplace())) {
        std::fprintf(stderr, "difftest: --max-price needs an integer, got '%s'\n",
                     std::string(value).c_str());
        return false;
      }
    } else if (flag == "--max-orders") {
      if (!parse_integer(value, max_orders.emplace())) {
        std::fprintf(stderr, "difftest: --max-orders needs a positive integer, got '%s'\n",
                     std::string(value).c_str());
        return false;
      }
    } else if (flag == "--max-levels") {
      if (!parse_integer(value, max_levels.emplace())) {
        std::fprintf(stderr, "difftest: --max-levels needs a positive integer, got '%s'\n",
                     std::string(value).c_str());
        return false;
      }
    } else {
      std::fprintf(stderr, "difftest: unknown option '%s'\n", argv[i - 1]);
      return false;
    }
  }

  if (!have_ops) {
    std::fputs("difftest: --ops is required\n", stderr);
    return false;
  }
  if (!have_seed) {
    std::fputs("difftest: --seed is required\n", stderr);
    return false;
  }

  // The config preset comes first so explicit limit flags override it no
  // matter where --config appears on the command line.
  options.config = options.config_kind == ConfigKind::Wide ? lob::flowfile::default_replay_config()
                                                           : default_diff_config();
  if (min_price) {
    options.config.min_price = *min_price;
  }
  if (max_price) {
    options.config.max_price = *max_price;
  }
  if (max_orders) {
    options.config.max_orders = *max_orders;
  }
  if (max_levels) {
    options.config.max_levels = *max_levels;
  }
  if (!options.have_mix) {
    options.mix = options.config_kind == ConfigKind::Wide ? wide_mix() : OpMix{};
  }

  if (!lob::valid(options.config)) {
    std::fputs(
        "difftest: the configuration is not usable "
        "(min_price >= 1, min_price <= max_price, max_orders >= 1, max_levels >= 1)\n",
        stderr);
    return false;
  }
  return true;
}

const char* engine_name(EngineKind engine) {
  return engine == EngineKind::Fast ? "fast" : "reference";
}

const char* config_name(ConfigKind config) {
  return config == ConfigKind::Wide ? "wide" : "narrow";
}

const char* kind_name(OpKind kind) {
  switch (kind) {
    case OpKind::Submit:
      return "Submit";
    case OpKind::Cancel:
      return "Cancel";
    case OpKind::Modify:
      return "Modify";
  }
  return "Submit";
}

const char* side_name(Side side) { return side == Side::Buy ? "Buy" : "Sell"; }

const char* type_name(OrderType type) {
  switch (type) {
    case OrderType::Limit:
      return "Limit";
    case OrderType::Market:
      return "Market";
    case OrderType::IOC:
      return "IOC";
    case OrderType::FOK:
      return "FOK";
    case OrderType::PostOnly:
      return "PostOnly";
  }
  return "Limit";
}

/// One operation as a C++ aggregate initializer a test can paste.
std::string pasteable(const Op& op) {
  std::ostringstream out;
  out << "{lob::opgen::OpKind::" << kind_name(op.kind) << ", " << op.id
      << ", lob::Side::" << side_name(op.side) << ", lob::OrderType::" << type_name(op.type) << ", "
      << op.price << ", " << op.qty << "}";
  return out.str();
}

std::string format_events(const std::vector<lob::test::Event>& events) {
  std::ostringstream out;
  for (std::size_t i = 0; i < events.size(); ++i) {
    if (i != 0) {
      out << ", ";
    }
    out << events[i];
  }
  return out.str();
}

/// True when replaying `ops` from a fresh pair of engines produces at least one
/// mismatch (events differ or digests differ).
template <class EngineA, class EngineB>
bool ops_mismatch(const Config& config, const std::vector<Op>& ops) {
  lob::test::RecordingListener listener_a;
  lob::test::RecordingListener listener_b;
  EngineA engine_a(config, listener_a);
  EngineB engine_b(config, listener_b);

  for (const Op& op : ops) {
    listener_a.clear();
    listener_b.clear();
    apply_op(engine_a, op);
    apply_op(engine_b, op);
    if (listener_a.events != listener_b.events || engine_a.digest() != engine_b.digest()) {
      return true;
    }
  }
  return false;
}

/// Greedily deletes operations from the failing prefix, from the back towards
/// the front, while the mismatch persists. Time-boxed; `timed_out` says
/// whether the budget ran out before the result stopped shrinking.
template <class EngineA, class EngineB>
std::vector<Op> shrink(const Config& config, const std::vector<Op>& failing, bool& timed_out) {
  std::vector<Op> current = failing;
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::duration<double>(kShrinkSeconds);
  timed_out = false;

  bool changed = true;
  while (changed) {
    changed = false;
    for (std::size_t i = current.size(); i-- > 0;) {
      if (std::chrono::steady_clock::now() >= deadline) {
        timed_out = true;
        return current;
      }
      std::vector<Op> candidate = current;
      candidate.erase(candidate.begin() + static_cast<std::ptrdiff_t>(i));
      if (ops_mismatch<EngineA, EngineB>(config, candidate)) {
        current.swap(candidate);
        changed = true;
      }
    }
  }
  return current;
}

/// Reports a mismatch, shrinks a repro and prints it.
template <class EngineA, class EngineB>
void report_mismatch(const DiffOptions& options, const DiffResult& result) {
  std::printf("mismatch at op %llu\n",
              static_cast<unsigned long long>(result.first_mismatch_index));
  std::printf("op: %s\n", pasteable(result.op).c_str());
  std::printf("events-a: [%s]\n", format_events(result.events_a).c_str());
  std::printf("events-b: [%s]\n", format_events(result.events_b).c_str());
  std::printf("digest-a: 0x%016llx\n", static_cast<unsigned long long>(result.digest_a));
  std::printf("digest-b: 0x%016llx\n", static_cast<unsigned long long>(result.digest_b));

  OpGenerator replay(options.seed, options.config, options.mix);
  std::vector<Op> prefix;
  prefix.reserve(static_cast<std::size_t>(result.first_mismatch_index) + 1);
  for (std::uint64_t i = 0; i <= result.first_mismatch_index; ++i) {
    prefix.push_back(replay.next());
  }

  bool timed_out = false;
  const std::vector<Op> repro = shrink<EngineA, EngineB>(options.config, prefix, timed_out);
  std::printf("repro-ops: %zu%s\n", repro.size(), timed_out ? " (shrink timed out)" : "");
  std::printf("repro:\n");
  for (const Op& op : repro) {
    std::printf("  %s,\n", pasteable(op).c_str());
  }
}

template <class EngineA, class EngineB>
int run_selected(const DiffOptions& options) {
  OpGenerator generator(options.seed, options.config, options.mix);
  std::function<void(std::uint64_t)> progress;
  if (options.progress) {
    progress = [](std::uint64_t done) {
      std::fprintf(stderr, "progress: %llu ops\n", static_cast<unsigned long long>(done));
    };
  }

  const auto started = std::chrono::steady_clock::now();
  const DiffResult result =
      run_differential<EngineA, EngineB>(options.config, generator, options.ops, progress);
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - started)
                           .count();

  if (result.mismatches != 0) {
    report_mismatch<EngineA, EngineB>(options, result);
  }

  std::printf("engine-a: %s\n", engine_name(options.engine_a));
  std::printf("engine-b: %s\n", engine_name(options.engine_b));
  std::printf("config: %s\n", config_name(options.config_kind));
  std::printf("mix: %s\n", std::string(lob::opgen::to_string(options.mix.preset)).c_str());
  std::printf("min-price: %lld\n", static_cast<long long>(options.config.min_price));
  std::printf("max-price: %lld\n", static_cast<long long>(options.config.max_price));
  std::printf("max-orders: %u\n", options.config.max_orders);
  std::printf("max-levels: %u\n", options.config.max_levels);
  std::printf("max-depth-a: %llu\n", static_cast<unsigned long long>(
                                         std::max(result.max_bid_depth_a, result.max_ask_depth_a)));
  std::printf("max-depth-b: %llu\n", static_cast<unsigned long long>(
                                         std::max(result.max_bid_depth_b, result.max_ask_depth_b)));
  std::printf("ops: %llu\n", static_cast<unsigned long long>(result.ops_run));
  std::printf("seed: %llu\n", static_cast<unsigned long long>(options.seed));
  std::printf("mismatches: %llu\n", static_cast<unsigned long long>(result.mismatches));
  std::printf("wall_ms: %lld\n", static_cast<long long>(elapsed));
  return result.mismatches == 0 ? 0 : 1;
}

int dispatch(const DiffOptions& options) {
  if (options.engine_a == EngineKind::Fast && options.engine_b == EngineKind::Fast) {
    return run_selected<OrderBook, OrderBook>(options);
  }
  if (options.engine_a == EngineKind::Fast && options.engine_b == EngineKind::Reference) {
    return run_selected<OrderBook, ReferenceBook>(options);
  }
  if (options.engine_a == EngineKind::Reference && options.engine_b == EngineKind::Fast) {
    return run_selected<ReferenceBook, OrderBook>(options);
  }
  return run_selected<ReferenceBook, ReferenceBook>(options);
}

}  // namespace

int run_difftest(int argc, char** argv) {
  DiffOptions options;
  if (!parse_args(argc, argv, options)) {
    return 2;
  }
  if (options.help) {
    std::fputs(kUsage.data(), stdout);
    return 0;
  }
  return dispatch(options);
}

}  // namespace lobsim
