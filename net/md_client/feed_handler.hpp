#pragma once

// Market-data feed handler (Phase 3).
//
// A FeedHandler subscribes to the publisher's UDP event stream, turns the
// frames back into level updates and trades, keeps a local price-level book and
// repairs gaps in the publisher's sequence by asking the recovery service over
// TCP for the missing range. It owns no threads: poll() reads at most one
// datagram and performs any needed recovery inline, so a caller can drive it
// from its own loop without shared state.
//
// Sequence contract (docs/architecture.md §14.2): every BookUpdate and Trade
// carries the publisher's gap-free sequence number, starting at 1. A
// SequenceReset names the next number to expect. Frames that arrive ahead of
// the expected number are buffered (up to
// FeedHandlerConfig::max_buffered_messages) and applied as soon as the hole
// closes, either because the datagrams show up late or because the recovery
// service re-sends them.
//
// Heartbeats (decision C57) let a subscriber notice a trailing gap: they carry
// the sequence number of the last published event, so a heartbeat above the
// last applied number opens a hole even when no later event ever arrives. A
// heartbeat never changes the book and never carries a sequence number of its
// own.

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "lob/types.hpp"
#include "net/protocol/messages.hpp"

namespace lob::md {

/// One price level of the local book.
struct Level {
  Price price = 0;
  Qty qty = 0;

  friend bool operator==(const Level&, const Level&) = default;
};

/// Local price-level book built from BookUpdate messages. Bids are kept by
/// ascending price and walked in reverse for the best bid; asks ascend from the
/// best ask. A quantity of zero removes the level, matching the wire contract.
class LevelBook {
 public:
  void apply(const net::BookUpdate& update);
  void clear();

  bool empty() const { return bids_.empty() && asks_.empty(); }
  std::size_t bid_levels() const { return bids_.size(); }
  std::size_t ask_levels() const { return asks_.size(); }

  /// Quantity resting at the level, or nullopt when the level is absent.
  std::optional<Qty> quantity_at(Side side, Price price) const;

  /// The best `count` levels, best price first. Shorter than `count` when the
  /// book has fewer levels.
  std::vector<Level> top(Side side, std::size_t count) const;

 private:
  std::map<Price, Qty> bids_;
  std::map<Price, Qty> asks_;
};

struct FeedHandlerConfig {
  /// UDP port to receive on; 0 asks the kernel for an ephemeral port (tests).
  std::uint16_t udp_port = 9100;
  /// Local address to bind the UDP socket to.
  std::string bind_address = "127.0.0.1";
  /// Recovery service to ask for missing frames.
  std::string recovery_host = "127.0.0.1";
  std::uint16_t recovery_port = 9002;
  /// Out-of-order frames held while a gap is open. Beyond this the frame is
  /// dropped and counted.
  std::size_t max_buffered_messages = 4096;
  /// Largest range asked for in one RecoveryRequest. The handler issues more
  /// requests while the gap is still open, so this only bounds one response.
  std::size_t max_recovery_range = 1024;
  /// How long one recovery exchange may take before the handler gives up and
  /// retries later.
  int recovery_timeout_ms = 1000;
  /// When false the handler still detects and buffers gaps but never asks the
  /// recovery service. Useful for exercising the reorder buffer in isolation.
  bool auto_recovery = true;
};

/// Observable counters. `received` counts frames read from live UDP datagrams
/// (duplicates included); recovered frames are reported in `recovery_messages`
/// so a huge recovery does not inflate the live-stream rate. `gaps_detected`
/// counts distinct missing-range episodes and `gaps_recovered` those closed by
/// the recovery service; a gap that fills from late UDP datagrams alone is
/// detected but not recovered.
struct FeedCounters {
  std::uint64_t datagrams = 0;
  std::uint64_t received = 0;
  std::uint64_t duplicates = 0;
  std::uint64_t gaps_detected = 0;
  std::uint64_t gaps_recovered = 0;
  std::uint64_t resets = 0;
  std::uint64_t recovery_requests = 0;
  std::uint64_t recovery_failures = 0;
  std::uint64_t recovery_messages = 0;
  std::uint64_t dropped = 0;
  std::uint64_t malformed = 0;
  std::uint64_t trades = 0;
  std::uint64_t heartbeats = 0;

  friend bool operator==(const FeedCounters&, const FeedCounters&) = default;
};

/// UDP feed handler with a local book and TCP gap recovery. Not thread-safe;
/// drive it from one thread (the md_listen loop or a test).
class FeedHandler {
 public:
  explicit FeedHandler(FeedHandlerConfig config = {});
  ~FeedHandler();

  FeedHandler(const FeedHandler&) = delete;
  FeedHandler& operator=(const FeedHandler&) = delete;
  FeedHandler(FeedHandler&&) = delete;
  FeedHandler& operator=(FeedHandler&&) = delete;

  /// Creates and binds the UDP socket. Returns false and fills `error` when the
  /// address cannot be bound. Calling it twice is harmless.
  bool open(std::string& error);
  void close();
  bool is_open() const { return fd_ >= 0; }

  /// Port actually bound, useful when the config asked for port 0.
  std::uint16_t udp_port() const { return bound_port_; }

  /// Waits up to `timeout_ms` for one datagram and processes it, including any
  /// recovery the datagram opens a need for. Returns 1 when a datagram was
  /// processed and 0 on timeout or socket error.
  std::size_t poll(int timeout_ms);

  /// Processes one datagram's bytes. Exposed for tests and for callers that
  /// receive datagrams themselves; a datagram holds one or more complete
  /// frames.
  void process_datagram(std::span<const std::byte> datagram);

  const LevelBook& book() const { return book_; }
  const FeedCounters& counters() const { return counters_; }

  /// Next publisher sequence number the handler expects.
  net::Seq expected_seq() const { return expected_seq_; }

  /// Last sequence number applied to the local book: 0 before the first event,
  /// after which it is `expected_seq() - 1`. The book is current through this
  /// number with no holes.
  net::Seq last_seen_seq() const { return expected_seq_ > 0 ? expected_seq_ - 1 : 0; }

  /// Highest sequence number the stream has told the handler about, whether
  /// from an applied frame, a buffered frame or a heartbeat. Equals
  /// `last_seen_seq()` when the local book is complete; a larger value means
  /// the stream published events the handler has not applied yet (a gap that
  /// may still be recovered). Never decreases.
  net::Seq highest_known_seq() const { return highest_known_seq_; }

  std::size_t buffered() const { return buffered_.size(); }

 private:
  void handle_frame(std::span<const std::byte> frame);
  void accept_sequenced(net::Seq seq, std::span<const std::byte> frame);
  void apply_frame_now(std::span<const std::byte> frame);
  void apply_reset(net::Seq next_seq);
  void drain_buffered();
  void maybe_recover();
  bool fetch_range(net::Seq from, net::Seq to);

  /// Highest sequence number that bounds the recovery range: the highest frame
  /// seen, whether buffered or applied, and the highest heartbeat.
  net::Seq highest_known() const;

  FeedHandlerConfig config_;
  int fd_ = -1;
  std::uint16_t bound_port_ = 0;

  LevelBook book_;
  FeedCounters counters_;

  net::Seq expected_seq_ = 1;
  net::Seq highest_known_seq_ = 0;
  net::Seq recovery_request_seq_ = 0;
  /// Frames received ahead of expected_seq_, keyed by their sequence number.
  std::map<net::Seq, std::vector<std::byte>> buffered_;
  bool gap_open_ = false;
  /// Datagrams left before a failed recovery may be retried.
  std::uint32_t recovery_retry_in_ = 0;
};

}  // namespace lob::md
