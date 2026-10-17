#pragma once

// Subcommand entry points for lobsim. Each command lives in its own
// translation unit; main.cpp only dispatches. argv[0] is the command name
// (e.g. "gen"), so a command's usage text can use it directly.

namespace lobsim {

int run_gen(int argc, char** argv);
int run_replay(int argc, char** argv);
int run_difftest(int argc, char** argv);
int run_convert_tardis(int argc, char** argv);

}  // namespace lobsim
