#include <cerrno>
#include <charconv>
#include <chrono>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>

#include "lob/order_book.hpp"
#include "lob/types.hpp"
#include "reference/reference_book.hpp"
#include "tools/flowfile/flowfile.hpp"
#include "tools/flowfile/replay_config.hpp"
#include "tools/lobsim/commands.hpp"

namespace lobsim {
namespace {

using lob::OrderId;
using lob::OrderType;
using lob::Price;
using lob::Qty;
using lob::RejectReason;
using lob::Side;
using lob::flowfile::FlowOp;
using lob::flowfile::FlowRecord;

constexpr std::string_view kUsage =
    "usage: lobsim replay F --engine fast|reference [options]\n"
    "\n"
    "Replays a flow file (binary, or CSV when F ends in .csv) against one\n"
    "engine and prints a key: value summary, including the submit count per\n"
    "order type. Exit codes: 0 success, 2 usage error, 1 input or output\n"
    "failure.\n"
    "\n"
    "required:\n"
    "  F                   flow file to replay\n"
    "  --engine E          'fast' or 'reference'\n"
    "\n"
    "options:\n"
    "  --digest            include the final book digest (0x hex)\n"
    "  --trades F          write one maker_id,taker_id,price,qty line per trade\n"
    "  --min-price P       config override (default 1)\n"
    "  --max-price P       config override (default 200000)\n"
    "  --max-orders N      config override (default 1048576)\n"
    "  --max-levels N      config override (default 200000)\n";

struct ReplayOptions {
  std::string input;
  std::string engine;
  bool digest = false;
  std::string trades_out;
  lob::Config config = lob::flowfile::default_replay_config();
};

struct Summary {
  std::uint64_t records = 0;
  std::uint64_t submits = 0;
  std::uint64_t cancels = 0;
  std::uint64_t modifies = 0;
  std::uint64_t limit_submits = 0;
  std::uint64_t market_submits = 0;
  std::uint64_t ioc_submits = 0;
  std::uint64_t fok_submits = 0;
  std::uint64_t post_only_submits = 0;
};

bool parse_u64(std::string_view text, std::uint64_t& out) {
  if (text.empty()) {
    return false;
  }
  const auto result = std::from_chars(text.data(), text.data() + text.size(), out);
  return result.ec == std::errc{} && result.ptr == text.data() + text.size();
}

bool parse_i64(std::string_view text, std::int64_t& out) {
  if (text.empty()) {
    return false;
  }
  const auto result = std::from_chars(text.data(), text.data() + text.size(), out);
  return result.ec == std::errc{} && result.ptr == text.data() + text.size();
}

bool parse_args(int argc, char** argv, ReplayOptions& options) {
  bool have_engine = false;

  for (int i = 1; i < argc; ++i) {
    const std::string_view flag = argv[i];
    if (flag.empty() || flag.front() != '-') {
      if (!options.input.empty()) {
        std::fprintf(stderr, "replay: unexpected extra argument '%s'\n", argv[i]);
        return false;
      }
      options.input = std::string(flag);
      continue;
    }
    if (flag == "--digest") {
      options.digest = true;
      continue;
    }
    if (i + 1 >= argc) {
      std::fprintf(stderr, "replay: '%s' needs a value\n", argv[i]);
      return false;
    }
    const std::string_view value = argv[++i];

    if (flag == "--engine") {
      options.engine = std::string(value);
      have_engine = true;
    } else if (flag == "--trades") {
      options.trades_out = std::string(value);
    } else if (flag == "--min-price") {
      if (!parse_i64(value, options.config.min_price)) {
        std::fputs("replay: --min-price needs an integer\n", stderr);
        return false;
      }
    } else if (flag == "--max-price") {
      if (!parse_i64(value, options.config.max_price)) {
        std::fputs("replay: --max-price needs an integer\n", stderr);
        return false;
      }
    } else if (flag == "--max-orders") {
      std::uint64_t parsed = 0;
      if (!parse_u64(value, parsed) || parsed > UINT32_MAX) {
        std::fputs("replay: --max-orders needs an integer in [1, 2^32-1]\n", stderr);
        return false;
      }
      options.config.max_orders = static_cast<std::uint32_t>(parsed);
    } else if (flag == "--max-levels") {
      std::uint64_t parsed = 0;
      if (!parse_u64(value, parsed) || parsed > UINT32_MAX) {
        std::fputs("replay: --max-levels needs an integer in [1, 2^32-1]\n", stderr);
        return false;
      }
      options.config.max_levels = static_cast<std::uint32_t>(parsed);
    } else {
      std::fprintf(stderr, "replay: unknown option '%s'\n", argv[i - 1]);
      return false;
    }
  }

  if (options.input.empty()) {
    std::fputs("replay: a flow file argument is required\n", stderr);
    return false;
  }
  if (!have_engine) {
    std::fputs("replay: --engine is required\n", stderr);
    return false;
  }
  if (options.engine != "fast" && options.engine != "reference") {
    std::fprintf(stderr, "replay: --engine must be 'fast' or 'reference', got '%s'\n",
                 options.engine.c_str());
    return false;
  }
  if (!lob::valid(options.config)) {
    std::fputs(
        "replay: invalid config (need 1 <= min-price <= max-price and non-zero capacities)\n",
        stderr);
    return false;
  }
  return true;
}

/// Counts events and optionally writes trades as `maker_id,taker_id,price,qty`.
class CountingListener final : public lob::Listener {
 public:
  explicit CountingListener(std::FILE* trades_file) : trades_file_(trades_file) {}

  void on_accept(OrderId) override { ++accepts_; }

  void on_reject(OrderId, RejectReason) override { ++rejects_; }

  void on_trade(OrderId maker_id, OrderId taker_id, Price price, Qty qty) override {
    ++trades_;
    traded_qty_ += qty;
    if (trades_file_ != nullptr) {
      std::fprintf(trades_file_, "%" PRIu64 ",%" PRIu64 ",%" PRId64 ",%" PRId64 "\n", maker_id,
                   taker_id, price, qty);
    }
  }

  void on_cancel(OrderId, Qty) override {}

  void on_modify(OrderId, Price, Qty) override {}

  void on_book_update(Side, Price, Qty) override {}

  std::uint64_t accepts() const { return accepts_; }
  std::uint64_t rejects() const { return rejects_; }
  std::uint64_t trades() const { return trades_; }
  Qty traded_qty() const { return traded_qty_; }

 private:
  std::FILE* trades_file_;
  std::uint64_t accepts_ = 0;
  std::uint64_t rejects_ = 0;
  std::uint64_t trades_ = 0;
  Qty traded_qty_ = 0;
};

bool has_csv_extension(std::string_view path) {
  return path.size() >= 4 && path.substr(path.size() - 4) == ".csv";
}

template <typename Book>
int replay_with(const ReplayOptions& options) {
  std::FILE* trades_file = nullptr;
  if (!options.trades_out.empty()) {
    trades_file = std::fopen(options.trades_out.c_str(), "wb");
    if (trades_file == nullptr) {
      std::fprintf(stderr, "replay: cannot open %s for writing: %s\n", options.trades_out.c_str(),
                   std::strerror(errno));
      return 1;
    }
  }

  CountingListener listener(trades_file);
  Book book(options.config, listener);

  Summary summary;
  auto apply = [&book, &summary](const FlowRecord& record) {
    switch (record.op) {
      case FlowOp::Submit:
        ++summary.submits;
        switch (record.type) {
          case OrderType::Limit:
            ++summary.limit_submits;
            break;
          case OrderType::Market:
            ++summary.market_submits;
            break;
          case OrderType::IOC:
            ++summary.ioc_submits;
            break;
          case OrderType::FOK:
            ++summary.fok_submits;
            break;
          case OrderType::PostOnly:
            ++summary.post_only_submits;
            break;
        }
        book.submit(record.id, record.side, record.type, record.price, record.qty);
        break;
      case FlowOp::Cancel:
        ++summary.cancels;
        book.cancel(record.id);
        break;
      case FlowOp::Modify:
        ++summary.modifies;
        book.modify(record.id, record.price, record.qty);
        break;
    }
    ++summary.records;
    return true;
  };

  lob::flowfile::FlowErrorInfo error;
  const auto start = std::chrono::steady_clock::now();
  if (has_csv_extension(options.input)) {
    lob::flowfile::read_csv(options.input, apply, error);
  } else {
    lob::flowfile::FlowReader reader;
    reader.open(options.input, error);
    FlowRecord record{};
    while (!error && reader.read(record, error)) {
      apply(record);
    }
  }
  const auto stop = std::chrono::steady_clock::now();

  if (trades_file != nullptr) {
    std::fclose(trades_file);
  }
  if (error) {
    std::fprintf(stderr, "replay: %s: %s\n", options.input.c_str(), error.message.c_str());
    return 1;
  }

  const auto wall_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(stop - start).count();
  const auto wall_ms = std::chrono::duration<double, std::milli>(stop - start).count();
  const double ns_per_record =
      summary.records == 0 ? 0.0
                           : static_cast<double>(wall_ns) / static_cast<double>(summary.records);

  std::printf("records: %" PRIu64 "\n", summary.records);
  std::printf("submits: %" PRIu64 "\n", summary.submits);
  std::printf("cancels: %" PRIu64 "\n", summary.cancels);
  std::printf("modifies: %" PRIu64 "\n", summary.modifies);
  std::printf("types: limit=%" PRIu64 " market=%" PRIu64 " ioc=%" PRIu64 " fok=%" PRIu64
              " post_only=%" PRIu64 "\n",
              summary.limit_submits, summary.market_submits, summary.ioc_submits,
              summary.fok_submits, summary.post_only_submits);
  std::printf("accepts: %" PRIu64 "\n", listener.accepts());
  std::printf("rejects: %" PRIu64 "\n", listener.rejects());
  std::printf("trades: %" PRIu64 "\n", listener.trades());
  std::printf("traded_qty: %" PRId64 "\n", listener.traded_qty());
  std::printf("final_orders: %zu\n", book.order_count());
  if (options.digest) {
    std::printf("digest: 0x%016" PRIx64 "\n", book.digest());
  }
  std::printf("wall_ms: %.3f\n", wall_ms);
  std::printf("ns_per_record: %.3f\n", ns_per_record);
  return 0;
}

}  // namespace

int run_replay(int argc, char** argv) {
  ReplayOptions options;
  if (!parse_args(argc, argv, options)) {
    std::fputs(kUsage.data(), stderr);
    return 2;
  }

  if (options.engine == "fast") {
    return replay_with<lob::OrderBook>(options);
  }
  return replay_with<lob::ref::ReferenceBook>(options);
}

}  // namespace lobsim
