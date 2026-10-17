#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
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
      : path_(std::filesystem::path(testing::TempDir()) /
              ("lobsim_e2e_fast_" + std::string(name))) {}

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
  std::istringstream input(text);
  std::string line;
  while (std::getline(input, line)) {
    const std::size_t colon = line.find(": ");
    if (colon != std::string::npos) {
      values[line.substr(0, colon)] = line.substr(colon + 2);
    }
  }
  return values;
}

/// Drops the timing lines, which legitimately differ between engines and runs.
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

std::string read_first_line(const std::filesystem::path& path) {
  std::ifstream input(path);
  std::string line;
  if (input.is_open()) {
    std::getline(input, line);
  }
  while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
    line.pop_back();
  }
  return line;
}

TEST(LobsimE2EFast, TardisSampleFastDigestMatchesGolden) {
  const auto sample = find_data_file("tardis_sample.csv.gz");
  ASSERT_TRUE(sample.has_value()) << "tests/data/tardis_sample.csv.gz not found";
  const auto digest_file = find_data_file("tardis_sample.digest");
  ASSERT_TRUE(digest_file.has_value()) << "tests/data/tardis_sample.digest not found";
  const std::string expected_digest = read_first_line(*digest_file);
  ASSERT_FALSE(expected_digest.empty());

  const TempFile converted("tardis_sample.bin");
  const Captured conversion = run_captured_stdout(
      lobsim::run_convert_tardis, {"convert-tardis", sample->string(), "--out", converted.str()});
  ASSERT_EQ(conversion.code, 0) << conversion.out;

  const Captured fast = run_captured_stdout(
      lobsim::run_replay, {"replay", converted.str(), "--engine", "fast", "--digest"});
  ASSERT_EQ(fast.code, 0);
  const Captured reference = run_captured_stdout(
      lobsim::run_replay, {"replay", converted.str(), "--engine", "reference", "--digest"});
  ASSERT_EQ(reference.code, 0);

  const auto fast_summary = parse_summary(fast.out);
  EXPECT_EQ(fast_summary.at("trades"), "0");
  EXPECT_EQ(fast_summary.at("digest"), expected_digest);
  EXPECT_EQ(fast_summary.at("final_orders"), parse_summary(reference.out).at("final_orders"));
  EXPECT_EQ(without_timing(fast.out), without_timing(reference.out));
}

TEST(LobsimE2EFast, GeneratedHundredThousandOrderFlowMatchesBothEngines) {
  const TempFile flow("generated.bin");

  const Captured generated = run_captured_stdout(
      lobsim::run_gen, {"gen", "--orders", "100000", "--seed", "42", "--out", flow.str()});
  ASSERT_EQ(generated.code, 0) << generated.out;
  EXPECT_EQ(parse_summary(generated.out).at("orders"), "100000");

  const Captured fast = run_captured_stdout(lobsim::run_replay,
                                            {"replay", flow.str(), "--engine", "fast", "--digest"});
  ASSERT_EQ(fast.code, 0);
  const Captured reference = run_captured_stdout(
      lobsim::run_replay, {"replay", flow.str(), "--engine", "reference", "--digest"});
  ASSERT_EQ(reference.code, 0);

  const auto fast_summary = parse_summary(fast.out);
  EXPECT_EQ(fast_summary.at("submits"), "100000");
  EXPECT_GT(std::stoull(fast_summary.at("trades")), 0U);
  EXPECT_EQ(without_timing(fast.out), without_timing(reference.out));
}

}  // namespace
