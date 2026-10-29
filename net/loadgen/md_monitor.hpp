#pragma once

// Market-data side of the load generator: a lob::md::FeedHandler driven on its
// own thread for the duration of a load run, so the order-entry loop's pacing
// and latency measurement never wait on the UDP feed. The handler detects
// sequence gaps and asks the publisher's recovery service for the missing
// range itself; the load generator only reports its counters.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>

#include "net/md_client/feed_handler.hpp"

namespace lob::loadgen {

struct MdMonitorOptions {
  /// UDP port to receive the publisher's stream on; 0 binds an ephemeral port
  /// (tests read it back from udp_port()).
  std::uint16_t udp_port = 0;
  std::string bind_address = "127.0.0.1";
  std::string recovery_host = "127.0.0.1";
  std::uint16_t recovery_port = 9002;
  /// How long one poll waits for a datagram before checking the stop flag.
  std::chrono::milliseconds poll_interval{20};
  /// After the load, keep draining until no datagram arrived for this long.
  std::chrono::milliseconds quiet_period{100};
  /// Never drain longer than this after the load.
  std::chrono::milliseconds max_drain{1000};
};

/// The md block of the report.
struct MdSummary {
  std::uint64_t received = 0;  ///< frames read from live UDP datagrams
  std::uint64_t duplicates = 0;
  std::uint64_t gaps_detected = 0;
  std::uint64_t gaps_recovered = 0;  ///< gaps closed by the recovery service
  std::uint64_t resets = 0;          ///< SequenceReset applied
  std::uint64_t final_seq = 0;       ///< last sequence the local book is current through
  /// 1 when out-of-order frames were still buffered (a gap open) at the end,
  /// 0 for a stream that closed cleanly. The FeedHandler buffers at most one
  /// contiguous gap at a time, so this is a flag, not a count.
  std::uint64_t sequence_gaps_unrecovered = 0;
  std::uint64_t buffered_at_end = 0;
  std::uint64_t trades = 0;
  std::uint64_t dropped = 0;
  std::uint64_t malformed = 0;
  std::uint64_t recovery_requests = 0;
  std::uint64_t recovery_failures = 0;
};

/// Runs one FeedHandler on a background thread. Start it before the load,
/// finish() it after, and put the summary in the report.
class MdMonitor {
 public:
  /// Binds the UDP socket and starts the polling thread. Returns nullptr and
  /// fills `error` when the port cannot be bound.
  static std::unique_ptr<MdMonitor> start(const MdMonitorOptions& options, std::string& error);

  ~MdMonitor();

  MdMonitor(const MdMonitor&) = delete;
  MdMonitor& operator=(const MdMonitor&) = delete;

  /// The port actually bound (useful when the options asked for 0).
  std::uint16_t udp_port() const { return handler_.udp_port(); }

  /// Stops the thread, drains datagrams that were still in flight until the
  /// stream has been quiet for quiet_period (bounded by max_drain), and
  /// returns the final counters. Idempotent.
  MdSummary finish();

  const MdSummary& summary() const { return summary_; }

 private:
  explicit MdMonitor(const MdMonitorOptions& options);

  void poll_loop();

  MdMonitorOptions options_;
  lob::md::FeedHandler handler_;
  std::thread thread_;
  std::atomic<bool> stop_{false};
  MdSummary summary_;
  bool finished_ = false;
};

}  // namespace lob::loadgen
