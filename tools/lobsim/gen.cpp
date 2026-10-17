#include <cstdio>

#include "tools/lobsim/commands.hpp"

namespace lobsim {

int run_gen(int, char**) {
  std::fputs("gen: not implemented\n", stderr);
  return 2;
}

}  // namespace lobsim
