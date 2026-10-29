#include <arpa/inet.h>
#include <fcntl.h>
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
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "net/protocol/codec.hpp"
#include "net/protocol/framer.hpp"
#include "net/protocol/messages.hpp"
#include "net/publisher/publisher.hpp"
#include "net/publisher/socket.hpp"

namespace {

namespace pub = lob::publisher;
namespace proto = lob::net;

using namespace std::chrono_literals;

// ------------------------------------------------------------- test frames

std::vector<std::byte> make_frame(proto::Seq seq, bool trade, lob::Price price, lob::Qty qty) {
  std::array<std::byte, proto::kMaxWireSize> buffer{};
  std::size_t written = 0;
  if (trade) {
    written = proto::encode(proto::Trade{seq + 1000, seq + 2000, price, qty}, seq, buffer);
  } else {
    written = proto::encode(proto::BookUpdate{price, qty, lob::Side::Buy}, seq, buffer);
  }
  EXPECT_GT(written, 0U);
  return std::vector<std::byte>(buffer.begin(),
                                buffer.begin() + static_cast<std::ptrdiff_t>(written));
}

std::vector<std::vector<std::byte>> make_stream(proto::Seq first, proto::Seq last) {
  std::vector<std::vector<std::byte>> frames;
  for (proto::Seq seq = first; seq <= last; ++seq) {
    frames.push_back(make_frame(seq, seq % 2 == 0, 100 + static_cast<lob::Price>(seq),
                                static_cast<lob::Qty>(seq)));
  }
  return frames;
}

std::vector<std::byte> concatenate(const std::vector<std::vector<std::byte>>& frames) {
  std::vector<std::byte> bytes;
  for (const std::vector<std::byte>& frame : frames) {
    bytes.insert(bytes.end(), frame.begin(), frame.end());
  }
  return bytes;
}

// --------------------------------------------------------------- collecting

class CollectingSink : public proto::FrameSink {
 public:
  bool on_frame(std::span<const std::byte> frame) override {
    frames.emplace_back(frame.begin(), frame.end());
    return true;
  }

  std::vector<std::vector<std::byte>> frames;
};

// ------------------------------------------------------------- UDP receiver

class UdpReceiver {
 public:
  UdpReceiver() {
    fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
    EXPECT_GE(fd_, 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    EXPECT_EQ(::bind(fd_, reinterpret_cast<const sockaddr*>(&address), sizeof(address)), 0);

    socklen_t length = sizeof(address);
    EXPECT_EQ(::getsockname(fd_, reinterpret_cast<sockaddr*>(&address), &length), 0);
    port_ = ntohs(address.sin_port);

    const int flags = ::fcntl(fd_, F_GETFL, 0);
    EXPECT_GE(flags, 0);
    EXPECT_EQ(::fcntl(fd_, F_SETFL, flags | O_NONBLOCK), 0);
  }

  ~UdpReceiver() {
    if (fd_ >= 0) {
      ::close(fd_);
    }
  }

  UdpReceiver(const UdpReceiver&) = delete;
  UdpReceiver& operator=(const UdpReceiver&) = delete;

  std::uint16_t port() const { return port_; }

  /// Reads every datagram that is currently queued. Each datagram gets its own
  /// framer, so bytes left buffered mean the publisher split a frame.
  void drain() {
    for (;;) {
      std::array<std::byte, 65535> buffer{};
      const ssize_t received = ::recv(fd_, buffer.data(), buffer.size(), 0);
      if (received < 0) {
        break;
      }
      if (received == 0) {
        continue;
      }
      datagram_sizes_.push_back(static_cast<std::size_t>(received));

      proto::StreamFramer framer;
      CollectingSink sink;
      const proto::PushResult result = framer.push(
          std::span<const std::byte>(buffer.data(), static_cast<std::size_t>(received)), sink);
      if (result.result != proto::FramerResult::Ok) {
        ++parse_errors_;
      }
      if (framer.buffered() != 0) {
        ++split_frames_;
      }
      for (std::vector<std::byte>& frame : sink.frames) {
        frames_.push_back(std::move(frame));
      }
    }
  }

  const std::vector<std::vector<std::byte>>& frames() const { return frames_; }
  const std::vector<std::size_t>& datagram_sizes() const { return datagram_sizes_; }
  std::size_t parse_errors() const { return parse_errors_; }
  std::size_t split_frames() const { return split_frames_; }

 private:
  int fd_ = -1;
  std::uint16_t port_ = 0;
  std::vector<std::vector<std::byte>> frames_;
  std::vector<std::size_t> datagram_sizes_;
  std::size_t parse_errors_ = 0;
  std::size_t split_frames_ = 0;
};

// ------------------------------------------------------------ fake gateway

/// An in-process event-stream server: accepts TCP connections and lets the test
/// push frames to every accepted client. Only the accept loop runs on a thread;
/// the publisher loop runs on the test thread.
class FakeGateway {
 public:
  FakeGateway() {
    listener_ = ::socket(AF_INET, SOCK_STREAM, 0);
    EXPECT_GE(listener_, 0);
    const int one = 1;
    ::setsockopt(listener_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    EXPECT_EQ(::bind(listener_, reinterpret_cast<const sockaddr*>(&address), sizeof(address)), 0);
    EXPECT_EQ(::listen(listener_, 8), 0);

    socklen_t length = sizeof(address);
    EXPECT_EQ(::getsockname(listener_, reinterpret_cast<sockaddr*>(&address), &length), 0);
    port_ = ntohs(address.sin_port);

    const int flags = ::fcntl(listener_, F_GETFL, 0);
    EXPECT_GE(flags, 0);
    EXPECT_EQ(::fcntl(listener_, F_SETFL, flags | O_NONBLOCK), 0);

    thread_ = std::thread([this] { accept_loop(); });
  }

  ~FakeGateway() {
    stop_.store(true);
    if (thread_.joinable()) {
      thread_.join();
    }
    close_connections();
    if (listener_ >= 0) {
      ::close(listener_);
    }
  }

  FakeGateway(const FakeGateway&) = delete;
  FakeGateway& operator=(const FakeGateway&) = delete;

  std::uint16_t port() const { return port_; }
  int connections() const { return connections_.load(); }

  void send_bytes(const std::vector<std::byte>& bytes) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (int fd : clients_) {
      std::size_t sent = 0;
      while (sent < bytes.size()) {
        const ssize_t written =
            ::send(fd, bytes.data() + sent, bytes.size() - sent, pub::kSendFlags);
        if (written <= 0) {
          break;
        }
        sent += static_cast<std::size_t>(written);
      }
    }
  }

  void close_connections() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (int fd : clients_) {
      ::close(fd);
    }
    clients_.clear();
  }

 private:
  void accept_loop() {
    while (!stop_.load()) {
      pollfd entry{listener_, POLLIN, 0};
      if (::poll(&entry, 1, 20) <= 0) {
        continue;
      }
      const int fd = ::accept(listener_, nullptr, nullptr);
      if (fd < 0) {
        continue;
      }
      pub::suppress_sigpipe(fd);
      std::lock_guard<std::mutex> lock(mutex_);
      clients_.push_back(fd);
      connections_.fetch_add(1);
    }
  }

  int listener_ = -1;
  std::uint16_t port_ = 0;
  std::atomic<bool> stop_{false};
  std::atomic<int> connections_{0};
  std::mutex mutex_;
  std::vector<int> clients_;
  std::thread thread_;
};

// ----------------------------------------------------------- recovery client

class RecoveryClient {
 public:
  ~RecoveryClient() {
    if (fd_ >= 0) {
      ::close(fd_);
    }
  }

  RecoveryClient() = default;
  RecoveryClient(const RecoveryClient&) = delete;
  RecoveryClient& operator=(const RecoveryClient&) = delete;

  bool connect_to(std::uint16_t port) {
    fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd_ < 0) {
      return false;
    }
    pub::suppress_sigpipe(fd_);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);
    if (::connect(fd_, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
      return false;
    }
    const int flags = ::fcntl(fd_, F_GETFL, 0);
    return flags >= 0 && ::fcntl(fd_, F_SETFL, flags | O_NONBLOCK) == 0;
  }

  bool send_request(proto::Seq from, proto::Seq to, proto::Seq header_seq) {
    std::array<std::byte, proto::RecoveryRequest::kWireSize> buffer{};
    const std::size_t written = proto::encode(proto::RecoveryRequest{from, to}, header_seq, buffer);
    if (written == 0) {
      return false;
    }
    std::size_t sent = 0;
    while (sent < written) {
      const ssize_t result = ::send(fd_, buffer.data() + sent, written - sent, pub::kSendFlags);
      if (result > 0) {
        sent += static_cast<std::size_t>(result);
        continue;
      }
      if (result < 0 && errno == EINTR) {
        continue;
      }
      return false;
    }
    return true;
  }

  void drain() {
    for (;;) {
      std::array<std::byte, 4096> buffer{};
      const ssize_t received = ::recv(fd_, buffer.data(), buffer.size(), 0);
      if (received > 0) {
        const auto span =
            std::span<const std::byte>(buffer.data(), static_cast<std::size_t>(received));
        if (framer_.push(span, sink_).result == proto::FramerResult::Error) {
          closed_ = true;
          return;
        }
        continue;
      }
      if (received == 0) {
        closed_ = true;
        return;
      }
      if (errno == EINTR) {
        continue;
      }
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        return;
      }
      closed_ = true;
      return;
    }
  }

  const std::vector<std::vector<std::byte>>& frames() const { return sink_.frames; }
  bool closed() const { return closed_; }

 private:
  int fd_ = -1;
  proto::StreamFramer framer_;
  CollectingSink sink_;
  bool closed_ = false;
};

// --------------------------------------------------------------- utilities

template <class Done>
bool drive_with_receivers(pub::Publisher& publisher, const std::vector<UdpReceiver*>& receivers,
                          Done done, int ticks = 4000) {
  for (int tick = 0; tick < ticks; ++tick) {
    publisher.poll_once(2ms);
    for (UdpReceiver* receiver : receivers) {
      receiver->drain();
    }
    if (done()) {
      return true;
    }
  }
  for (UdpReceiver* receiver : receivers) {
    receiver->drain();
  }
  return done();
}

template <class Done>
bool drive_recovery(pub::Publisher& publisher, RecoveryClient& client, Done done,
                    int ticks = 4000) {
  for (int tick = 0; tick < ticks; ++tick) {
    publisher.poll_once(2ms);
    client.drain();
    if (done()) {
      return true;
    }
    if (client.closed()) {
      return false;
    }
  }
  client.drain();
  return done();
}

pub::PublisherConfig make_config(const FakeGateway& gateway,
                                 const std::vector<UdpReceiver*>& receivers,
                                 std::size_t retransmit_capacity = 256) {
  pub::PublisherConfig config;
  config.gateway = pub::Endpoint{"127.0.0.1", gateway.port()};
  for (const UdpReceiver* receiver : receivers) {
    config.udp_destinations.push_back(pub::Endpoint{"127.0.0.1", receiver->port()});
  }
  config.recovery_bind_host = "127.0.0.1";
  config.recovery_port = 0;
  config.retransmit_capacity = retransmit_capacity;
  config.reconnect_initial = 5ms;
  config.reconnect_max = 20ms;
  return config;
}

void expect_frames(const std::vector<std::vector<std::byte>>& actual,
                   const std::vector<std::vector<std::byte>>& expected) {
  ASSERT_EQ(actual.size(), expected.size());
  for (std::size_t index = 0; index < expected.size(); ++index) {
    EXPECT_EQ(actual[index], expected[index]) << "frame " << index;
  }
}

// ------------------------------------------------------------------- tests

TEST(PublisherDelivery, DeliversEveryMessageOncePerDestinationInOrder) {
  UdpReceiver first;
  UdpReceiver second;
  FakeGateway gateway;
  const std::vector<UdpReceiver*> receivers{&first, &second};

  pub::Publisher publisher(make_config(gateway, receivers));
  std::string error;
  ASSERT_TRUE(publisher.open(error)) << error;

  ASSERT_TRUE(
      drive_with_receivers(publisher, receivers, [&] { return gateway.connections() >= 1; }));

  const std::vector<std::vector<std::byte>> expected = make_stream(1, 100);
  gateway.send_bytes(concatenate(expected));

  ASSERT_TRUE(drive_with_receivers(publisher, receivers, [&] {
    return first.frames().size() >= expected.size() && second.frames().size() >= expected.size();
  }));

  for (const UdpReceiver* receiver : receivers) {
    expect_frames(receiver->frames(), expected);
    EXPECT_EQ(receiver->split_frames(), 0U);
    EXPECT_EQ(receiver->parse_errors(), 0U);
    ASSERT_FALSE(receiver->datagram_sizes().empty());
    std::size_t largest = 0;
    for (const std::size_t size : receiver->datagram_sizes()) {
      EXPECT_LE(size, 1200U);
      largest = std::max(largest, size);
    }
    EXPECT_GT(largest, 1000U) << "frames should be batched into datagrams";
    EXPECT_GE(receiver->datagram_sizes().size(), 2U);
  }
}

TEST(PublisherDelivery, ReconnectsToTheGatewayWithBackoff) {
  UdpReceiver receiver;
  FakeGateway gateway;
  const std::vector<UdpReceiver*> receivers{&receiver};

  pub::Publisher publisher(make_config(gateway, receivers));
  std::string error;
  ASSERT_TRUE(publisher.open(error)) << error;

  ASSERT_TRUE(
      drive_with_receivers(publisher, receivers, [&] { return gateway.connections() >= 1; }));

  const std::vector<std::vector<std::byte>> first_batch = make_stream(1, 5);
  gateway.send_bytes(concatenate(first_batch));
  ASSERT_TRUE(
      drive_with_receivers(publisher, receivers, [&] { return receiver.frames().size() >= 5; }));

  gateway.close_connections();
  ASSERT_TRUE(
      drive_with_receivers(publisher, receivers, [&] { return gateway.connections() >= 2; }));

  const std::vector<std::vector<std::byte>> second_batch = make_stream(6, 10);
  gateway.send_bytes(concatenate(second_batch));
  ASSERT_TRUE(
      drive_with_receivers(publisher, receivers, [&] { return receiver.frames().size() >= 10; }));

  std::vector<std::vector<std::byte>> expected = first_batch;
  expected.insert(expected.end(), second_batch.begin(), second_batch.end());
  expect_frames(receiver.frames(), expected);
  EXPECT_GE(publisher.stats().gateway_connections, 2U);
}

TEST(PublisherRecovery, RecoversAMidStreamGap) {
  UdpReceiver receiver;
  FakeGateway gateway;
  const std::vector<UdpReceiver*> receivers{&receiver};

  pub::Publisher publisher(make_config(gateway, receivers));
  std::string error;
  ASSERT_TRUE(publisher.open(error)) << error;

  ASSERT_TRUE(
      drive_with_receivers(publisher, receivers, [&] { return gateway.connections() >= 1; }));
  const std::vector<std::vector<std::byte>> expected = make_stream(1, 30);
  gateway.send_bytes(concatenate(expected));
  ASSERT_TRUE(drive_with_receivers(publisher, receivers,
                                   [&] { return receiver.frames().size() >= expected.size(); }));

  RecoveryClient client;
  ASSERT_TRUE(client.connect_to(publisher.recovery_port()));
  ASSERT_TRUE(client.send_request(10, 14, 1));
  ASSERT_TRUE(drive_recovery(publisher, client, [&] { return client.frames().size() >= 5; }));
  ASSERT_EQ(client.frames().size(), 5U);
  for (std::size_t index = 0; index < 5; ++index) {
    EXPECT_EQ(client.frames()[index], expected[9 + index]) << "recovered frame " << index;
  }

  // The same connection serves a second request (framing is per stream).
  ASSERT_TRUE(client.send_request(20, 22, 2));
  ASSERT_TRUE(drive_recovery(publisher, client, [&] { return client.frames().size() >= 8; }));
  ASSERT_EQ(client.frames().size(), 8U);
  for (std::size_t index = 0; index < 3; ++index) {
    EXPECT_EQ(client.frames()[5 + index], expected[19 + index]);
  }
}

TEST(PublisherRecovery, SendsSequenceResetWhenTheStartIsGone) {
  UdpReceiver receiver;
  FakeGateway gateway;
  const std::vector<UdpReceiver*> receivers{&receiver};

  pub::Publisher publisher(make_config(gateway, receivers, 8));
  std::string error;
  ASSERT_TRUE(publisher.open(error)) << error;

  ASSERT_TRUE(
      drive_with_receivers(publisher, receivers, [&] { return gateway.connections() >= 1; }));
  const std::vector<std::vector<std::byte>> expected = make_stream(1, 20);
  gateway.send_bytes(concatenate(expected));
  ASSERT_TRUE(drive_with_receivers(publisher, receivers,
                                   [&] { return receiver.frames().size() >= expected.size(); }));

  RecoveryClient client;
  ASSERT_TRUE(client.connect_to(publisher.recovery_port()));
  ASSERT_TRUE(client.send_request(1, 20, 1));
  ASSERT_TRUE(drive_recovery(publisher, client, [&] { return client.frames().size() >= 1; }));
  ASSERT_EQ(client.frames().size(), 1U);

  proto::SequenceReset reset;
  proto::Seq header_seq = 0;
  ASSERT_EQ(proto::decode(client.frames()[0], reset, header_seq), proto::DecodeStatus::Ok);
  EXPECT_EQ(reset.next_seq, 13U);  // 20 held messages, capacity 8 -> oldest is 13
  EXPECT_EQ(publisher.stats().sequence_resets, 1U);

  // Recovery resumes from the reset sequence.
  ASSERT_TRUE(client.send_request(13, 15, 2));
  ASSERT_TRUE(drive_recovery(publisher, client, [&] { return client.frames().size() >= 4; }));
  ASSERT_EQ(client.frames().size(), 4U);
  for (std::size_t index = 0; index < 3; ++index) {
    EXPECT_EQ(client.frames()[1 + index], expected[12 + index]);
  }
}

TEST(PublisherRecovery, RejectsMalformedRequestsByClosingTheConnection) {
  UdpReceiver receiver;
  FakeGateway gateway;
  const std::vector<UdpReceiver*> receivers{&receiver};

  pub::Publisher publisher(make_config(gateway, receivers, 8));
  std::string error;
  ASSERT_TRUE(publisher.open(error)) << error;

  ASSERT_TRUE(
      drive_with_receivers(publisher, receivers, [&] { return gateway.connections() >= 1; }));
  gateway.send_bytes(concatenate(make_stream(1, 20)));
  ASSERT_TRUE(
      drive_with_receivers(publisher, receivers, [&] { return receiver.frames().size() >= 20; }));

  struct Request {
    proto::Seq from;
    proto::Seq to;
    const char* why;
  };
  const std::vector<Request> requests{
      {5, 4, "reversed range"},
      {0, 1, "zero sequence"},
      {15, 23, "span larger than the store"},  // 15..23 inclusive is 9 > capacity 8
  };
  for (const Request& request : requests) {
    RecoveryClient client;
    ASSERT_TRUE(client.connect_to(publisher.recovery_port())) << request.why;
    ASSERT_TRUE(client.send_request(request.from, request.to, 1)) << request.why;
    ASSERT_TRUE(drive_recovery(publisher, client, [&] { return client.closed(); })) << request.why;
    EXPECT_TRUE(client.frames().empty()) << request.why;
  }
  EXPECT_EQ(publisher.stats().rejected_recovery_requests, requests.size());
}

TEST(PublisherRecovery, ServesConcurrentRecoveriesWhilePublishingContinues) {
  UdpReceiver receiver;
  FakeGateway gateway;
  const std::vector<UdpReceiver*> receivers{&receiver};

  pub::Publisher publisher(make_config(gateway, receivers));
  std::string error;
  ASSERT_TRUE(publisher.open(error)) << error;

  ASSERT_TRUE(
      drive_with_receivers(publisher, receivers, [&] { return gateway.connections() >= 1; }));
  const std::vector<std::vector<std::byte>> first = make_stream(1, 20);
  gateway.send_bytes(concatenate(first));
  ASSERT_TRUE(drive_with_receivers(publisher, receivers,
                                   [&] { return receiver.frames().size() >= first.size(); }));

  RecoveryClient alpha;
  RecoveryClient beta;
  ASSERT_TRUE(alpha.connect_to(publisher.recovery_port()));
  ASSERT_TRUE(beta.connect_to(publisher.recovery_port()));
  ASSERT_TRUE(alpha.send_request(2, 4, 1));
  ASSERT_TRUE(beta.send_request(10, 12, 1));

  for (int tick = 0; tick < 4000; ++tick) {
    publisher.poll_once(2ms);
    alpha.drain();
    beta.drain();
    if (alpha.frames().size() >= 3 && beta.frames().size() >= 3) {
      break;
    }
  }
  ASSERT_EQ(alpha.frames().size(), 3U);
  ASSERT_EQ(beta.frames().size(), 3U);
  for (std::size_t index = 0; index < 3; ++index) {
    EXPECT_EQ(alpha.frames()[index], first[1 + index]);
    EXPECT_EQ(beta.frames()[index], first[9 + index]);
  }

  // Live publishing keeps going while alpha stays connected.
  const std::vector<std::vector<std::byte>> second = make_stream(21, 25);
  gateway.send_bytes(concatenate(second));
  ASSERT_TRUE(
      drive_with_receivers(publisher, receivers, [&] { return receiver.frames().size() >= 25; }));

  ASSERT_TRUE(alpha.send_request(21, 23, 2));
  ASSERT_TRUE(drive_recovery(publisher, alpha, [&] { return alpha.frames().size() >= 6; }));
  ASSERT_EQ(alpha.frames().size(), 6U);
  for (std::size_t index = 0; index < 3; ++index) {
    EXPECT_EQ(alpha.frames()[3 + index], second[index]);
  }
  // Beta received no extra frames: its stream is independent.
  beta.drain();
  EXPECT_EQ(beta.frames().size(), 3U);
  EXPECT_EQ(publisher.stats().recovery_requests, 3U);
}

}  // namespace
