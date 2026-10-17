#include <cstdio>

#include "tools/lobsim/commands.hpp"

namespace lobsim {

int run_convert_tardis(int, char**) {
  std::fputs("convert-tardis: not implemented\n", stderr);
  return 2;
}

}  // namespace lobsim
