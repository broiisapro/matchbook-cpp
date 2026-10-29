#include "net/loadgen/md_monitor.hpp"

#include <chrono>
#include <string>
#include <utility>

namespace lob::loadgen {
namespace {

using Clock = std::chrono::steady_clock;

lob::md::FeedHandlerConfig handler_config(const MdMonitorOptions& options) {
  lob::md::FeedHandlerConfig config;
  config.udp_port = options.udp_port;
  config.bind_address = options.bind_address;
  config.recovery_host = options.recovery_host;
  config.recovery_port = options.recovery_port;
  return config;
}

MdSummary summarize(const lob::md::FeedHandler& handler) {
  const lob::md::FeedCounters& counters = handler.counters();
  MdSummary summary;
  summary.received = counters.received;
  summary.duplicates = counters.duplicates;
  summary.gaps_detected = counters.gaps_detected;
  summary.gaps_recovered = counters.gaps_recovered;
  summary.resets = counters.resets;
  summary.trades = counters.trades;
  summary.dropped = counters.dropped;
  summary.malformed = counters.malformed;
  summary.recovery_requests = counters.recovery_requests;
  summary.recovery_failures = counters.recovery_failures;
  summary.final_seq = handler.expected_seq() > 0 ? handler.expected_seq() - 1 : 0;
  summary.buffered_at_end = handler.buffered();
  summary.sequence_gaps_unrecovered = handler.buffered() > 0 ? 1 : 0;
  return summary;
}

}  // namespace

MdMonitor::MdMonitor(const MdMonitorOptions& options)
    : options_(options), handler_(handler_config(options)) {}

MdMonitor::~MdMonitor() {
  stop_.store(true);
  if (thread_.joinable()) {
    thread_.join();
  }
}

std::unique_ptr<MdMonitor> MdMonitor::start(const MdMonitorOptions& options, std::string& error) {
  std::unique_ptr<MdMonitor> monitor(new MdMonitor(options));
  if (!monitor->handler_.open(error)) {
    error = "md feed: " + error;
    return nullptr;
  }
  monitor->thread_ = std::thread([pointer = monitor.get()]() { pointer->poll_loop(); });
  return monitor;
}

void MdMonitor::poll_loop() {
  const int timeout_ms = static_cast<int>(options_.poll_interval.count());
  while (!stop_.load()) {
    (void)handler_.poll(timeout_ms);
  }
}

MdSummary MdMonitor::finish() {
  if (finished_) {
    return summary_;
  }
  stop_.store(true);
  if (thread_.joinable()) {
    thread_.join();
  }

  // The load just ended, but datagrams (and recovery replies) may still be on
  // their way. Keep draining until nothing has arrived for quiet_period, with
  // max_drain as the upper bound so a chatty publisher cannot hold the report
  // open forever.
  const auto deadline = Clock::now() + options_.max_drain;
  auto quiet_at = Clock::now() + options_.quiet_period;
  while (Clock::now() < deadline) {
    if (handler_.poll(5) > 0) {
      quiet_at = Clock::now() + options_.quiet_period;
    } else if (Clock::now() >= quiet_at) {
      break;
    }
  }

  summary_ = summarize(handler_);
  finished_ = true;
  return summary_;
}

}  // namespace lob::loadgen
