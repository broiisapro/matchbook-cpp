#include <gtest/gtest.h>
#include <zlib.h>

#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "lob/types.hpp"
#include "reference/reference_book.hpp"
#include "tools/flowfile/flow_record.hpp"
#include "tools/flowfile/flowfile.hpp"
#include "tools/flowfile/replay_config.hpp"
#include "tools/flowfile/tardis.hpp"
#include "tools/lobsim/commands.hpp"

namespace {

using lob::OrderId;
using lob::OrderType;
using lob::Price;
using lob::Qty;
using lob::RejectReason;
using lob::Side;
using lob::flowfile::DecimalSpec;
using lob::flowfile::FlowOp;
using lob::flowfile::FlowReader;
using lob::flowfile::FlowRecord;
using lob::flowfile::TardisErrorCode;
using lob::flowfile::TardisErrorInfo;
using lob::flowfile::TardisOptions;

constexpr std::string_view kHeader =
    "exchange,symbol,timestamp,local_timestamp,is_snapshot,side,price,amount";

std::string row(std::string_view snapshot, std::string_view side, std::string_view price,
                std::string_view amount, std::uint64_t timestamp = 1000) {
  const std::string ts = std::to_string(timestamp);
  return "binance,ETHBTC," + ts + "," + ts + "," + std::string(snapshot) + "," + std::string(side) +
         "," + std::string(price) + "," + std::string(amount) + "\n";
}

std::string with_header(std::string_view rows) {
  return std::string(kHeader) + "\n" + std::string(rows);
}

FlowRecord submit(OrderId id, Side side, Price price, Qty qty, std::uint64_t ts_ns) {
  return FlowRecord{FlowOp::Submit, side, OrderType::Limit, id, price, qty, ts_ns};
}

FlowRecord cancel(OrderId id, Side side, std::uint64_t ts_ns) {
  return FlowRecord{FlowOp::Cancel, side, OrderType::Limit, id, 0, 0, ts_ns};
}

FlowRecord modify(OrderId id, Side side, Price price, Qty qty, std::uint64_t ts_ns) {
  return FlowRecord{FlowOp::Modify, side, OrderType::Limit, id, price, qty, ts_ns};
}

/// Counts accept/reject/trade events; used to prove a converted flow trades
/// zero times.
class CountingListener final : public lob::Listener {
 public:
  void on_accept(OrderId) override { ++accepts; }
  void on_reject(OrderId, RejectReason) override { ++rejects; }
  void on_trade(OrderId, OrderId, Price, Qty qty) override {
    ++trades;
    traded_qty += qty;
  }
  void on_cancel(OrderId, Qty) override {}
  void on_modify(OrderId, Price, Qty) override {}
  void on_book_update(Side, Price, Qty) override {}

  std::uint64_t accepts = 0;
  std::uint64_t rejects = 0;
  std::uint64_t trades = 0;
  Qty traded_qty = 0;
};

/// Replays a converted flow through the reference engine.
CountingListener replay_reference(const std::vector<FlowRecord>& records) {
  CountingListener listener;
  lob::ref::ReferenceBook book(lob::flowfile::default_replay_config(), listener);
  for (const FlowRecord& record : records) {
    switch (record.op) {
      case FlowOp::Submit:
        book.submit(record.id, record.side, record.type, record.price, record.qty);
        break;
      case FlowOp::Cancel:
        book.cancel(record.id);
        break;
      case FlowOp::Modify:
        book.modify(record.id, record.price, record.qty);
        break;
    }
  }
  return listener;
}

std::vector<FlowRecord> convert(const std::string& text, const TardisOptions& options,
                                TardisErrorInfo& error, std::uint64_t* rows = nullptr) {
  std::vector<FlowRecord> records;
  const bool ok = lob::flowfile::convert_tardis_text(
      text, options,
      [&records](const FlowRecord& record) {
        records.push_back(record);
        return true;
      },
      error, rows, nullptr);
  if (!ok) {
    records.clear();
  }
  return records;
}

const TardisOptions kWholeUnits{{1, 0}, {1, 0}, 0};

/// Path under the gtest temp directory, removed when the test ends.
class TempFile {
 public:
  explicit TempFile(std::string_view name)
      : path_(std::filesystem::path(testing::TempDir()) / ("tardis_" + std::string(name))) {}

  ~TempFile() {
    std::error_code ignored;
    std::filesystem::remove(path_, ignored);
  }

  const std::string str() const { return path_.string(); }
  const std::filesystem::path& path() const { return path_; }

 private:
  std::filesystem::path path_;
};

std::optional<std::filesystem::path> find_data_file(std::string_view name) {
  std::filesystem::path directory = std::filesystem::current_path();
  for (int depth = 0; depth < 6; ++depth) {
    const std::filesystem::path candidate = directory / "tests" / "data" / name;
    if (std::filesystem::exists(candidate)) {
      return candidate;
    }
    if (!directory.has_parent_path()) {
      break;
    }
    directory = directory.parent_path();
  }
  return std::nullopt;
}

void write_gzip(const std::filesystem::path& path, std::string_view text) {
  gzFile file = gzopen(path.string().c_str(), "wb");
  ASSERT_NE(file, nullptr);
  ASSERT_EQ(gzwrite(file, text.data(), static_cast<unsigned int>(text.size())),
            static_cast<int>(text.size()));
  ASSERT_EQ(gzclose(file), Z_OK);
}

int call_command(int (*fn)(int, char**), std::vector<std::string> args) {
  std::vector<char*> argv;
  argv.reserve(args.size());
  for (std::string& arg : args) {
    argv.push_back(arg.data());
  }
  return fn(static_cast<int>(argv.size()), argv.data());
}

// ---------------------------------------------------------------------------
// Decimal parsing
// ---------------------------------------------------------------------------

TEST(TardisDecimal, ParseSpec) {
  EXPECT_EQ(lob::flowfile::parse_decimal_spec("0.00001"), (DecimalSpec{1, 5}));
  EXPECT_EQ(lob::flowfile::parse_decimal_spec("1"), (DecimalSpec{1, 0}));
  EXPECT_EQ(lob::flowfile::parse_decimal_spec("0.0001"), (DecimalSpec{1, 4}));
  EXPECT_EQ(lob::flowfile::parse_decimal_spec("0.00002"), (DecimalSpec{2, 5}));

  EXPECT_FALSE(lob::flowfile::parse_decimal_spec("").has_value());
  EXPECT_FALSE(lob::flowfile::parse_decimal_spec("abc").has_value());
  EXPECT_FALSE(lob::flowfile::parse_decimal_spec("1.").has_value());
  EXPECT_FALSE(lob::flowfile::parse_decimal_spec(".5").has_value());
  EXPECT_FALSE(lob::flowfile::parse_decimal_spec("-1").has_value());
  EXPECT_FALSE(lob::flowfile::parse_decimal_spec("1.2.3").has_value());
  EXPECT_FALSE(lob::flowfile::parse_decimal_spec("0.0").has_value());
}

TEST(TardisDecimal, ToGridTrailingZerosDoNotChangeTheValue) {
  const DecimalSpec tick{1, 5};
  ASSERT_TRUE(lob::flowfile::decimal_to_grid("0.1", tick).has_value());
  ASSERT_TRUE(lob::flowfile::decimal_to_grid("0.10000", tick).has_value());
  EXPECT_EQ(lob::flowfile::decimal_to_grid("0.1", tick),
            lob::flowfile::decimal_to_grid("0.10000", tick));
  EXPECT_EQ(lob::flowfile::decimal_to_grid("0.1", tick), 10000);
  EXPECT_EQ(lob::flowfile::decimal_to_grid("0.03213", tick), 3213);
}

TEST(TardisDecimal, ToGridRejectsOffGridValues) {
  EXPECT_FALSE(lob::flowfile::decimal_to_grid("0.032155", DecimalSpec{1, 5}).has_value());
  EXPECT_FALSE(lob::flowfile::decimal_to_grid("1.00005", DecimalSpec{1, 4}).has_value());
  EXPECT_FALSE(lob::flowfile::decimal_to_grid("0.00003", DecimalSpec{2, 5}).has_value());
  EXPECT_FALSE(lob::flowfile::decimal_to_grid("abc", DecimalSpec{1, 5}).has_value());
}

TEST(TardisDecimal, ToGridExactConversions) {
  EXPECT_EQ(lob::flowfile::decimal_to_grid("56.065", DecimalSpec{1, 4}), 560650);
  EXPECT_EQ(lob::flowfile::decimal_to_grid("10", DecimalSpec{1, 4}), 100000);
  EXPECT_EQ(lob::flowfile::decimal_to_grid("0.00002", DecimalSpec{2, 5}), 1);
  EXPECT_EQ(lob::flowfile::decimal_to_grid("0", DecimalSpec{1, 4}), 0);
}

// ---------------------------------------------------------------------------
// Synthetic book behaviour
// ---------------------------------------------------------------------------

TEST(TardisConvert, SnapshotLoadsOneOrderPerLevel) {
  TardisErrorInfo error;
  const std::vector<FlowRecord> records =
      convert(with_header(row("true", "bid", "100", "5") + row("true", "ask", "101", "3")),
              kWholeUnits, error);
  ASSERT_FALSE(error) << error.message;
  ASSERT_EQ(records.size(), 2U);
  EXPECT_EQ(records[0], submit(1, Side::Buy, 100, 5, 1'000'000));
  EXPECT_EQ(records[1], submit(2, Side::Sell, 101, 3, 1'000'000));
}

TEST(TardisConvert, RowsBeforeTheFirstSnapshotAreSkipped) {
  TardisErrorInfo error;
  const std::vector<FlowRecord> records =
      convert(with_header(row("false", "bid", "99", "4") + row("true", "bid", "100", "5")),
              kWholeUnits, error);
  ASSERT_FALSE(error) << error.message;
  ASSERT_EQ(records.size(), 1U);
  EXPECT_EQ(records[0], submit(1, Side::Buy, 100, 5, 1'000'000));
}

TEST(TardisConvert, IncreaseSubmitsTheDeltaAtTheBack) {
  TardisErrorInfo error;
  const std::vector<FlowRecord> records =
      convert(with_header(row("true", "bid", "100", "5") + row("false", "bid", "100", "8")),
              kWholeUnits, error);
  ASSERT_FALSE(error) << error.message;
  ASSERT_EQ(records.size(), 2U);
  EXPECT_EQ(records[0], submit(1, Side::Buy, 100, 5, 1'000'000));
  EXPECT_EQ(records[1], submit(2, Side::Buy, 100, 3, 1'000'000));
}

TEST(TardisConvert, DecreaseReducesNewestOrdersFirst) {
  TardisErrorInfo error;
  const std::vector<FlowRecord> records =
      convert(with_header(row("true", "bid", "100", "5") + row("false", "bid", "100", "8") +
                          row("false", "bid", "100", "6") + row("false", "bid", "100", "4")),
              kWholeUnits, error);
  ASSERT_FALSE(error) << error.message;
  ASSERT_EQ(records.size(), 5U);
  EXPECT_EQ(records[0], submit(1, Side::Buy, 100, 5, 1'000'000));
  EXPECT_EQ(records[1], submit(2, Side::Buy, 100, 3, 1'000'000));
  EXPECT_EQ(records[2], modify(2, Side::Buy, 100, 1, 1'000'000));
  EXPECT_EQ(records[3], cancel(2, Side::Buy, 1'000'000));
  EXPECT_EQ(records[4], modify(1, Side::Buy, 100, 4, 1'000'000));
}

TEST(TardisConvert, ZeroAmountCancelsEveryOrderAtTheLevel) {
  TardisErrorInfo error;
  const std::vector<FlowRecord> records =
      convert(with_header(row("true", "ask", "101", "5") + row("false", "ask", "101", "0")),
              kWholeUnits, error);
  ASSERT_FALSE(error) << error.message;
  ASSERT_EQ(records.size(), 2U);
  EXPECT_EQ(records[0], submit(1, Side::Sell, 101, 5, 1'000'000));
  EXPECT_EQ(records[1], cancel(1, Side::Sell, 1'000'000));
}

TEST(TardisConvert, CrossingLevelsAreCancelledBeforeTheNewLevelIsAdded) {
  TardisErrorInfo error;
  const std::vector<FlowRecord> records =
      convert(with_header(row("true", "ask", "101", "5") + row("false", "bid", "101", "2")),
              kWholeUnits, error);
  ASSERT_FALSE(error) << error.message;
  ASSERT_EQ(records.size(), 3U);
  EXPECT_EQ(records[0], submit(1, Side::Sell, 101, 5, 1'000'000));
  EXPECT_EQ(records[1], cancel(1, Side::Sell, 1'000'000));
  EXPECT_EQ(records[2], submit(2, Side::Buy, 101, 2, 1'000'000));
}

TEST(TardisConvert, CrossingRemovalLeavesNoTradesOnReplay) {
  TardisErrorInfo error;
  const std::vector<FlowRecord> records =
      convert(with_header(row("true", "ask", "101", "5") + row("false", "ask", "102", "2") +
                          row("false", "bid", "103", "4")),
              kWholeUnits, error);
  ASSERT_FALSE(error) << error.message;
  const CountingListener listener = replay_reference(records);
  EXPECT_EQ(listener.trades, 0U);
  EXPECT_EQ(listener.traded_qty, 0);
}

TEST(TardisConvert, SecondSnapshotClearsAndReloadsTheSyntheticBook) {
  TardisErrorInfo error;
  const std::vector<FlowRecord> records =
      convert(with_header(row("true", "bid", "100", "5") + row("false", "bid", "100", "7") +
                          row("true", "bid", "100", "4") + row("true", "ask", "101", "2")),
              kWholeUnits, error);
  ASSERT_FALSE(error) << error.message;
  ASSERT_EQ(records.size(), 6U);
  EXPECT_EQ(records[0], submit(1, Side::Buy, 100, 5, 1'000'000));
  EXPECT_EQ(records[1], submit(2, Side::Buy, 100, 2, 1'000'000));
  // The snapshot row clears the rebuilt levels newest-first...
  EXPECT_EQ(records[2], cancel(2, Side::Buy, 1'000'000));
  EXPECT_EQ(records[3], cancel(1, Side::Buy, 1'000'000));
  // ...and then loads the snapshot levels as fresh orders.
  EXPECT_EQ(records[4], submit(3, Side::Buy, 100, 4, 1'000'000));
  EXPECT_EQ(records[5], submit(4, Side::Sell, 101, 2, 1'000'000));
}

TEST(TardisConvert, TimestampsAreMicrosecondsTimesThousand) {
  TardisErrorInfo error;
  const std::vector<FlowRecord> records =
      convert(with_header(row("true", "bid", "100", "5", 1790812800607004ULL)), kWholeUnits, error);
  ASSERT_FALSE(error) << error.message;
  ASSERT_EQ(records.size(), 1U);
  EXPECT_EQ(records[0].ts_ns, 1790812800607004000ULL);
}

TEST(TardisConvert, MaxRowsStopsAfterTheGivenRows) {
  TardisErrorInfo error;
  TardisOptions options = kWholeUnits;
  options.max_rows = 2;
  std::uint64_t rows = 0;
  const std::vector<FlowRecord> records =
      convert(with_header(row("true", "bid", "100", "5") + row("false", "bid", "100", "7") +
                          row("false", "bid", "100", "3")),
              options, error, &rows);
  ASSERT_FALSE(error) << error.message;
  EXPECT_EQ(rows, 2U);
  ASSERT_EQ(records.size(), 2U);
  EXPECT_EQ(records[0], submit(1, Side::Buy, 100, 5, 1'000'000));
  EXPECT_EQ(records[1], submit(2, Side::Buy, 100, 2, 1'000'000));
}

// ---------------------------------------------------------------------------
// Errors
// ---------------------------------------------------------------------------

TEST(TardisConvert, ErrorCasesReportLineNumbers) {
  {
    TardisErrorInfo error;
    const std::vector<FlowRecord> records = convert("wrong,header\n", kWholeUnits, error);
    EXPECT_EQ(error.code, TardisErrorCode::BadHeader);
    EXPECT_TRUE(records.empty());
  }
  {
    TardisErrorInfo error;
    convert(with_header("binance,ETHBTC,1000,1000,true,bid,100\n"), kWholeUnits, error);
    EXPECT_EQ(error.code, TardisErrorCode::BadRow);
    EXPECT_NE(error.message.find("line 2"), std::string::npos) << error.message;
  }
  {
    TardisErrorInfo error;
    convert(with_header(row("true", "bid", "100", "5") + "binance,ETHBTC,x,1000,false,bid,100,5\n"),
            kWholeUnits, error);
    EXPECT_EQ(error.code, TardisErrorCode::BadTimestamp);
    EXPECT_NE(error.message.find("line 3"), std::string::npos) << error.message;
  }
  {
    TardisErrorInfo error;
    convert(with_header(row("true", "middle", "100", "5")), kWholeUnits, error);
    EXPECT_EQ(error.code, TardisErrorCode::UnknownSide);
  }
  {
    TardisErrorInfo error;
    convert(with_header(row("true", "bid", "100", "5") + row("false", "bid", "100.5", "6")),
            kWholeUnits, error);
    EXPECT_EQ(error.code, TardisErrorCode::OffGridPrice);
    EXPECT_NE(error.message.find("line 3"), std::string::npos) << error.message;
  }
  {
    TardisErrorInfo error;
    convert(with_header(row("true", "bid", "100", "5") + row("false", "bid", "100", "6.5")),
            kWholeUnits, error);
    EXPECT_EQ(error.code, TardisErrorCode::OffGridAmount);
    EXPECT_NE(error.message.find("line 3"), std::string::npos) << error.message;
  }
  {
    TardisErrorInfo error;
    convert(with_header(row("false", "bid", "100", "5")), kWholeUnits, error);
    EXPECT_EQ(error.code, TardisErrorCode::NoSnapshot);
  }
  {
    TardisErrorInfo error;
    convert(with_header(row("maybe", "bid", "100", "5")), kWholeUnits, error);
    EXPECT_EQ(error.code, TardisErrorCode::BadSnapshotFlag);
  }
}

TEST(TardisConvert, OffGridPriceWithDefaultTick) {
  TardisErrorInfo error;
  convert(with_header(row("true", "bid", "0.032155", "5")), TardisOptions{}, error);
  EXPECT_EQ(error.code, TardisErrorCode::OffGridPrice);
  EXPECT_NE(error.message.find("0.032155"), std::string::npos) << error.message;
}

// ---------------------------------------------------------------------------
// CLI
// ---------------------------------------------------------------------------

TEST(TardisConvert, CliConvertsGzippedFile) {
  const TempFile gz("cli.csv.gz");
  const TempFile out("cli.bin");
  const std::string text =
      with_header(row("true", "bid", "100", "5") + row("false", "ask", "101", "3"));
  write_gzip(gz.path(), text);

  const int code =
      call_command(lobsim::run_convert_tardis,
                   {"convert-tardis", gz.str(), "--out", out.str(), "--tick", "1", "--lot", "1"});
  ASSERT_EQ(code, 0);

  lob::flowfile::FlowErrorInfo error;
  FlowReader reader;
  reader.open(out.str(), error);
  ASSERT_FALSE(error) << error.message;
  std::vector<FlowRecord> records;
  FlowRecord record{};
  while (reader.read(record, error)) {
    records.push_back(record);
  }
  ASSERT_FALSE(error) << error.message;
  ASSERT_EQ(records.size(), 2U);
  EXPECT_EQ(records[0], submit(1, Side::Buy, 100, 5, 1'000'000));
  EXPECT_EQ(records[1], submit(2, Side::Sell, 101, 3, 1'000'000));
}

TEST(TardisConvert, CliUsageErrorsExitTwo) {
  EXPECT_EQ(call_command(lobsim::run_convert_tardis, {"convert-tardis"}), 2);
  EXPECT_EQ(call_command(lobsim::run_convert_tardis, {"convert-tardis", "in.csv.gz"}), 2);
  EXPECT_EQ(call_command(lobsim::run_convert_tardis,
                         {"convert-tardis", "in.csv.gz", "--out", "out.bin", "--tick", "abc"}),
            2);
  EXPECT_EQ(call_command(lobsim::run_convert_tardis,
                         {"convert-tardis", "in.csv.gz", "--out", "out.bin", "--bogus", "1"}),
            2);
  EXPECT_EQ(call_command(lobsim::run_convert_tardis,
                         {"convert-tardis", "missing.csv.gz", "--out", "out.bin"}),
            1);
}

// ---------------------------------------------------------------------------
// Committed Tardis sample
// ---------------------------------------------------------------------------

TEST(TardisSample, ReferenceDigestIsGoldenAndThereAreNoTrades) {
  const auto sample = find_data_file("tardis_sample.csv.gz");
  ASSERT_TRUE(sample.has_value()) << "tests/data/tardis_sample.csv.gz not found";
  const auto digest_file = find_data_file("tardis_sample.digest");
  ASSERT_TRUE(digest_file.has_value()) << "tests/data/tardis_sample.digest not found";

  std::string text;
  TardisErrorInfo error;
  ASSERT_TRUE(lob::flowfile::read_gzip_file(sample->string(), text, error)) << error.message;

  std::uint64_t rows = 0;
  std::uint64_t records = 0;
  std::vector<FlowRecord> converted;
  ASSERT_TRUE(lob::flowfile::convert_tardis_text(
      text, TardisOptions{},
      [&converted](const FlowRecord& record) {
        converted.push_back(record);
        return true;
      },
      error, &rows, &records))
      << error.message;
  EXPECT_GT(rows, 10000U);
  EXPECT_GT(records, 10000U);

  CountingListener listener;
  lob::ref::ReferenceBook book(lob::flowfile::default_replay_config(), listener);
  for (const FlowRecord& record : converted) {
    switch (record.op) {
      case FlowOp::Submit:
        book.submit(record.id, record.side, record.type, record.price, record.qty);
        break;
      case FlowOp::Cancel:
        book.cancel(record.id);
        break;
      case FlowOp::Modify:
        book.modify(record.id, record.price, record.qty);
        break;
    }
  }
  EXPECT_EQ(listener.trades, 0U);
  EXPECT_EQ(listener.traded_qty, 0);

  char formatted[32];
  std::snprintf(formatted, sizeof(formatted), "0x%016" PRIx64, book.digest());

  std::ifstream input(*digest_file);
  std::string expected;
  ASSERT_TRUE(input.is_open());
  std::getline(input, expected);
  while (!expected.empty() && (expected.back() == '\r' || expected.back() == ' ')) {
    expected.pop_back();
  }
  EXPECT_EQ(std::string(formatted), expected);
}

}  // namespace
