#pragma once

// UDP market-data publisher with a TCP recovery service.
//
// One Publisher runs one single-threaded poll loop: it connects to the
// gateway's event stream, reads protocol frames, stores each one in a
// preallocated retransmit ring, packs them into UDP datagrams and sends every
// datagram to every destination. The same loop accepts recovery clients on
// TCP, so a recovery response never blocks live publishing and several
// subscribers can recover at once.
//
// The loop is exposed one step at a time (poll_once) so tests can drive it
// deterministically against an in-process fake gateway and loopback receivers.

#include <sys/socket.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "net/protocol/framer.hpp"
#include "net/protocol/messages.hpp"
#include "net/publisher/datagram_packer.hpp"
#include "net/publisher/endpoint.hpp"
#include "net/publisher/retransmit_store.hpp"
#include "net/publisher/udp_sender.hpp"

namespace lob::publisher {

using net::StreamFramer;

struct PublisherConfig {
  Endpoint gateway{"127.0.0.1", 9001};
  std::vector<Endpoint> udp_destinations;
  /// Optional extra destination for a multicast group. The publisher only
  /// sends; it never joins the group.
  std::optional<Endpoint> multicast_group;
  /// Recovery listener address and port. Port 0 asks the kernel for an
  /// ephemeral port; recovery_port() reports the bound one.
  std::string recovery_bind_host{"0.0.0.0"};
  std::uint16_t recovery_port = 9002;
  std::size_t retransmit_capacity = 65536;
  std::size_t max_datagram_bytes = DatagramPacker::kDefaultMaxBytes;
  std::chrono::milliseconds reconnect_initial{100};
  std::chrono::milliseconds reconnect_max{5000};
  /// Time allowed for a non-blocking TCP connect to the gateway.
  std::chrono::milliseconds connect_timeout{2000};
  /// Print connect/disconnect lines to stderr. Off for tests.
  bool log_connections = false;
};

struct PublisherStats {
  std::uint64_t messages_published = 0;
  std::uint64_t datagrams_sent = 0;
  std::uint64_t send_failures = 0;
  std::uint64_t oversized_frames = 0;
  std::uint64_t gateway_connections = 0;
  std::uint64_t gateway_disconnects = 0;
  std::uint64_t protocol_errors = 0;
  std::uint64_t recovery_requests = 0;
  std::uint64_t sequence_resets = 0;
  std::uint64_t rejected_recovery_requests = 0;
};

class Publisher : private net::FrameSink {
 public:
  /// Builds the retransmit ring and the datagram packer. Throws
  /// std::invalid_argument when the capacity or datagram size is invalid.
  explicit Publisher(PublisherConfig config);
  ~Publisher();

  Publisher(const Publisher&) = delete;
  Publisher& operator=(const Publisher&) = delete;

  /// Opens the UDP destinations and the recovery listener and resolves the
  /// gateway address. Idempotent. On failure `error` explains which step
  /// failed and nothing is left open.
  bool open(std::string& error);

  /// Runs one iteration of the poll loop: tries to connect to the gateway when
  /// it is due, waits up to `timeout` for I/O, then services the gateway and
  /// every recovery client.
  void poll_once(std::chrono::milliseconds timeout);

  /// Runs poll_once(200 ms) until stop() is called.
  void run();

  void stop() { stop_.store(true, std::memory_order_relaxed); }

  /// Actual recovery port after a bind to an ephemeral port (0 requested).
  std::uint16_t recovery_port() const { return recovery_port_; }

  const PublisherStats& stats() const { return stats_; }

 private:
  struct RecoveryClient {
    int fd = -1;
    StreamFramer framer;
    std::vector<std::byte> output;  ///< queued response bytes
    std::size_t output_offset = 0;  ///< bytes of output already sent
  };

  bool on_frame(std::span<const std::byte> frame) override;

  void attempt_gateway_connect();
  void finish_gateway_connect();
  void drop_gateway();
  void schedule_reconnect();
  void read_gateway();
  bool on_gateway_frame(std::span<const std::byte> frame);
  void flush_datagrams();

  void accept_recovery_clients();
  bool service_recovery_client(std::size_t index, short revents);
  bool flush_recovery_output(RecoveryClient& client);
  bool queue_recovery_bytes(RecoveryClient& client, const std::byte* data, std::size_t size);
  bool on_recovery_frame(RecoveryClient& client, std::span<const std::byte> frame);

  int poll_timeout_ms(std::chrono::milliseconds requested) const;
  void log(const char* text, const Endpoint& endpoint);

  PublisherConfig config_;
  RetransmitStore store_;
  DatagramPacker packer_;
  UdpSender udp_;
  PublisherStats stats_;

  int recovery_listener_ = -1;
  std::uint16_t recovery_port_ = 0;
  std::vector<RecoveryClient> recovery_clients_;
  std::size_t max_pending_recovery_bytes_ = 0;

  sockaddr_storage gateway_address_{};
  socklen_t gateway_address_length_ = 0;
  int gateway_fd_ = -1;
  enum class GatewayState : std::uint8_t { Disconnected, Connecting, Connected };
  GatewayState gateway_state_ = GatewayState::Disconnected;
  StreamFramer gateway_framer_;
  std::chrono::steady_clock::time_point next_connect_{};
  std::chrono::steady_clock::time_point connect_deadline_{};
  std::chrono::milliseconds backoff_{0};

  RecoveryClient* recovery_target_ = nullptr;
  std::vector<std::byte> read_buffer_;
  std::atomic<bool> stop_{false};
  bool opened_ = false;
};

}  // namespace lob::publisher
