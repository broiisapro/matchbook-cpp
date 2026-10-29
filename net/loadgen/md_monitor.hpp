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
  /// Local address to bind the UDP socket to. The default accepts traffic on
  /// every interface, so loopback and a container's bridge address both work.
  std::string bind_address = "0.0.0.0";
  std::string recovery_host = "127.0.0.1";
  std::uint16_t recovery_port = 9002;
  /// How long one poll waits for a datagram before checking the stop flag.
  std::chrono::milliseconds poll_interval{20};
  /// Stop settling as soon as the stream has been quiet this long, so a clean
  /// run does not wait for the whole settle budget.
  std::chrono::milliseconds quiet_period{100};
  /// After the load, keep draining until the local book has applied everything
  /// the stream announced (highest_known_seq), bounded by this. A lost final
  /// datagram is detected through the gateway's idle heartbeat, which is why
  /// this is a couple of heartbeat intervals rather than a fixed sleep.
  std::chrono::milliseconds settle_timeout{2000};
  /// Ask the recovery service for gaps. Off makes the monitor report a gap it
  /// cannot close, which is useful for exercising the completeness counters.
  bool auto_recovery = true;
};

/// The md block of the report.
struct MdSummary {
  std::uint64_t received = 0;  ///< frames read from live UDP datagrams
  std::uint64_t duplicates = 0;
  std::uint64_t gaps_detected = 0;
  std::uint64_t gaps_recovered = 0;  ///< gaps closed by the recovery service
  std::uint64_t resets = 0;          ///< SequenceReset applied
  std::uint64_t heartbeats = 0;      ///< idle stream heartbeats received
  std::uint64_t final_seq = 0;       ///< last sequence the local book is current through
  /// Highest sequence the stream announced (from events, buffered frames or an
  /// idle heartbeat). `final_seq >= highest_known_seq` means the local book is
  /// complete.
  std::uint64_t highest_known_seq = 0;
  /// Sequence numbers the stream announced but the local book never applied,
  /// including a trailing gap that no later event followed. 0 means complete.
  std::uint64_t unrecovered_gaps = 0;
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

  /// Stops the thread and keeps draining the feed after the load until the
  /// local book has applied `highest_known_seq()` (a trailing lost datagram is
  /// discovered through the idle heartbeat and recovered), the stream has been
  /// quiet for quiet_period, or settle_timeout expires. Returns the final
  /// counters. Idempotent.
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
