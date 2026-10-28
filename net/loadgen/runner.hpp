#pragma once

// The load generator's client runner: N closed-loop sessions, each with at
// most one request in flight, driven by one thread that also paces the send
// rate and waits on every socket. Wire-to-wire latency is measured from the
// timestamp taken immediately before send() to the moment the first response
// that references the request is decoded (see docs in the report).

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "lob/types.hpp"
#include "net/loadgen/latency.hpp"
#include "net/loadgen/order_flow.hpp"
#include "net/loadgen/rate_limiter.hpp"
#include "net/loadgen/transport.hpp"
#include "net/protocol/messages.hpp"

namespace lob::loadgen {

struct RunnerOptions {
  /// Total requests to send across all sessions.
  std::uint64_t orders = 0;
  std::uint64_t seed = 1;
  /// Target send rate in requests per second across all sessions; 0 means as
  /// fast as the loop allows.
  double rate = 0.0;
  /// A request still unanswered this long after its send ends the run.
  std::chrono::milliseconds response_timeout{5000};
  /// Price bounds handed to the order-flow generator (the gateway's config
  /// defaults, decision C24).
  lob::Price min_price = 1;
  lob::Price max_price = 200000;
};

/// Counts of the four response message types a gateway can send.
struct ResponseCounts {
  std::uint64_t ack = 0;
  std::uint64_t reject = 0;
  std::uint64_t fill = 0;
  std::uint64_t cancelled = 0;

  std::uint64_t total() const { return ack + reject + fill + cancelled; }

  /// Adds one message of `type`; false when the type is not a response.
  bool add(lob::net::MessageType type);
};

struct RunStats {
  std::uint64_t orders_requested = 0;
  std::uint64_t requests_sent = 0;
  std::uint64_t responses_received = 0;
  std::uint64_t first_responses = 0;
  /// Response frames that did not complete a request (for example a Fill for
  /// one of our orders resting in the book).
  std::uint64_t unsolicited_responses = 0;
  std::uint64_t send_errors = 0;
  std::uint64_t protocol_errors = 0;
  bool timed_out = false;
  double elapsed_seconds = 0.0;
  double requests_per_second = 0.0;

  /// Type of the response that completed each request.
  ResponseCounts first_response_by_type;
  /// Every response frame received, including unsolicited ones.
  ResponseCounts responses_by_type;

  std::uint64_t orders_without_response() const {
    return orders_requested > first_responses ? orders_requested - first_responses : 0;
  }
};

struct RunResult {
  RunStats stats;
  /// Empty when the run completed normally.
  std::string error;

  /// True when every requested request was sent and answered.
  bool ok() const {
    return error.empty() && !stats.timed_out && stats.orders_without_response() == 0;
  }
};

class LoadRunner {
 public:
  /// Takes ownership of one transport per session. `latency` outlives the
  /// runner and receives one sample per answered request.
  LoadRunner(std::vector<std::unique_ptr<OrderEntryTransport>> transports, RunnerOptions options,
             LatencySink& latency);
  ~LoadRunner();

  LoadRunner(const LoadRunner&) = delete;
  LoadRunner& operator=(const LoadRunner&) = delete;

  RunResult run();

 private:
  struct PendingRequest {
    Request request;
    std::chrono::steady_clock::time_point sent_at;
  };

  struct SessionState;
  class ResponseSink;

  void handle_frame(SessionState& session, std::span<const std::byte> frame);

  std::vector<std::unique_ptr<SessionState>> sessions_;
  RunnerOptions options_;
  LatencySink& latency_;
  RunStats stats_;
};

}  // namespace lob::loadgen
