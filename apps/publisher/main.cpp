#include <chrono>
#include <csignal>
#include <cstdio>
#include <string>

#include "net/publisher/options.hpp"
#include "net/publisher/publisher.hpp"

namespace {

volatile std::sig_atomic_t g_stop = 0;

void handle_stop_signal(int) { g_stop = 1; }

}  // namespace

int main(int argc, char** argv) {
  lob::publisher::PublisherOptions options;
  std::string error;
  if (!lob::publisher::parse_publisher_options(argc, argv, options, error)) {
    std::fprintf(stderr, "lob_publisher: %s\n\n", error.c_str());
    std::fputs(lob::publisher::publisher_usage().data(), stderr);
    return 2;
  }
  if (options.help) {
    std::fputs(lob::publisher::publisher_usage().data(), stdout);
    return 0;
  }

  options.config.log_connections = true;

  lob::publisher::Publisher publisher(options.config);
  if (!publisher.open(error)) {
    std::fprintf(stderr, "lob_publisher: %s\n", error.c_str());
    return 1;
  }

  std::signal(SIGINT, handle_stop_signal);
  std::signal(SIGTERM, handle_stop_signal);

  const std::string multicast = options.config.multicast_group.has_value()
                                    ? lob::publisher::to_string(*options.config.multicast_group)
                                    : std::string("none");
  const std::string gateway = lob::publisher::to_string(options.config.gateway);
  std::fprintf(stderr,
               "lob_publisher: gateway %s, %zu UDP destination(s), multicast %s, "
               "recovery TCP port %u, retransmit %zu\n",
               gateway.c_str(), options.config.udp_destinations.size(), multicast.c_str(),
               publisher.recovery_port(), options.config.retransmit_capacity);

  while (g_stop == 0) {
    publisher.poll_once(std::chrono::milliseconds(200));
  }
  return 0;
}
