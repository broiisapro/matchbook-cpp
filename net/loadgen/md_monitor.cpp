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
  config.auto_recovery = options.auto_recovery;
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
  summary.heartbeats = counters.heartbeats;
  summary.trades = counters.trades;
  summary.dropped = counters.dropped;
  summary.malformed = counters.malformed;
  summary.recovery_requests = counters.recovery_requests;
  summary.recovery_failures = counters.recovery_failures;
  summary.final_seq = handler.last_seen_seq();
  summary.highest_known_seq = handler.highest_known_seq();
  summary.unrecovered_gaps = summary.highest_known_seq > summary.final_seq
                                 ? summary.highest_known_seq - summary.final_seq
                                 : 0;
  summary.buffered_at_end = handler.buffered();
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

  // The load just ended, but the publisher may still be forwarding events and
  // a final datagram may have been lost. Keep draining until the local book has
  // applied everything the stream announced; a trailing loss is discovered by
  // the gateway's idle heartbeat, which carries the last published sequence
  // number, and repaired over TCP. The settle budget bounds the wait so a
  // publisher that stops talking cannot hold the report open, and a quiet
  // stream ends it early.
  const auto deadline = Clock::now() + options_.settle_timeout;
  auto quiet_at = Clock::now() + options_.quiet_period;
  const std::uint64_t heartbeats_at_start = handler_.counters().heartbeats;
  while (Clock::now() < deadline) {
    if (handler_.poll(5) > 0) {
      quiet_at = Clock::now() + options_.quiet_period;
      continue;
    }
    if (handler_.last_seen_seq() < handler_.highest_known_seq()) {
      continue;  // still behind the stream: keep draining and recovering
    }
    // Complete. A heartbeat received since the load ended proves the stream
    // itself has gone idle and announced its last sequence number, which is a
    // stronger signal than a quiet socket; otherwise wait out the quiet period.
    const bool stream_announced_idle = handler_.counters().heartbeats > heartbeats_at_start;
    if (stream_announced_idle || Clock::now() >= quiet_at) {
      break;
    }
  }

  summary_ = summarize(handler_);
  finished_ = true;
  return summary_;
}

}  // namespace lob::loadgen
