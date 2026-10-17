#include <cstdio>
#include <string_view>

#include "tools/lobsim/commands.hpp"

namespace {

constexpr std::string_view kUsage =
    "usage: lobsim <command> [options]\n"
    "\n"
    "commands:\n"
    "  gen             generate a random operation flow\n"
    "  replay          replay a flow file against an engine\n"
    "  difftest        differential-test the fast engine against the reference\n"
    "  convert-tardis  convert a Tardis incremental book file to a flow file\n"
    "\n"
    "run 'lobsim <command> --help' for command-specific options\n";

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2 || std::string_view(argv[1]) == "--help" || std::string_view(argv[1]) == "-h") {
    std::fputs(kUsage.data(), stdout);
    return 0;
  }

  const std::string_view command = argv[1];
  if (command == "gen") {
    return lobsim::run_gen(argc - 1, argv + 1);
  }
  if (command == "replay") {
    return lobsim::run_replay(argc - 1, argv + 1);
  }
  if (command == "difftest") {
    return lobsim::run_difftest(argc - 1, argv + 1);
  }
  if (command == "convert-tardis") {
    return lobsim::run_convert_tardis(argc - 1, argv + 1);
  }

  std::fprintf(stderr, "lobsim: unknown command '%s'\n", argv[1]);
  std::fputs(kUsage.data(), stderr);
  return 2;
}
