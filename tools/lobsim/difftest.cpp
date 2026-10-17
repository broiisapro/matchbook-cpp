#include <cstdio>

#include "tools/lobsim/commands.hpp"

namespace lobsim {

int run_difftest(int, char**) {
  std::fputs("difftest: not implemented\n", stderr);
  return 2;
}

}  // namespace lobsim
