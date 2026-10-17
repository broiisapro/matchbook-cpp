#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include "lob/types.hpp"
#include "tools/flowfile/flowfile.hpp"
#include "tools/flowfile/replay_config.hpp"
#include "tools/lobsim/commands.hpp"

namespace lobsim {
namespace {

using lob::OrderId;
using lob::OrderType;
using lob::Price;
using lob::Qty;
using lob::Side;
using lob::flowfile::FlowOp;
using lob::flowfile::FlowRecord;

constexpr std::string_view kUsage =
    "usage: lobsim gen --orders N --seed S --out F [options]\n"
    "\n"
    "Generates a seeded synthetic flow: Poisson arrivals (exponential\n"
    "inter-arrival times in ts_ns), a random-walk mid price in integer ticks\n"
    "inside the default replay config, and cancels/modifies of earlier ids.\n"
    "\n"
    "required:\n"
    "  --orders N          number of submit records (N >= 0)\n"
    "  --seed S            64-bit RNG seed; the same seed gives a byte-identical file\n"
    "  --out F             output path\n"
    "\n"
    "options:\n"
    "  --csv               write the CSV form instead of the binary form\n"
    "  --cancel-ratio R    cancels per submit, in [0,1] (default 0.10)\n"
    "  --modify-ratio R    modifies per submit, in [0,1] (default 0.10)\n"
    "  --market-ratio R    market orders per submit, in [0,1] (default 0.00)\n"
    "  --ioc-ratio R       IOC orders per submit, in [0,1] (default 0.00)\n"
    "  --mid P             initial mid price in ticks (default 100000)\n"
    "  --spread-ticks K    base half-spread for passive prices (default 2)\n";

constexpr std::uint64_t kMeanInterArrivalNs = 1'000'000;

struct GenOptions {
  std::uint64_t orders = 0;
  std::uint64_t seed = 0;
  std::string out;
  bool csv = false;
  double cancel_ratio = 0.10;
  double modify_ratio = 0.10;
  double market_ratio = 0.0;
  double ioc_ratio = 0.0;
  Price mid = 100000;
  Price spread_ticks = 2;
};

bool parse_u64(std::string_view text, std::uint64_t& out) {
  if (text.empty()) {
    return false;
  }
  const auto result = std::from_chars(text.data(), text.data() + text.size(), out);
  return result.ec == std::errc{} && result.ptr == text.data() + text.size();
}

bool parse_price(std::string_view text, Price& out) {
  if (text.empty()) {
    return false;
  }
  const auto result = std::from_chars(text.data(), text.data() + text.size(), out);
  return result.ec == std::errc{} && result.ptr == text.data() + text.size();
}

bool parse_ratio(std::string_view text, double& out) {
  if (text.empty()) {
    return false;
  }
  const std::string owned(text);
  char* end = nullptr;
  out = std::strtod(owned.c_str(), &end);
  return end == owned.c_str() + owned.size() && out >= 0.0 && out <= 1.0;
}

/// Parses argv (argv[0] is "gen"), printing a usage message on any problem.
bool parse_args(int argc, char** argv, GenOptions& options) {
  bool have_orders = false;
  bool have_seed = false;
  bool have_out = false;

  for (int i = 1; i < argc; ++i) {
    const std::string_view flag = argv[i];
    if (flag.empty() || flag.front() != '-') {
      std::fprintf(stderr, "gen: unexpected argument '%s'\n", argv[i]);
      return false;
    }
    if (flag == "--csv") {
      options.csv = true;
      continue;
    }
    if (i + 1 >= argc) {
      std::fprintf(stderr, "gen: '%s' needs a value\n", argv[i]);
      return false;
    }
    const std::string_view value = argv[++i];

    if (flag == "--orders") {
      have_orders = parse_u64(value, options.orders);
      if (!have_orders) {
        std::fprintf(stderr, "gen: --orders needs a non-negative integer, got '%s'\n",
                     std::string(value).c_str());
        return false;
      }
    } else if (flag == "--seed") {
      have_seed = parse_u64(value, options.seed);
      if (!have_seed) {
        std::fprintf(stderr, "gen: --seed needs an unsigned integer, got '%s'\n",
                     std::string(value).c_str());
        return false;
      }
    } else if (flag == "--out") {
      options.out = std::string(value);
      have_out = true;
    } else if (flag == "--cancel-ratio") {
      if (!parse_ratio(value, options.cancel_ratio)) {
        std::fprintf(stderr, "gen: --cancel-ratio needs a number in [0,1], got '%s'\n",
                     std::string(value).c_str());
        return false;
      }
    } else if (flag == "--modify-ratio") {
      if (!parse_ratio(value, options.modify_ratio)) {
        std::fprintf(stderr, "gen: --modify-ratio needs a number in [0,1], got '%s'\n",
                     std::string(value).c_str());
        return false;
      }
    } else if (flag == "--market-ratio") {
      if (!parse_ratio(value, options.market_ratio)) {
        std::fprintf(stderr, "gen: --market-ratio needs a number in [0,1], got '%s'\n",
                     std::string(value).c_str());
        return false;
      }
    } else if (flag == "--ioc-ratio") {
      if (!parse_ratio(value, options.ioc_ratio)) {
        std::fprintf(stderr, "gen: --ioc-ratio needs a number in [0,1], got '%s'\n",
                     std::string(value).c_str());
        return false;
      }
    } else if (flag == "--mid") {
      if (!parse_price(value, options.mid)) {
        std::fprintf(stderr, "gen: --mid needs an integer price, got '%s'\n",
                     std::string(value).c_str());
        return false;
      }
    } else if (flag == "--spread-ticks") {
      if (!parse_price(value, options.spread_ticks) || options.spread_ticks < 0) {
        std::fprintf(stderr, "gen: --spread-ticks needs a non-negative integer, got '%s'\n",
                     std::string(value).c_str());
        return false;
      }
    } else {
      std::fprintf(stderr, "gen: unknown option '%s'\n", argv[i - 1]);
      return false;
    }
  }

  if (!have_orders) {
    std::fputs("gen: --orders is required\n", stderr);
    return false;
  }
  if (!have_seed) {
    std::fputs("gen: --seed is required\n", stderr);
    return false;
  }
  if (!have_out) {
    std::fputs("gen: --out is required\n", stderr);
    return false;
  }
  if (options.market_ratio + options.ioc_ratio > 1.0) {
    std::fputs("gen: --market-ratio + --ioc-ratio must be <= 1\n", stderr);
    return false;
  }
  return true;
}

double uniform01(std::mt19937_64& rng) {
  return static_cast<double>(rng() >> 11) * (1.0 / 9007199254740992.0);
}

std::int64_t uniform_span(std::mt19937_64& rng, std::int64_t lo, std::int64_t hi) {
  return lo + static_cast<std::int64_t>(rng() % static_cast<std::uint64_t>(hi - lo + 1));
}

/// Exponential inter-arrival in nanoseconds with the given mean.
std::uint64_t next_interval_ns(std::mt19937_64& rng) {
  const double u = uniform01(rng);
  const double value = -std::log(1.0 - u) * static_cast<double>(kMeanInterArrivalNs);
  return static_cast<std::uint64_t>(value) + 1;
}

Price clamp_price(Price price, Price lo, Price hi) { return std::min(std::max(price, lo), hi); }

}  // namespace

int run_gen(int argc, char** argv) {
  GenOptions options;
  if (!parse_args(argc, argv, options)) {
    std::fputs(kUsage.data(), stderr);
    return 2;
  }

  const lob::Config config = lob::flowfile::default_replay_config();
  const Price margin = options.spread_ticks + 8;
  const Price low = std::min(config.min_price + margin, config.max_price);
  const Price high = std::max(config.max_price - margin, config.min_price);
  Price mid = clamp_price(options.mid, low, high);

  std::mt19937_64 rng(options.seed);
  std::vector<FlowRecord> records;
  std::vector<OrderId> earlier_ids;
  records.reserve(static_cast<std::size_t>(options.orders) * 2);
  earlier_ids.reserve(static_cast<std::size_t>(options.orders));

  std::uint64_t ts_ns = 0;
  OrderId next_id = 1;
  for (std::uint64_t i = 0; i < options.orders; ++i) {
    ts_ns += next_interval_ns(rng);

    const Price step = uniform_span(rng, -2, 2);
    mid = clamp_price(mid + step, low, high);

    const Side side = (rng() % 2 == 0) ? Side::Buy : Side::Sell;

    const double type_draw = uniform01(rng);
    OrderType type = OrderType::Limit;
    if (type_draw < options.market_ratio) {
      type = OrderType::Market;
    } else if (type_draw < options.market_ratio + options.ioc_ratio) {
      type = OrderType::IOC;
    }

    Price price = 0;
    if (type != OrderType::Market) {
      if (uniform01(rng) < 0.25) {
        // Aggressive: priced through the mid, so some orders cross.
        const Price offset = uniform_span(rng, 0, 2);
        price = side == Side::Buy ? mid + offset : mid - offset;
      } else {
        const Price half_spread = options.spread_ticks / 2;
        const Price depth = uniform_span(rng, 0, 5);
        price = side == Side::Buy ? mid - half_spread - depth : mid + half_spread + depth;
      }
      price = clamp_price(price, config.min_price, config.max_price);
    }

    const Qty qty = uniform_span(rng, 1, 100);
    const OrderId id = next_id++;
    records.push_back(FlowRecord{FlowOp::Submit, side, type, id, price, qty, ts_ns});
    earlier_ids.push_back(id);

    if (!earlier_ids.empty()) {
      if (uniform01(rng) < options.cancel_ratio) {
        ts_ns += next_interval_ns(rng);
        const OrderId target = earlier_ids[rng() % earlier_ids.size()];
        records.push_back(
            FlowRecord{FlowOp::Cancel, Side::Buy, OrderType::Limit, target, 0, 0, ts_ns});
      }
      if (uniform01(rng) < options.modify_ratio) {
        ts_ns += next_interval_ns(rng);
        const OrderId target = earlier_ids[rng() % earlier_ids.size()];
        const Price offset = uniform_span(rng, 0, 5);
        const Price new_price = clamp_price(side == Side::Buy ? mid - offset : mid + offset,
                                            config.min_price, config.max_price);
        const Qty new_qty = uniform_span(rng, 1, 100);
        records.push_back(
            FlowRecord{FlowOp::Modify, side, OrderType::Limit, target, new_price, new_qty, ts_ns});
      }
    }
  }

  lob::flowfile::FlowErrorInfo error;
  if (options.csv) {
    lob::flowfile::write_csv(options.out, records, error);
  } else {
    lob::flowfile::FlowWriter writer;
    writer.open(options.out, error);
    for (const FlowRecord& record : records) {
      if (error) {
        break;
      }
      writer.write(record, error);
    }
    if (!error) {
      writer.close(error);
    }
  }
  if (error) {
    std::fprintf(stderr, "gen: %s: %s\n", options.out.c_str(), error.message.c_str());
    return 1;
  }

  std::printf("orders: %llu\n", static_cast<unsigned long long>(options.orders));
  std::printf("records: %llu\n", static_cast<unsigned long long>(records.size()));
  std::printf("out: %s\n", options.out.c_str());
  return 0;
}

}  // namespace lobsim
