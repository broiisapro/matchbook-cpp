// Order-entry gateway tests (issue #119): real loopback sockets against an
// in-process gateway on port 0. No test sleeps for more than 50 ms and none
// asserts on wall time.

#include <gtest/gtest.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <thread>
#include <utility>
#include <vector>

#include "net/gateway/gateway.hpp"
#include "net/protocol/codec.hpp"
#include "net/protocol/framer.hpp"
#include "net/protocol/messages.hpp"

namespace {

namespace proto = lob::net;

using lob::net::Gateway;
using lob::net::GatewayOptions;

int connect_to(std::uint16_t port) {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    return -1;
  }
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(port);
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof address) != 0) {
    ::close(fd);
    return -1;
  }
  return fd;
}

/// Copies the first complete frame the framer hands it and stops the push.
class FrameCapture final : public proto::FrameSink {
 public:
  bool on_frame(std::span<const std::byte> frame) override {
    bytes.assign(frame.begin(), frame.end());
    return false;
  }

  std::vector<std::byte> bytes;
};

/// Minimal blocking wire client for tests: encodes requests, frames responses
/// and exposes a bounded wait and an end-of-stream check.
class WireClient {
 public:
  explicit WireClient(std::uint16_t port) : fd_(connect_to(port)) {}
  ~WireClient() { close(); }

  WireClient(const WireClient&) = delete;
  WireClient& operator=(const WireClient&) = delete;

  bool connected() const { return fd_ >= 0; }

  void close() {
    if (fd_ >= 0) {
      ::close(fd_);
      fd_ = -1;
    }
  }

  template <class Message>
  bool send(const Message& message) {
    std::array<std::byte, Message::kWireSize> buffer{};
    const std::size_t size = proto::encode(message, next_seq_++, buffer);
    return send_bytes(std::span<const std::byte>(buffer.data(), size));
  }

  bool send_bytes(std::span<const std::byte> bytes) {
    std::size_t offset = 0;
    while (offset < bytes.size()) {
      const ssize_t written = ::send(fd_, bytes.data() + offset, bytes.size() - offset, 0);
      if (written > 0) {
        offset += static_cast<std::size_t>(written);
        continue;
      }
      if (written < 0 && errno == EINTR) {
        continue;
      }
      return false;
    }
    return true;
  }

  /// Reads one complete frame. Returns nullopt on timeout or after the peer
  /// closed; eof() tells the two apart.
  std::optional<std::vector<std::byte>> read_frame(int timeout_ms = 5000) {
    for (;;) {
      if (!pending_.empty()) {
        FrameCapture capture;
        const proto::PushResult result = framer_.push(pending_, capture);
        pending_.erase(pending_.begin(),
                       pending_.begin() + static_cast<std::ptrdiff_t>(result.consumed));
        if (result.result == proto::FramerResult::Error) {
          return std::nullopt;
        }
        if (!capture.bytes.empty()) {
          return std::move(capture.bytes);
        }
      }
      if (!wait_readable(timeout_ms)) {
        return std::nullopt;
      }
      std::array<std::byte, 4096> buffer{};
      const ssize_t received = ::recv(fd_, buffer.data(), buffer.size(), 0);
      if (received == 0) {
        eof_ = true;
        return std::nullopt;
      }
      if (received < 0) {
        if (errno == EINTR) {
          continue;
        }
        return std::nullopt;
      }
      pending_.insert(pending_.end(), buffer.begin(), buffer.begin() + received);
    }
  }

  /// Consumes whatever is readable (discarding it) until the peer closes.
  bool wait_for_close(int timeout_ms = 5000) {
    for (;;) {
      if (!wait_readable(timeout_ms)) {
        return false;
      }
      std::array<std::byte, 4096> buffer{};
      const ssize_t received = ::recv(fd_, buffer.data(), buffer.size(), 0);
      if (received == 0) {
        eof_ = true;
        return true;
      }
      if (received < 0) {
        if (errno == EINTR) {
          continue;
        }
        return false;
      }
    }
  }

  bool eof() const { return eof_; }

 private:
  bool wait_readable(int timeout_ms) {
    pollfd descriptor{fd_, POLLIN, 0};
    const int ready = ::poll(&descriptor, 1, timeout_ms);
    return ready > 0 && (descriptor.revents & POLLIN) != 0;
  }

  int fd_ = -1;
  proto::Seq next_seq_ = 1;
  proto::StreamFramer framer_;
  std::vector<std::byte> pending_;
  bool eof_ = false;
};

struct DecodedFrame {
  proto::MessageType type = proto::MessageType::Ack;
  proto::Seq seq = 0;
  std::vector<std::byte> bytes;
};

std::optional<DecodedFrame> next_frame(WireClient& client, int timeout_ms = 5000) {
  std::optional<std::vector<std::byte>> bytes = client.read_frame(timeout_ms);
  if (!bytes) {
    return std::nullopt;
  }
  proto::Header header;
  if (proto::decode_header(*bytes, header) != proto::DecodeStatus::Ok) {
    return std::nullopt;
  }
  return DecodedFrame{header.type, header.seq, std::move(*bytes)};
}

template <class Message>
std::optional<Message> next_message(WireClient& client, proto::MessageType expected,
                                    int timeout_ms = 5000) {
  std::optional<DecodedFrame> frame = next_frame(client, timeout_ms);
  if (!frame || frame->type != expected) {
    return std::nullopt;
  }
  Message message;
  proto::Seq seq = 0;
  if (proto::decode(frame->bytes, message, seq) != proto::DecodeStatus::Ok) {
    return std::nullopt;
  }
  return message;
}

proto::NewOrder limit_order(std::uint64_t id, lob::Side side, std::int64_t price,
                            std::int64_t qty) {
  proto::NewOrder order;
  order.order_id = id;
  order.price = price;
  order.qty = qty;
  order.side = side;
  order.type = lob::OrderType::Limit;
  return order;
}

template <class Message>
std::optional<Message> decode_as(const DecodedFrame& frame) {
  Message message;
  proto::Seq seq = 0;
  if (proto::decode(frame.bytes, message, seq) != proto::DecodeStatus::Ok) {
    return std::nullopt;
  }
  return message;
}

template <class Predicate>
bool wait_until(Predicate predicate, int timeout_ms = 10000) {
  for (int elapsed = 0; elapsed < timeout_ms; ++elapsed) {
    if (predicate()) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return predicate();
}

class GatewayTest : public ::testing::Test {
 protected:
  void SetUp() override {
    GatewayOptions options;
    options.port = 0;
    options.md_port = 0;
    gateway_ = std::make_unique<Gateway>(options);
    gateway_->start();
  }

  void TearDown() override {
    if (gateway_) {
      gateway_->stop();
    }
  }

  std::uint16_t port() const { return gateway_->order_entry_port(); }

  std::unique_ptr<Gateway> gateway_;
};

TEST_F(GatewayTest, OrderEntryAcknowledgesAnOrderAndEchoesClientId) {
  WireClient client(port());
  ASSERT_TRUE(client.connected());

  ASSERT_TRUE(client.send(limit_order(7, lob::Side::Buy, 100, 10)));
  const std::optional<proto::Ack> ack = next_message<proto::Ack>(client, proto::MessageType::Ack);
  ASSERT_TRUE(ack.has_value());
  EXPECT_EQ(ack->order_id, 7u);

  std::optional<DecodedFrame> extra = next_frame(client, 50);
  EXPECT_FALSE(extra.has_value());
  EXPECT_FALSE(client.eof());
}

TEST_F(GatewayTest, CrossingOrdersFillBothSessions) {
  WireClient maker(port());
  WireClient taker(port());
  ASSERT_TRUE(maker.connected());
  ASSERT_TRUE(taker.connected());

  ASSERT_TRUE(maker.send(limit_order(11, lob::Side::Sell, 100, 5)));
  ASSERT_TRUE(next_message<proto::Ack>(maker, proto::MessageType::Ack).has_value());

  ASSERT_TRUE(taker.send(limit_order(22, lob::Side::Buy, 100, 5)));

  const std::optional<proto::Ack> ack = next_message<proto::Ack>(taker, proto::MessageType::Ack);
  ASSERT_TRUE(ack.has_value());
  EXPECT_EQ(ack->order_id, 22u);
  const std::optional<proto::Fill> taker_fill =
      next_message<proto::Fill>(taker, proto::MessageType::Fill);
  ASSERT_TRUE(taker_fill.has_value());

  const std::optional<proto::Fill> maker_fill =
      next_message<proto::Fill>(maker, proto::MessageType::Fill);
  ASSERT_TRUE(maker_fill.has_value());

  for (const proto::Fill* fill : {&*taker_fill, &*maker_fill}) {
    EXPECT_EQ(fill->maker_id, 11u);
    EXPECT_EQ(fill->taker_id, 22u);
    EXPECT_EQ(fill->price, 100);
    EXPECT_EQ(fill->qty, 5);
  }
}

TEST_F(GatewayTest, CancelOfAnUnknownOrderIsRejected) {
  WireClient client(port());
  ASSERT_TRUE(client.connected());

  proto::Cancel cancel;
  cancel.order_id = 999;
  ASSERT_TRUE(client.send(cancel));
  const std::optional<proto::Reject> reject =
      next_message<proto::Reject>(client, proto::MessageType::Reject);
  ASSERT_TRUE(reject.has_value());
  EXPECT_EQ(reject->order_id, 999u);
  EXPECT_EQ(reject->reason, lob::RejectReason::UnknownOrder);
}

TEST_F(GatewayTest, CancelOfARestingOrderReportsTheRemovedQuantity) {
  WireClient client(port());
  ASSERT_TRUE(client.connected());
  ASSERT_TRUE(client.send(limit_order(5, lob::Side::Buy, 100, 12)));
  ASSERT_TRUE(next_message<proto::Ack>(client, proto::MessageType::Ack).has_value());

  proto::Cancel cancel;
  cancel.order_id = 5;
  ASSERT_TRUE(client.send(cancel));
  const std::optional<proto::Cancelled> cancelled =
      next_message<proto::Cancelled>(client, proto::MessageType::Cancelled);
  ASSERT_TRUE(cancelled.has_value());
  EXPECT_EQ(cancelled->order_id, 5u);
  EXPECT_EQ(cancelled->qty, 12);
}

TEST_F(GatewayTest, ModifyAcknowledgeAndModifyUnknownIsRejected) {
  WireClient client(port());
  ASSERT_TRUE(client.connected());
  ASSERT_TRUE(client.send(limit_order(3, lob::Side::Buy, 100, 10)));
  ASSERT_TRUE(next_message<proto::Ack>(client, proto::MessageType::Ack).has_value());

  proto::Modify reduce;
  reduce.order_id = 3;
  reduce.price = 100;
  reduce.qty = 4;
  ASSERT_TRUE(client.send(reduce));
  // The protocol has no Modified message, so an in-place reduce is answered
  // with an Ack for the same order (docs/architecture.md section 15).
  const std::optional<proto::Ack> ack = next_message<proto::Ack>(client, proto::MessageType::Ack);
  ASSERT_TRUE(ack.has_value());
  EXPECT_EQ(ack->order_id, 3u);

  proto::Modify unknown;
  unknown.order_id = 44;
  unknown.price = 101;
  unknown.qty = 1;
  ASSERT_TRUE(client.send(unknown));
  const std::optional<proto::Reject> reject =
      next_message<proto::Reject>(client, proto::MessageType::Reject);
  ASSERT_TRUE(reject.has_value());
  EXPECT_EQ(reject->order_id, 44u);
  EXPECT_EQ(reject->reason, lob::RejectReason::UnknownOrder);
}

TEST_F(GatewayTest, HeartbeatEchoesTheNonce) {
  WireClient client(port());
  ASSERT_TRUE(client.connected());

  proto::Heartbeat heartbeat;
  heartbeat.nonce = 0x0123456789abcdefULL;
  ASSERT_TRUE(client.send(heartbeat));
  const std::optional<proto::Heartbeat> echo =
      next_message<proto::Heartbeat>(client, proto::MessageType::Heartbeat);
  ASSERT_TRUE(echo.has_value());
  EXPECT_EQ(echo->nonce, heartbeat.nonce);
}

TEST_F(GatewayTest, MalformedFrameClosesOnlyThatSession) {
  WireClient broken(port());
  WireClient healthy(port());
  ASSERT_TRUE(broken.connected());
  ASSERT_TRUE(healthy.connected());

  // A NewOrder header with a non-zero reserved field: the framer rejects it as
  // soon as the header is complete.
  std::array<std::byte, proto::kHeaderSize> header{};
  header[0] = std::byte{1};   // type NewOrder
  header[2] = std::byte{32};  // body size 32
  header[15] = std::byte{9};  // reserved must be zero
  ASSERT_TRUE(broken.send_bytes(header));
  EXPECT_TRUE(broken.wait_for_close());
  EXPECT_TRUE(broken.eof());

  ASSERT_TRUE(healthy.send(limit_order(1, lob::Side::Buy, 100, 1)));
  EXPECT_TRUE(next_message<proto::Ack>(healthy, proto::MessageType::Ack).has_value());
  EXPECT_EQ(gateway_->session_count(), 1u);
}

TEST_F(GatewayTest, FramesSplitAcrossWritesAreReassembled) {
  WireClient client(port());
  ASSERT_TRUE(client.connected());

  std::array<std::byte, proto::NewOrder::kWireSize> buffer{};
  const std::size_t size = proto::encode(limit_order(9, lob::Side::Sell, 50, 3), 1, buffer);
  for (std::size_t i = 0; i < size; ++i) {
    ASSERT_TRUE(client.send_bytes(std::span<const std::byte>(buffer.data() + i, 1)));
  }

  const std::optional<proto::Ack> ack = next_message<proto::Ack>(client, proto::MessageType::Ack);
  ASSERT_TRUE(ack.has_value());
  EXPECT_EQ(ack->order_id, 9u);
}

TEST_F(GatewayTest, SeveralFramesInOneWriteAreAllAnswered) {
  WireClient client(port());
  ASSERT_TRUE(client.connected());

  std::array<std::byte, 3 * proto::NewOrder::kWireSize> buffer{};
  std::size_t offset = 0;
  for (std::uint64_t id = 1; id <= 3; ++id) {
    offset += proto::encode(limit_order(id, lob::Side::Buy, 100 + static_cast<std::int64_t>(id), 1),
                            id, std::span<std::byte>(buffer).subspan(offset));
  }
  ASSERT_TRUE(client.send_bytes(std::span<const std::byte>(buffer.data(), offset)));
  for (std::uint64_t id = 1; id <= 3; ++id) {
    const std::optional<proto::Ack> ack = next_message<proto::Ack>(client, proto::MessageType::Ack);
    ASSERT_TRUE(ack.has_value());
    EXPECT_EQ(ack->order_id, id);
  }
}

TEST_F(GatewayTest, ClientIdOutsideFortyBitsIsRejectedAndTheSessionSurvives) {
  WireClient client(port());
  ASSERT_TRUE(client.connected());

  const std::uint64_t too_large = std::uint64_t{1} << 40;
  ASSERT_TRUE(client.send(limit_order(too_large, lob::Side::Buy, 100, 1)));
  const std::optional<proto::Reject> reject =
      next_message<proto::Reject>(client, proto::MessageType::Reject);
  ASSERT_TRUE(reject.has_value());
  EXPECT_EQ(reject->order_id, too_large);
  EXPECT_EQ(reject->reason, lob::RejectReason::CapacityExceeded);

  ASSERT_TRUE(client.send(limit_order(1, lob::Side::Buy, 100, 1)));
  EXPECT_TRUE(next_message<proto::Ack>(client, proto::MessageType::Ack).has_value());
}

TEST_F(GatewayTest, TenThousandOrdersFromFourSessionsAreAnsweredExactlyOnce) {
  constexpr std::uint64_t kOrdersPerSession = 2500;
  constexpr int kSessions = 4;
  std::atomic<int> failures{0};

  std::vector<std::thread> threads;
  threads.reserve(kSessions);
  for (int session = 0; session < kSessions; ++session) {
    threads.emplace_back([&, session] {
      WireClient client(port());
      if (!client.connected()) {
        ++failures;
        return;
      }
      for (std::uint64_t id = 1; id <= kOrdersPerSession; ++id) {
        const std::int64_t price =
            100 + static_cast<std::int64_t>(session) * 1000 + static_cast<std::int64_t>(id % 1000);
        if (!client.send(limit_order(id, lob::Side::Buy, price, 1))) {
          ++failures;
          return;
        }
      }
      std::vector<bool> answered(kOrdersPerSession + 1, false);
      for (std::uint64_t i = 0; i < kOrdersPerSession; ++i) {
        const std::optional<proto::Ack> ack =
            next_message<proto::Ack>(client, proto::MessageType::Ack, 30000);
        if (!ack || ack->order_id < 1 || ack->order_id > kOrdersPerSession ||
            answered[ack->order_id]) {
          ++failures;
          return;
        }
        answered[ack->order_id] = true;
      }
    });
  }
  for (std::thread& thread : threads) {
    thread.join();
  }
  EXPECT_EQ(failures.load(), 0);
}

TEST_F(GatewayTest, MarketDataStreamCarriesBookUpdates) {
  WireClient subscriber(gateway_->md_port());
  ASSERT_TRUE(subscriber.connected());
  ASSERT_TRUE(wait_until([&] { return gateway_->subscriber_count() == 1; }));

  WireClient client(port());
  ASSERT_TRUE(client.connected());
  ASSERT_TRUE(client.send(limit_order(4, lob::Side::Buy, 120, 7)));
  ASSERT_TRUE(next_message<proto::Ack>(client, proto::MessageType::Ack).has_value());

  const std::optional<DecodedFrame> frame = next_frame(subscriber);
  ASSERT_TRUE(frame.has_value());
  EXPECT_EQ(frame->type, proto::MessageType::BookUpdate);
  EXPECT_EQ(frame->seq, 1u);
  const std::optional<proto::BookUpdate> update = decode_as<proto::BookUpdate>(*frame);
  ASSERT_TRUE(update.has_value());
  EXPECT_EQ(update->side, lob::Side::Buy);
  EXPECT_EQ(update->price, 120);
  EXPECT_EQ(update->qty, 7);
}

TEST_F(GatewayTest, StopClosesSessionsAfterAnsweringQueuedCommands) {
  WireClient client(port());
  ASSERT_TRUE(client.connected());
  ASSERT_TRUE(client.send(limit_order(1, lob::Side::Buy, 100, 1)));
  ASSERT_TRUE(next_message<proto::Ack>(client, proto::MessageType::Ack).has_value());

  gateway_->stop();
  EXPECT_TRUE(client.wait_for_close());
}

}  // namespace
