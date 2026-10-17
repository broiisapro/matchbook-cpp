#include <gtest/gtest.h>

#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "tools/lobsim/commands.hpp"

namespace {

using GeneFn = int (*)(int, char**);

/// Path under the gtest temp directory, removed when the test ends.
class TempFile {
 public:
  explicit TempFile(std::string_view name)
      : path_(std::filesystem::path(testing::TempDir()) / ("lobsim_e2e_" + std::string(name))) {}

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

/// Drops the two timing lines, which legitimately differ between runs.
std::string without_timing(const std::string& summary) {
  std::istringstream input(summary);
  std::string kept;
  std::string line;
  while (std::getline(input, line)) {
    if (line.rfind("wall_ms:", 0) == 0 || line.rfind("ns_per_record:", 0) == 0) {
      continue;
    }
    kept += line;
    kept += '\n';
  }
  return kept;
}

std::uint64_t count_lines(const std::filesystem::path& path) {
  std::ifstream input(path);
  std::uint64_t lines = 0;
  std::string line;
  while (std::getline(input, line)) {
    ++lines;
  }
  return lines;
}

TEST(LobsimE2E, GenThenReplayReferenceTwiceIsDeterministic) {
  const TempFile flow("determinism.bin");

  const Captured generated = run_captured_stdout(
      lobsim::run_gen, {"gen", "--orders", "500", "--seed", "77", "--out", flow.str()});
  ASSERT_EQ(generated.code, 0);

  const Captured first = run_captured_stdout(
      lobsim::run_replay, {"replay", flow.str(), "--engine", "reference", "--digest"});
  ASSERT_EQ(first.code, 0);
  const Captured second = run_captured_stdout(
      lobsim::run_replay, {"replay", flow.str(), "--engine", "reference", "--digest"});
  ASSERT_EQ(second.code, 0);

  EXPECT_EQ(without_timing(first.out), without_timing(second.out));
  EXPECT_NE(first.out.find("digest: 0x"), std::string::npos);
}

TEST(LobsimE2E, BinaryAndCsvFormsOfTheSameFlowGiveTheSameDigest) {
  const TempFile binary("same.bin");
  const TempFile csv("same.csv");

  const Captured as_binary = run_captured_stdout(
      lobsim::run_gen, {"gen", "--orders", "400", "--seed", "123", "--out", binary.str()});
  ASSERT_EQ(as_binary.code, 0);
  const Captured as_csv = run_captured_stdout(
      lobsim::run_gen, {"gen", "--orders", "400", "--seed", "123", "--out", csv.str(), "--csv"});
  ASSERT_EQ(as_csv.code, 0);

  const Captured from_binary = run_captured_stdout(
      lobsim::run_replay, {"replay", binary.str(), "--engine", "reference", "--digest"});
  ASSERT_EQ(from_binary.code, 0);
  const Captured from_csv = run_captured_stdout(
      lobsim::run_replay, {"replay", csv.str(), "--engine", "reference", "--digest"});
  ASSERT_EQ(from_csv.code, 0);

  const auto binary_summary = parse_summary(from_binary.out);
  const auto csv_summary = parse_summary(from_csv.out);
  EXPECT_EQ(binary_summary.at("records"), csv_summary.at("records"));
  EXPECT_EQ(binary_summary.at("accepts"), csv_summary.at("accepts"));
  EXPECT_EQ(binary_summary.at("trades"), csv_summary.at("trades"));
  EXPECT_EQ(binary_summary.at("digest"), csv_summary.at("digest"));
}

TEST(LobsimE2E, TradesCsvRowCountEqualsTradesSummaryLine) {
  const TempFile flow("trades.bin");
  const TempFile trades("trades.csv");

  const Captured generated = run_captured_stdout(
      lobsim::run_gen, {"gen", "--orders", "300", "--seed", "55", "--out", flow.str()});
  ASSERT_EQ(generated.code, 0);

  const Captured replayed = run_captured_stdout(
      lobsim::run_replay,
      {"replay", flow.str(), "--engine", "reference", "--digest", "--trades", trades.str()});
  ASSERT_EQ(replayed.code, 0);

  const auto summary = parse_summary(replayed.out);
  const std::uint64_t trades_count = std::stoull(summary.at("trades"));
  ASSERT_GT(trades_count, 0U);
  EXPECT_EQ(count_lines(trades.path()), trades_count);
}

TEST(LobsimE2E, DigestIsPrintedAsSixteenHexDigits) {
  const TempFile flow("digest_format.csv");
  {
    std::ofstream out(flow.path(), std::ios::binary);
    ASSERT_TRUE(out.is_open());
    out << "op,side,type,id,price,qty,ts_ns\n"
        << "submit,buy,limit,1,100,5,1\n";
  }

  const Captured replayed = run_captured_stdout(
      lobsim::run_replay, {"replay", flow.str(), "--engine", "reference", "--digest"});
  ASSERT_EQ(replayed.code, 0);

  const std::string digest = parse_summary(replayed.out).at("digest");
  ASSERT_EQ(digest.size(), 18U);
  EXPECT_EQ(digest.substr(0, 2), "0x");
  for (std::size_t i = 2; i < digest.size(); ++i) {
    EXPECT_TRUE(std::isxdigit(static_cast<unsigned char>(digest[i]))) << digest;
  }
}

}  // namespace
