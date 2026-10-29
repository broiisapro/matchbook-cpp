// Tests for the load generator's runner against in-process fakes, the real TCP
// transport against a loopback fake gateway, and the JSON report.

#include <gtest/gtest.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "net/loadgen/endpoint.hpp"
#include "net/loadgen/latency.hpp"
#include "net/loadgen/report.hpp"
#include "net/loadgen/runner.hpp"
#include "net/loadgen/tcp_transport.hpp"
#include "net/loadgen/transport.hpp"
#include "net/protocol/codec.hpp"
#include "net/protocol/framer.hpp"
#include "net/protocol/messages.hpp"

namespace {

namespace loadgen = lob::loadgen;
namespace proto = lob::net;
using namespace std::chrono_literals;

using Clock = std::chrono::steady_clock;

class SampleSink final : public loadgen::LatencySink {
 public:
  void record(std::uint64_t latency_ns) override { samples.push_back(latency_ns); }

  std::vector<std::uint64_t> samples;
};

/// Options for FakeGateway.
struct FakeOptions {
  /// Withhold the response to every Nth request; 0 answers everything.
  std::uint64_t drop_every = 0;
  /// Reject every Nth new order; 0 accepts everything.
  std::uint64_t reject_every = 0;
};

/// In-process fake gateway: decodes every request and answers it from the same
/// thread, exactly like a gateway that is already waiting on the socket. Tests
/// configure it to reject a share of new orders or to withhold an answer.
class FakeGateway final : public loadgen::OrderEntryTransport {
 public:
  explicit FakeGateway(FakeOptions options = {}) : options_(options) {}

  bool send(std::span<const std::byte> frame) override {
    proto::Header header;
    if (proto::decode_header(frame, header) != proto::DecodeStatus::Ok) {
      error_ = "bad request header";
      return false;
    }
    ++requests_;
    if (options_.drop_every != 0 && requests_ % options_.drop_every == 0) {
      return true;
    }

    std::array<std::byte, proto::kMaxWireSize> response{};
    std::size_t bytes = 0;
    proto::Seq request_seq = 0;
    switch (header.type) {
      case proto::MessageType::NewOrder: {
        proto::NewOrder order;
        if (proto::decode(frame, order, request_seq) != proto::DecodeStatus::Ok) {
          error_ = "bad NewOrder";
          return false;
        }
        if (options_.reject_every != 0 && requests_ % options_.reject_every == 0) {
          bytes = proto::encode(proto::Reject{order.order_id, lob::RejectReason::InvalidPrice},
                                next_seq_++, response);
        } else {
          bytes = proto::encode(proto::Ack{order.order_id}, next_seq_++, response);
        }
        break;
      }
      case proto::MessageType::Cancel: {
        proto::Cancel cancel;
        if (proto::decode(frame, cancel, request_seq) != proto::DecodeStatus::Ok) {
          error_ = "bad Cancel";
          return false;
        }
        bytes = proto::encode(proto::Cancelled{cancel.order_id, 1}, next_seq_++, response);
        break;
      }
      case proto::MessageType::Modify: {
        proto::Modify modify;
        if (proto::decode(frame, modify, request_seq) != proto::DecodeStatus::Ok) {
          error_ = "bad Modify";
          return false;
        }
        bytes = proto::encode(proto::Ack{modify.order_id}, next_seq_++, response);
        break;
      }
      default:
        error_ = "unexpected request type";
        return false;
    }
    outbox_.emplace_back(response.begin(), response.begin() + static_cast<std::ptrdiff_t>(bytes));
    return true;
  }

  int fd() const override { return -1; }

  bool receive(proto::FrameSink& sink) override {
    for (const auto& frame : outbox_) {
      if (!sink.on_frame(std::span<const std::byte>(frame.data(), frame.size()))) {
        error_ = "sink stopped";
        return false;
      }
    }
    outbox_.clear();
    return true;
  }

  const char* last_error() const override { return error_.c_str(); }

  std::uint64_t requests() const { return requests_; }

 private:
  FakeOptions options_;
  std::vector<std::vector<std::byte>> outbox_;
  proto::Seq next_seq_ = 1;
  std::uint64_t requests_ = 0;
  std::string error_;
};

std::vector<std::unique_ptr<loadgen::OrderEntryTransport>> make_gateways(std::size_t count,
                                                                         FakeOptions options = {}) {
  std::vector<std::unique_ptr<loadgen::OrderEntryTransport>> transports;
  for (std::size_t index = 0; index < count; ++index) {
    transports.push_back(std::make_unique<FakeGateway>(options));
  }
  return transports;
}

TEST(LoadgenRunner, AnswersEveryRequestAcrossSessions) {
  SampleSink latency;
  loadgen::RunnerOptions options;
  options.orders = 500;
  options.seed = 42;
  options.response_timeout = 1s;

  loadgen::LoadRunner runner(make_gateways(3), options, latency);
  const loadgen::RunResult result = runner.run();

  ASSERT_TRUE(result.ok()) << result.error;
  EXPECT_EQ(result.stats.orders_requested, 500u);
  EXPECT_EQ(result.stats.requests_sent, 500u);
  EXPECT_EQ(result.stats.first_responses, 500u);
  EXPECT_EQ(result.stats.orders_without_response(), 0u);
  EXPECT_FALSE(result.stats.timed_out);
  EXPECT_EQ(result.stats.send_errors, 0u);
  EXPECT_EQ(result.stats.protocol_errors, 0u);
  EXPECT_EQ(latency.samples.size(), 500u);
  EXPECT_EQ(result.stats.responses_by_type.total(), 500u);
  EXPECT_EQ(result.stats.first_response_by_type.ack + result.stats.first_response_by_type.cancelled,
            500u);
  EXPECT_GT(result.stats.requests_per_second, 0.0);
  EXPECT_GT(result.stats.elapsed_seconds, 0.0);
}

TEST(LoadgenRunner, CountsRejectsAsAnswers) {
  SampleSink latency;
  loadgen::RunnerOptions options;
  options.orders = 300;
  options.seed = 5;
  options.response_timeout = 1s;

  loadgen::LoadRunner runner(make_gateways(2, {.reject_every = 5}), options, latency);
  const loadgen::RunResult result = runner.run();

  ASSERT_TRUE(result.ok()) << result.error;
  EXPECT_EQ(result.stats.first_responses, 300u);
  EXPECT_GT(result.stats.first_response_by_type.reject, 0u);
  EXPECT_EQ(result.stats.first_response_by_type.reject, result.stats.responses_by_type.reject);
  EXPECT_EQ(result.stats.first_response_by_type.ack + result.stats.first_response_by_type.reject +
                result.stats.first_response_by_type.cancelled,
            result.stats.first_responses);
}

TEST(LoadgenRunner, ReportsUnansweredRequestsAfterTheTimeout) {
  SampleSink latency;
  loadgen::RunnerOptions options;
  options.orders = 10;
  options.seed = 3;
  options.response_timeout = 30ms;

  loadgen::LoadRunner runner(make_gateways(1, {.drop_every = 2}), options, latency);
  const loadgen::RunResult result = runner.run();

  EXPECT_FALSE(result.ok());
  EXPECT_TRUE(result.stats.timed_out);
  EXPECT_GT(result.stats.orders_without_response(), 0u);
  EXPECT_LT(result.stats.first_responses, 10u);
  EXPECT_EQ(result.stats.first_responses + result.stats.orders_without_response(),
            result.stats.orders_requested);
}

TEST(LoadgenRunner, RateLimitSpacesTheStream) {
  SampleSink latency;
  loadgen::RunnerOptions options;
  options.orders = 3;
  options.seed = 11;
  options.rate = 200.0;
  options.response_timeout = 1s;

  loadgen::LoadRunner runner(make_gateways(1), options, latency);
  const loadgen::RunResult result = runner.run();

  ASSERT_TRUE(result.ok()) << result.error;
  EXPECT_EQ(result.stats.requests_sent, 3u);
  EXPECT_EQ(result.stats.first_responses, 3u);
  // Requests 0, 1 and 2 are due at 0, 5 ms and 10 ms; a lower bound only, so a
  // slow machine can never make the test flaky.
  EXPECT_GE(result.stats.elapsed_seconds, 0.008);
  EXPECT_LE(result.stats.requests_per_second, 400.0);
}

TEST(LoadgenRunner, ZeroOrdersCompletesImmediately) {
  SampleSink latency;
  loadgen::RunnerOptions options;
  options.orders = 0;
  loadgen::LoadRunner runner(make_gateways(2), options, latency);
  const loadgen::RunResult result = runner.run();
  EXPECT_TRUE(result.ok());
  EXPECT_EQ(result.stats.requests_sent, 0u);
  EXPECT_TRUE(latency.samples.empty());
}

TEST(LoadgenReport, WritesTheDocumentedSchema) {
  loadgen::Report report;
  report.gateway = "127.0.0.1:9000";
  report.sessions = 2;
  report.seed = 9;
  report.rate_limit = 2500.0;
  report.stats.orders_requested = 4;
  report.stats.requests_sent = 4;
  report.stats.responses_received = 5;
  report.stats.first_responses = 4;
  report.stats.unsolicited_responses = 1;
  report.stats.first_response_by_type.ack = 2;
  report.stats.first_response_by_type.reject = 1;
  report.stats.first_response_by_type.cancelled = 1;
  report.stats.responses_by_type = report.stats.first_response_by_type;
  report.stats.responses_by_type.fill = 1;
  report.stats.elapsed_seconds = 1.5;
  report.stats.requests_per_second = 2.6666667;
  report.latency = loadgen::LatencySummary{4, 1000, 2000, 3000, 4000, 4000, 1800.0};

  const std::string json = loadgen::to_json(report);
  EXPECT_NE(json.find("\"schema_version\": 1"), std::string::npos);
  EXPECT_NE(json.find("\"gateway\": \"127.0.0.1:9000\""), std::string::npos);
  EXPECT_NE(json.find("\"sessions\": 2"), std::string::npos);
  EXPECT_NE(json.find("\"rate_limit_per_second\": 2500"), std::string::npos);
  EXPECT_NE(json.find("\"orders_requested\": 4"), std::string::npos);
  EXPECT_NE(json.find("\"orders_without_response\": 0"), std::string::npos);
  EXPECT_NE(json.find("\"complete\": true"), std::string::npos);
  EXPECT_NE(json.find("\"error\": \"\""), std::string::npos);
  EXPECT_NE(json.find("\"latency_ns\": {\"count\": 4, \"p50_ns\": 1000, \"p90_ns\": 2000"),
            std::string::npos);
  EXPECT_NE(json.find("\"p999_ns\": 4000, \"max_ns\": 4000"), std::string::npos);
  EXPECT_NE(json.find("\"latency_clamped_samples\": 0"), std::string::npos);
  EXPECT_NE(json.find("\"first_response_by_type\": {\"ack\": 2, \"reject\": 1, \"fill\": 0"),
            std::string::npos);
  EXPECT_NE(json.find("\"responses_by_type\": {\"ack\": 2, \"reject\": 1, \"fill\": 1"),
            std::string::npos);
  EXPECT_NE(json.find("\"clock\": \"std::chrono::steady_clock"), std::string::npos);
  EXPECT_NE(json.find("\"loop\": \"closed"), std::string::npos);
  EXPECT_NE(json.find("coordinated_omission"), std::string::npos);
  EXPECT_NE(json.find("\"network\": \"TCP loopback"), std::string::npos);
}

TEST(LoadgenReport, NullsAnUnlimitedRateAndReportsFailures) {
  loadgen::Report report;
  report.gateway = "gateway";
  report.stats.orders_requested = 3;
  report.stats.timed_out = true;
  report.error = "session receive failed: connection closed";

  const std::string json = loadgen::to_json(report);
  EXPECT_NE(json.find("\"rate_limit_per_second\": null"), std::string::npos);
  EXPECT_NE(json.find("\"orders_without_response\": 3"), std::string::npos);
  EXPECT_NE(json.find("\"timed_out\": true"), std::string::npos);
  EXPECT_NE(json.find("\"complete\": false"), std::string::npos);
  EXPECT_NE(json.find("connection closed"), std::string::npos);
}

TEST(LoadgenReport, EscapesStringsAndIgnoresNonFiniteNumbers) {
  loadgen::Report report;
  report.gateway = "host\"with\\quote";
  report.error = "line\nbreak\ttab";
  report.stats.elapsed_seconds = std::numeric_limits<double>::quiet_NaN();
  report.latency.mean_ns = std::numeric_limits<double>::infinity();

  const std::string json = loadgen::to_json(report);
  EXPECT_NE(json.find("\"gateway\": \"host\\\"with\\\\quote\""), std::string::npos);
  EXPECT_NE(json.find("\"line\\nbreak\\ttab\""), std::string::npos);
  EXPECT_NE(json.find("\"elapsed_seconds\": null"), std::string::npos);
  EXPECT_NE(json.find("\"mean_ns\": null"), std::string::npos);
}

/// Loopback TCP server that speaks the wire protocol and answers every request
/// with an Ack (NewOrder/Modify) or Cancelled (Cancel). It runs on its own
/// thread so the client exercises the real socket and framer path.
class FakeTcpGateway {
 public:
  explicit FakeTcpGateway(bool split_writes = false) : split_writes_(split_writes) {
    listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) {
      return;
    }
    int reuse = 1;
    (void)::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    if (::bind(listen_fd_, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0 ||
        ::listen(listen_fd_, 4) != 0) {
      ::close(listen_fd_);
      listen_fd_ = -1;
      return;
    }
    socklen_t length = sizeof(address);
    (void)::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&address), &length);
    port_ = ntohs(address.sin_port);
    thread_ = std::thread([this]() { serve(); });
  }

  ~FakeTcpGateway() {
    stop_.store(true);
    if (thread_.joinable()) {
      thread_.join();
    }
    for (std::thread& client : clients_) {
      if (client.joinable()) {
        client.join();
      }
    }
    if (listen_fd_ >= 0) {
      ::close(listen_fd_);
    }
  }

  FakeTcpGateway(const FakeTcpGateway&) = delete;
  FakeTcpGateway& operator=(const FakeTcpGateway&) = delete;

  std::uint16_t port() const { return port_; }
  std::uint64_t requests() const { return requests_.load(); }
  bool ready() const { return listen_fd_ >= 0; }

 private:
  class ReplySink final : public proto::FrameSink {
   public:
    ReplySink(FakeTcpGateway& gateway, int fd) : gateway_(gateway), fd_(fd) {}

    bool on_frame(std::span<const std::byte> frame) override { return gateway_.reply(fd_, frame); }

   private:
    FakeTcpGateway& gateway_;
    int fd_;
  };

  bool reply(int fd, std::span<const std::byte> frame) {
    proto::Header header;
    if (proto::decode_header(frame, header) != proto::DecodeStatus::Ok) {
      return false;
    }
    requests_.fetch_add(1);

    std::array<std::byte, proto::kMaxWireSize> response{};
    std::size_t bytes = 0;
    proto::Seq seq = 0;
    switch (header.type) {
      case proto::MessageType::NewOrder: {
        proto::NewOrder order;
        if (proto::decode(frame, order, seq) != proto::DecodeStatus::Ok) {
          return false;
        }
        bytes = proto::encode(proto::Ack{order.order_id}, next_seq_.fetch_add(1) + 1, response);
        break;
      }
      case proto::MessageType::Cancel: {
        proto::Cancel cancel;
        if (proto::decode(frame, cancel, seq) != proto::DecodeStatus::Ok) {
          return false;
        }
        bytes = proto::encode(proto::Cancelled{cancel.order_id, 1}, next_seq_.fetch_add(1) + 1,
                              response);
        break;
      }
      case proto::MessageType::Modify: {
        proto::Modify modify;
        if (proto::decode(frame, modify, seq) != proto::DecodeStatus::Ok) {
          return false;
        }
        bytes = proto::encode(proto::Ack{modify.order_id}, next_seq_.fetch_add(1) + 1, response);
        break;
      }
      default:
        return false;
    }

    std::size_t offset = 0;
    while (offset < bytes) {
      const std::size_t chunk = split_writes_ ? 1 : bytes - offset;
      const ssize_t written = ::send(fd, response.data() + offset, chunk, 0);
      if (written <= 0) {
        return false;
      }
      offset += static_cast<std::size_t>(written);
    }
    return true;
  }

  void serve() {
    while (!stop_.load()) {
      pollfd listener{listen_fd_, POLLIN, 0};
      if (::poll(&listener, 1, 10) <= 0) {
        continue;
      }
      const int client = ::accept(listen_fd_, nullptr, nullptr);
      if (client < 0) {
        continue;
      }
      clients_.emplace_back([this, client]() {
        serve_client(client);
        ::close(client);
      });
    }
  }

  void serve_client(int fd) {
    std::array<std::byte, 4096> buffer{};
    proto::StreamFramer framer;
    ReplySink sink(*this, fd);
    while (!stop_.load()) {
      pollfd readable{fd, POLLIN, 0};
      if (::poll(&readable, 1, 10) <= 0) {
        continue;
      }
      ssize_t count = 0;
      do {
        count = ::recv(fd, buffer.data(), buffer.size(), 0);
      } while (count < 0 && errno == EINTR);
      if (count <= 0) {
        return;
      }
      const auto bytes = std::span<const std::byte>(buffer.data(), static_cast<std::size_t>(count));
      if (framer.push(bytes, sink).result == proto::FramerResult::Error) {
        return;
      }
    }
  }

  bool split_writes_;
  int listen_fd_ = -1;
  std::uint16_t port_ = 0;
  std::atomic<bool> stop_{false};
  std::atomic<std::uint64_t> requests_{0};
  std::atomic<std::uint64_t> next_seq_{0};
  std::thread thread_;
  std::vector<std::thread> clients_;
};

std::vector<std::unique_ptr<loadgen::OrderEntryTransport>> connect_tcp(
    const loadgen::Endpoint& endpoint, std::uint32_t sessions, std::string& error) {
  std::vector<std::unique_ptr<loadgen::OrderEntryTransport>> transports;
  for (std::uint32_t index = 0; index < sessions; ++index) {
    std::unique_ptr<loadgen::TcpTransport> transport =
        loadgen::TcpTransport::connect(endpoint, error);
    if (transport == nullptr) {
      return {};
    }
    transports.push_back(std::move(transport));
  }
  return transports;
}

TEST(LoadgenTcp, TalksToALoopbackGateway) {
  FakeTcpGateway gateway;
  ASSERT_TRUE(gateway.ready());
  loadgen::Endpoint endpoint{"127.0.0.1", gateway.port()};

  std::string error;
  auto transports = connect_tcp(endpoint, 2, error);
  ASSERT_EQ(transports.size(), 2u) << error;

  SampleSink latency;
  loadgen::RunnerOptions options;
  options.orders = 200;
  options.seed = 21;
  options.response_timeout = 2s;
  loadgen::LoadRunner runner(std::move(transports), options, latency);
  const loadgen::RunResult result = runner.run();

  ASSERT_TRUE(result.ok()) << result.error;
  EXPECT_EQ(result.stats.requests_sent, 200u);
  EXPECT_EQ(result.stats.first_responses, 200u);
  EXPECT_EQ(gateway.requests(), 200u);
  EXPECT_EQ(latency.samples.size(), 200u);
}

TEST(LoadgenTcp, HandlesResponsesSplitAcrossReads) {
  FakeTcpGateway gateway(/*split_writes=*/true);
  ASSERT_TRUE(gateway.ready());
  loadgen::Endpoint endpoint{"127.0.0.1", gateway.port()};

  std::string error;
  auto transports = connect_tcp(endpoint, 1, error);
  ASSERT_EQ(transports.size(), 1u) << error;

  SampleSink latency;
  loadgen::RunnerOptions options;
  options.orders = 40;
  options.seed = 22;
  options.response_timeout = 2s;
  loadgen::LoadRunner runner(std::move(transports), options, latency);
  const loadgen::RunResult result = runner.run();

  ASSERT_TRUE(result.ok()) << result.error;
  EXPECT_EQ(result.stats.first_responses, 40u);
  EXPECT_EQ(latency.samples.size(), 40u);
}

TEST(LoadgenTcp, ConnectionFailureIsReportedNotFatal) {
  loadgen::Endpoint endpoint{"127.0.0.1", 1};  // nothing listens on port 1
  std::string error;
  auto transports = connect_tcp(endpoint, 1, error);
  EXPECT_TRUE(transports.empty());
  EXPECT_FALSE(error.empty());
}

}  // namespace
