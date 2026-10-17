#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "tools/flowfile/flowfile.hpp"
#include "tools/lobsim/commands.hpp"

namespace {

using GeneFn = int (*)(int, char**);

/// Path under the gtest temp directory, removed when the test ends.
class TempFile {
 public:
  explicit TempFile(std::string_view name)
      : path_(std::filesystem::path(testing::TempDir()) / ("lobsim_" + std::string(name))) {}

  ~TempFile() {
    std::error_code ignored;
    std::filesystem::remove(path_, ignored);
  }

  const std::string str() const { return path_.string(); }
  const std::filesystem::path& path() const { return path_; }

 private:
  std::filesystem::path path_;
};

struct Captured {
  int code = 0;
  std::string out;
  std::string err;
};

Captured run_captured_stdout(GeneFn fn, std::vector<std::string> args) {
  std::vector<char*> argv;
  argv.reserve(args.size());
  for (std::string& arg : args) {
    argv.push_back(arg.data());
  }
  Captured captured;
  testing::internal::CaptureStdout();
  captured.code = fn(static_cast<int>(argv.size()), argv.data());
  captured.out = testing::internal::GetCapturedStdout();
  return captured;
}

Captured run_captured_stderr(GeneFn fn, std::vector<std::string> args) {
  std::vector<char*> argv;
  argv.reserve(args.size());
  for (std::string& arg : args) {
    argv.push_back(arg.data());
  }
  Captured captured;
  testing::internal::CaptureStderr();
  captured.code = fn(static_cast<int>(argv.size()), argv.data());
  captured.err = testing::internal::GetCapturedStderr();
  return captured;
}

std::map<std::string, std::string> parse_summary(const std::string& text) {
  std::map<std::string, std::string> values;
  std::size_t start = 0;
  while (start < text.size()) {
    const std::size_t end = text.find('\n', start);
    const std::string_view line(text.data() + start,
                                (end == std::string::npos ? text.size() : end) - start);
    const std::size_t colon = line.find(": ");
    if (colon != std::string_view::npos) {
      values[std::string(line.substr(0, colon))] = std::string(line.substr(colon + 2));
    }
    if (end == std::string::npos) {
      break;
    }
    start = end + 1;
  }
  return values;
}

std::string read_file(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

void write_text(const std::filesystem::path& path, std::string_view text) {
  std::ofstream out(path, std::ios::binary);
  ASSERT_TRUE(out.is_open());
  out.write(text.data(), static_cast<std::streamsize>(text.size()));
}

constexpr std::string_view kHandFlow =
    "op,side,type,id,price,qty,ts_ns\n"
    "submit,buy,limit,1,100,5,1000\n"
    "submit,sell,limit,2,101,3,2000\n"
    "submit,buy,limit,3,101,2,3000\n"
    "cancel,sell,limit,4,0,0,4000\n"
    "modify,buy,limit,1,99,4,5000\n";

// bids 99: [(1, 4)], asks 101: [(2, 1)], computed outside C++ from the digest
// byte spec (version byte, level fields, then FIFO orders, little-endian,
// XXH64 seed 0).
constexpr const char* kHandFlowDigest = "0x5e7d2c984ba03ef2";

TEST(LobsimTools, GenIsDeterministicForSameSeed) {
  const TempFile first("gen_first.bin");
  const TempFile second("gen_second.bin");

  const Captured run_a = run_captured_stdout(
      lobsim::run_gen, {"gen", "--orders", "1000", "--seed", "42", "--out", first.str()});
  ASSERT_EQ(run_a.code, 0) << run_a.err;
  const Captured run_b = run_captured_stdout(
      lobsim::run_gen, {"gen", "--orders", "1000", "--seed", "42", "--out", second.str()});
  ASSERT_EQ(run_b.code, 0) << run_b.err;

  EXPECT_EQ(read_file(first.path()), read_file(second.path()));

  const auto summary = parse_summary(run_a.out);
  EXPECT_EQ(summary.at("orders"), "1000");
  const std::uint64_t records = std::stoull(summary.at("records"));
  EXPECT_GE(records, 1000U);
}

TEST(LobsimTools, GenCsvHasHeaderAndRoundTripsThroughReplay) {
  const TempFile flow("gen_flow.csv");

  const Captured generated = run_captured_stdout(
      lobsim::run_gen, {"gen", "--orders", "200", "--seed", "5", "--out", flow.str(), "--csv"});
  ASSERT_EQ(generated.code, 0) << generated.err;
  const auto gen_summary = parse_summary(generated.out);
  const std::string records = gen_summary.at("records");

  const std::string text = read_file(flow.path());
  constexpr std::string_view kHeaderLine = "op,side,type,id,price,qty,ts_ns\n";
  ASSERT_GE(text.size(), kHeaderLine.size());
  EXPECT_EQ(text.substr(0, kHeaderLine.size()), kHeaderLine);

  const Captured replayed =
      run_captured_stdout(lobsim::run_replay, {"replay", flow.str(), "--engine", "reference"});
  ASSERT_EQ(replayed.code, 0) << replayed.err;
  const auto replay_summary = parse_summary(replayed.out);
  EXPECT_EQ(replay_summary.at("records"), records);
  EXPECT_EQ(replay_summary.at("submits"), "200");
  EXPECT_NE(replay_summary.at("accepts"), "0");
}

TEST(LobsimTools, ReplayReferenceMatchesHandComputedSummary) {
  const TempFile flow("hand.csv");
  write_text(flow.path(), kHandFlow);

  const Captured run = run_captured_stdout(
      lobsim::run_replay, {"replay", flow.str(), "--engine", "reference", "--digest"});
  ASSERT_EQ(run.code, 0) << run.err;
  const auto summary = parse_summary(run.out);

  EXPECT_EQ(summary.at("records"), "5");
  EXPECT_EQ(summary.at("submits"), "3");
  EXPECT_EQ(summary.at("cancels"), "1");
  EXPECT_EQ(summary.at("modifies"), "1");
  EXPECT_EQ(summary.at("accepts"), "4");
  EXPECT_EQ(summary.at("rejects"), "1");
  EXPECT_EQ(summary.at("trades"), "1");
  EXPECT_EQ(summary.at("traded_qty"), "2");
  EXPECT_EQ(summary.at("final_orders"), "2");
  EXPECT_EQ(summary.at("digest"), kHandFlowDigest);
  EXPECT_TRUE(summary.count("wall_ms") == 1);
  EXPECT_TRUE(summary.count("ns_per_record") == 1);
}

TEST(LobsimTools, ReplayWithoutDigestOmitsDigestLine) {
  const TempFile flow("nodigest.csv");
  write_text(flow.path(), kHandFlow);

  const Captured run =
      run_captured_stdout(lobsim::run_replay, {"replay", flow.str(), "--engine", "reference"});
  ASSERT_EQ(run.code, 0) << run.err;
  const auto summary = parse_summary(run.out);
  EXPECT_EQ(summary.count("digest"), 0U);
}

TEST(LobsimTools, ReplayWritesTradesCsv) {
  const TempFile flow("trades_in.csv");
  const TempFile trades("trades_out.csv");
  write_text(flow.path(), kHandFlow);

  const Captured run = run_captured_stdout(
      lobsim::run_replay,
      {"replay", flow.str(), "--engine", "reference", "--trades", trades.str()});
  ASSERT_EQ(run.code, 0) << run.err;

  // One maker 2, taker 3 fill at 101 for qty 2; data rows only.
  EXPECT_EQ(read_file(trades.path()), "2,3,101,2\n");
}

TEST(LobsimTools, ReplayBinaryFlowMatchesCsvFlow) {
  const TempFile binary("replay.bin");
  const TempFile csv("replay.csv");

  const Captured generated = run_captured_stdout(
      lobsim::run_gen, {"gen", "--orders", "300", "--seed", "9", "--out", binary.str()});
  ASSERT_EQ(generated.code, 0) << generated.err;

  const Captured as_csv = run_captured_stdout(
      lobsim::run_gen, {"gen", "--orders", "300", "--seed", "9", "--out", csv.str(), "--csv"});
  ASSERT_EQ(as_csv.code, 0) << as_csv.err;

  const Captured from_binary = run_captured_stdout(
      lobsim::run_replay, {"replay", binary.str(), "--engine", "reference", "--digest"});
  ASSERT_EQ(from_binary.code, 0) << from_binary.err;
  const Captured from_csv = run_captured_stdout(
      lobsim::run_replay, {"replay", csv.str(), "--engine", "reference", "--digest"});
  ASSERT_EQ(from_csv.code, 0) << from_csv.err;

  const auto binary_summary = parse_summary(from_binary.out);
  const auto csv_summary = parse_summary(from_csv.out);
  EXPECT_EQ(binary_summary.at("records"), csv_summary.at("records"));
  EXPECT_EQ(binary_summary.at("accepts"), csv_summary.at("accepts"));
  EXPECT_EQ(binary_summary.at("trades"), csv_summary.at("trades"));
  EXPECT_EQ(binary_summary.at("digest"), csv_summary.at("digest"));
}

TEST(LobsimTools, MissingInputFileExitsOne) {
  const TempFile missing("missing.csv");

  const Captured run =
      run_captured_stderr(lobsim::run_replay, {"replay", missing.str(), "--engine", "reference"});
  EXPECT_EQ(run.code, 1);
  EXPECT_NE(run.err.find("cannot open"), std::string::npos) << run.err;
}

TEST(LobsimTools, UsageErrorsExitTwo) {
  const TempFile flow("usage.csv");
  write_text(flow.path(), kHandFlow);

  EXPECT_EQ(run_captured_stderr(lobsim::run_gen, {"gen"}).code, 2);
  EXPECT_EQ(
      run_captured_stderr(lobsim::run_gen, {"gen", "--orders", "10", "--out", "/tmp/x.bin"}).code,
      2);
  EXPECT_EQ(run_captured_stderr(lobsim::run_gen, {"gen", "--orders", "10", "--seed", "1"}).code, 2);
  EXPECT_EQ(run_captured_stderr(lobsim::run_gen, {"gen", "--orders", "10", "--seed", "1", "--out",
                                                  "/tmp/x.bin", "--cancel-ratio", "1.5"})
                .code,
            2);
  EXPECT_EQ(run_captured_stderr(lobsim::run_gen,
                                {"gen", "--orders", "10", "--seed", "1", "--out", "/tmp/x.bin",
                                 "--market-ratio", "0.6", "--ioc-ratio", "0.6"})
                .code,
            2);

  EXPECT_EQ(run_captured_stderr(lobsim::run_replay, {"replay"}).code, 2);
  EXPECT_EQ(run_captured_stderr(lobsim::run_replay, {"replay", flow.str()}).code, 2);
  EXPECT_EQ(
      run_captured_stderr(lobsim::run_replay, {"replay", flow.str(), "--engine", "bogus"}).code, 2);
  EXPECT_EQ(run_captured_stderr(lobsim::run_replay, {"replay", flow.str(), "--engine", "reference",
                                                     "--min-price", "10", "--max-price", "5"})
                .code,
            2);
  EXPECT_EQ(run_captured_stderr(lobsim::run_replay, {"replay", flow.str(), "--engine", "reference",
                                                     "--unknown-flag", "1"})
                .code,
            2);
}

TEST(LobsimTools, FastEngineSelectionParsesTheFlow) {
  const TempFile flow("fast.csv");
  write_text(flow.path(), kHandFlow);

  const Captured run =
      run_captured_stdout(lobsim::run_replay, {"replay", flow.str(), "--engine", "fast"});
  EXPECT_EQ(run.code, 0) << run.err;
  const auto summary = parse_summary(run.out);
  EXPECT_EQ(summary.at("records"), "5");
  EXPECT_EQ(summary.at("submits"), "3");
}

}  // namespace
