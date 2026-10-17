#include <cstdio>

#include "tools/lobsim/commands.hpp"

namespace lobsim {

int run_replay(int, char**) {
  std::fputs("replay: not implemented\n", stderr);
  return 2;
}

}  // namespace lobsim
