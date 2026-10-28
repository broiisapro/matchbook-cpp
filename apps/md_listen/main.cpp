#include <iostream>
#include <optional>
#include <string>
#include <string_view>

#include "listener.hpp"

namespace {

void print_usage(std::ostream& out) {
  out << "usage: lob_md_listen --udp-port 9100 --recovery host:9002 [--top 5] [--seconds N]\n"
      << "                      [--bind address]\n"
      << "\n"
      << "  --udp-port PORT   UDP market-data port to listen on (default 9100)\n"
      << "  --recovery ADDR   publisher recovery service host:port (default 127.0.0.1:9002)\n"
      << "  --top DEPTH       price levels printed per side (default 5)\n"
      << "  --seconds N       exit after N seconds (default: run until interrupted)\n"
      << "  --bind ADDRESS    local address for the UDP socket (default 0.0.0.0)\n"
      << "  --help            print this help\n";
}

}  // namespace

int main(int argc, char** argv) {
  if (argc == 2) {
    const std::string_view argument = argv[1];
    if (argument == "--help" || argument == "-h") {
      print_usage(std::cout);
      return 0;
    }
  }

  std::string error;
  const std::optional<lob::md::ListenerOptions> options =
      lob::md::parse_listener_options(argc, argv, error);
  if (!options) {
    std::cerr << "lob_md_listen: " << error << "\n";
    print_usage(std::cerr);
    return 2;
  }
  return lob::md::run_listener(*options, std::cout, std::cerr);
}
