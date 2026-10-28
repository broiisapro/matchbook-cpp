#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "tools/lobsim/commands.hpp"

namespace {

struct RunResult {
  int code = 0;
  std::string out;
  std::string err;
};

/// Calls run_difftest with the given argv (argv[0] is "difftest") and captures
/// both streams.
RunResult run_tool(std::vector<std::string> args) {
  std::vector<char*> argv;
  argv.reserve(args.size() + 1);
  for (std::string& arg : args) {
    argv.push_back(arg.data());
  }
  argv.push_back(nullptr);

  testing::internal::CaptureStdout();
  testing::internal::CaptureStderr();
  const int code = lobsim::run_difftest(static_cast<int>(args.size()), argv.data());
  RunResult result;
  result.code = code;
  result.out = testing::internal::GetCapturedStdout();
  result.err = testing::internal::GetCapturedStderr();
  return result;
}

bool contains(const std::string& haystack, const std::string& needle) {
  return haystack.find(needle) != std::string::npos;
}

TEST(LobsimDifftest, ReferenceAgainstReferenceReportsZeroMismatches) {
  const RunResult result = run_tool({"difftest", "--ops", "100000", "--seed", "1", "--engine-a",
                                     "reference", "--engine-b", "reference", "--progress"});

  ASSERT_EQ(result.code, 0) << result.err;
  EXPECT_TRUE(contains(result.out, "ops: 100000"));
  EXPECT_TRUE(contains(result.out, "seed: 1"));
  EXPECT_TRUE(contains(result.out, "mismatches: 0"));
  EXPECT_TRUE(contains(result.out, "wall_ms: "));
  EXPECT_TRUE(contains(result.out, "engine-a: reference"));
  EXPECT_TRUE(contains(result.out, "engine-b: reference"));
  EXPECT_TRUE(contains(result.out, "config: narrow"));
  EXPECT_TRUE(contains(result.out, "mix: default"));
  EXPECT_TRUE(contains(result.out, "max-depth-a: "));
  EXPECT_TRUE(contains(result.err, "progress: 100000 ops"));
}

TEST(LobsimDifftest, WideConfigIsAcceptedAndReported) {
  const RunResult result = run_tool({"difftest", "--ops", "2000", "--seed", "5", "--engine-a",
                                     "reference", "--engine-b", "reference", "--config", "wide"});

  ASSERT_EQ(result.code, 0) << result.err;
  EXPECT_TRUE(contains(result.out, "config: wide"));
  EXPECT_TRUE(contains(result.out, "mix: wide"));
  EXPECT_TRUE(contains(result.out, "min-price: 1"));
  EXPECT_TRUE(contains(result.out, "max-price: 200000"));
  EXPECT_TRUE(contains(result.out, "max-orders: 1048576"));
  EXPECT_TRUE(contains(result.out, "max-levels: 200000"));
  EXPECT_TRUE(contains(result.out, "mismatches: 0"));
}

TEST(LobsimDifftest, ExplicitLimitsOverrideTheConfigPresetInAnyOrder) {
  // The explicit limits must win even when they appear before --config on the
  // command line.
  const RunResult result =
      run_tool({"difftest", "--ops", "1000", "--seed", "6", "--engine-a", "reference", "--engine-b",
                "reference", "--max-orders", "8", "--min-price", "100", "--config", "wide",
                "--max-price", "110", "--max-levels", "4"});

  ASSERT_EQ(result.code, 0) << result.err;
  EXPECT_TRUE(contains(result.out, "config: wide"));
  EXPECT_TRUE(contains(result.out, "min-price: 100"));
  EXPECT_TRUE(contains(result.out, "max-price: 110"));
  EXPECT_TRUE(contains(result.out, "max-orders: 8"));
  EXPECT_TRUE(contains(result.out, "max-levels: 4"));
  EXPECT_TRUE(contains(result.out, "mismatches: 0"));
}

TEST(LobsimDifftest, LadderStressMixIsAcceptedAndReported) {
  const RunResult result =
      run_tool({"difftest", "--ops", "2000", "--seed", "3", "--engine-a", "reference", "--engine-b",
                "reference", "--mix", "ladder_stress"});

  ASSERT_EQ(result.code, 0) << result.err;
  EXPECT_TRUE(contains(result.out, "mix: ladder_stress"));
  EXPECT_TRUE(contains(result.out, "mismatches: 0"));
}

TEST(LobsimDifftest, TypesDefaultToBasicAndAllAddsPhaseTwoTypes) {
  const RunResult basic = run_tool({"difftest", "--ops", "2000", "--seed", "4", "--engine-a",
                                    "reference", "--engine-b", "reference"});
  ASSERT_EQ(basic.code, 0) << basic.err;
  EXPECT_TRUE(contains(basic.out, "types: basic"));

  const RunResult all = run_tool({"difftest", "--ops", "20000", "--seed", "4", "--engine-a",
                                  "reference", "--engine-b", "reference", "--types", "all"});
  ASSERT_EQ(all.code, 0) << all.err;
  EXPECT_TRUE(contains(all.out, "types: all"));
  EXPECT_TRUE(contains(all.out, "mismatches: 0"));
}

// --types composes with an explicit --mix instead of replacing it.
TEST(LobsimDifftest, AllTypesComposesWithAnExplicitMix) {
  const RunResult result =
      run_tool({"difftest", "--ops", "5000", "--seed", "8", "--engine-a", "reference", "--engine-b",
                "reference", "--mix", "wide", "--types", "all"});

  ASSERT_EQ(result.code, 0) << result.err;
  EXPECT_TRUE(contains(result.out, "mix: wide"));
  EXPECT_TRUE(contains(result.out, "types: all"));
  EXPECT_TRUE(contains(result.out, "mismatches: 0"));
}

TEST(LobsimDifftest, ConfigOverridesAreAccepted) {
  const RunResult result =
      run_tool({"difftest", "--ops", "1000", "--seed", "9", "--engine-a", "reference", "--engine-b",
                "reference", "--min-price", "100", "--max-price", "110", "--max-orders", "4",
                "--max-levels", "2"});

  ASSERT_EQ(result.code, 0) << result.err;
  EXPECT_TRUE(contains(result.out, "min-price: 100"));
  EXPECT_TRUE(contains(result.out, "max-price: 110"));
  EXPECT_TRUE(contains(result.out, "max-orders: 4"));
  EXPECT_TRUE(contains(result.out, "max-levels: 2"));
  EXPECT_TRUE(contains(result.out, "ops: 1000"));
  EXPECT_TRUE(contains(result.out, "mismatches: 0"));
}

TEST(LobsimDifftest, HelpExitsZeroAndPrintsUsage) {
  const RunResult result = run_tool({"difftest", "--help"});

  EXPECT_EQ(result.code, 0);
  EXPECT_TRUE(contains(result.out, "usage: lobsim difftest"));
  // The order-type switch is part of the help (wave-05 carryover).
  EXPECT_TRUE(contains(result.out, "--types"));
  EXPECT_TRUE(contains(result.out, "basic"));
  EXPECT_TRUE(contains(result.out, "all"));
}

TEST(LobsimDifftest, UsageErrorsExitWithTwo) {
  const std::vector<std::vector<std::string>> cases = {
      {"difftest"},
      {"difftest", "--ops"},
      {"difftest", "--ops", "abc", "--seed", "1"},
      {"difftest", "--ops", "10"},
      {"difftest", "--ops", "10", "--seed", "1", "--engine-a", "quantum"},
      {"difftest", "--ops", "10", "--seed", "1", "--engine-b", "quantum"},
      {"difftest", "--ops", "10", "--seed", "1", "--mix", "quantum"},
      {"difftest", "--ops", "10", "--seed", "1", "--types", "quantum"},
      {"difftest", "--ops", "10", "--seed", "1", "--config", "quantum"},
      {"difftest", "--ops", "10", "--seed", "1", "--bogus"},
      {"difftest", "--ops", "10", "--seed", "1", "--min-price", "0"},
      {"difftest", "--ops", "10", "--seed", "1", "--max-orders", "0"},
      {"difftest", "extra"},
  };

  for (const auto& args : cases) {
    const RunResult result = run_tool(args);
    EXPECT_EQ(result.code, 2) << "args: " << (args.size() > 1 ? args[1] : "");
    EXPECT_FALSE(result.err.empty());
  }
}

}  // namespace
