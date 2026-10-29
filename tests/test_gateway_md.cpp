// Market-data event stream tests (issue #120): sequence contiguity, late
// subscribers and slow-subscriber eviction, all on loopback against an
// in-process gateway.

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
#include <memory>
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

int connect_to(std::uint16_t port, int receive_buffer = 0) {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    return -1;
  }
  if (receive_buffer > 0) {
    ::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &receive_buffer, sizeof receive_buffer);
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

class FrameCapture final : public proto::FrameSink {
 public:
  bool on_frame(std::span<const std::byte> frame) override {
    bytes.assign(frame.begin(), frame.end());
    return false;
  }

  std::vector<std::byte> bytes;
};

class WireClient {
 public:
  WireClient(std::uint16_t port, int receive_buffer = 0) : fd_(connect_to(port, receive_buffer)) {}
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

  std::optional<std::vector<std::byte>> read_frame(int timeout_ms = 10000) {
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

std::optional<DecodedFrame> next_frame(WireClient& client, int timeout_ms = 10000) {
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

/// Next market-data frame that carries an event. Idle heartbeats are skipped:
/// they repeat the last published sequence number and never advance the stream,
/// so a test that is about events should not have to know when one lands.
std::optional<DecodedFrame> next_event_frame(WireClient& client, int timeout_ms = 10000) {
  for (;;) {
    std::optional<DecodedFrame> frame = next_frame(client, timeout_ms);
    if (!frame || frame->type != proto::MessageType::Heartbeat) {
      return frame;
    }
  }
}

template <class Message>
std::optional<Message> next_message(WireClient& client, proto::MessageType expected,
                                    int timeout_ms = 10000) {
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
bool wait_until(Predicate predicate, int timeout_ms = 20000) {
  for (int elapsed = 0; elapsed < timeout_ms; ++elapsed) {
    if (predicate()) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return predicate();
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

/// An order-entry session used to generate market data.
class Entry {
 public:
  explicit Entry(std::uint16_t port) : client_(port) {}
  WireClient& client() { return client_; }

  bool submit(std::uint64_t id, lob::Side side, std::int64_t price, std::int64_t qty) {
    return client_.send(limit_order(id, side, price, qty));
  }

  bool read_ack() { return next_message<proto::Ack>(client_, proto::MessageType::Ack).has_value(); }

 private:
  WireClient client_;
};

class GatewayMarketDataTest : public ::testing::Test {
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

  std::uint16_t entry_port() const { return gateway_->order_entry_port(); }
  std::uint16_t md_port() const { return gateway_->md_port(); }

  std::unique_ptr<Gateway> gateway_;
};

TEST_F(GatewayMarketDataTest, SequenceIsContiguousAcrossOneHundredThousandEvents) {
  constexpr std::uint64_t kOrders = 100000;
  std::atomic<bool> contiguous{true};
  std::atomic<bool> reader_failed{false};
  std::atomic<std::uint64_t> received{0};

  std::thread reader([&] {
    WireClient md(md_port());
    if (!md.connected()) {
      reader_failed = true;
      return;
    }
    std::uint64_t expected = 1;
    for (std::uint64_t i = 0; i < kOrders; ++i) {
      const std::optional<DecodedFrame> frame = next_event_frame(md, 20000);
      if (!frame) {
        reader_failed = true;
        return;
      }
      if (frame->type != proto::MessageType::BookUpdate) {
        reader_failed = true;
        return;
      }
      if (frame->seq != expected) {
        contiguous = false;
        expected = frame->seq;
      }
      ++expected;
      ++received;
    }
  });
  ASSERT_TRUE(wait_until([&] { return gateway_->subscriber_count() == 1; }));

  Entry entry(entry_port());
  ASSERT_TRUE(entry.client().connected());
  constexpr std::uint64_t kChunk = 1000;
  for (std::uint64_t base = 0; base < kOrders; base += kChunk) {
    for (std::uint64_t i = 0; i < kChunk; ++i) {
      const std::uint64_t id = base + i + 1;
      const std::int64_t price = 100 + static_cast<std::int64_t>(id % 1000);
      ASSERT_TRUE(entry.submit(id, lob::Side::Buy, price, 1));
    }
    for (std::uint64_t i = 0; i < kChunk; ++i) {
      ASSERT_TRUE(entry.read_ack());
    }
  }

  reader.join();
  EXPECT_FALSE(reader_failed.load());
  EXPECT_TRUE(contiguous.load());
  EXPECT_EQ(received.load(), kOrders);
  EXPECT_TRUE(wait_until([&] { return gateway_->md_sequence_published() == kOrders; }));
  EXPECT_TRUE(wait_until([&] { return gateway_->md_sequence_forwarded() == kOrders; }));
}

TEST_F(GatewayMarketDataTest, LateSubscriberStartsAtTheCurrentSequence) {
  Entry entry(entry_port());
  ASSERT_TRUE(entry.client().connected());
  for (std::uint64_t id = 1; id <= 10; ++id) {
    ASSERT_TRUE(entry.submit(id, lob::Side::Buy, 100 + static_cast<std::int64_t>(id), 1));
    ASSERT_TRUE(entry.read_ack());
  }
  ASSERT_TRUE(wait_until([&] { return gateway_->md_sequence_published() == 10; }));
  ASSERT_TRUE(wait_until([&] { return gateway_->md_sequence_forwarded() == 10; }));

  WireClient md(md_port());
  ASSERT_TRUE(md.connected());
  ASSERT_TRUE(wait_until([&] { return gateway_->subscriber_count() == 1; }));

  for (std::uint64_t id = 11; id <= 15; ++id) {
    ASSERT_TRUE(entry.submit(id, lob::Side::Buy, 100 + static_cast<std::int64_t>(id), 1));
    ASSERT_TRUE(entry.read_ack());
  }
  ASSERT_TRUE(wait_until([&] { return gateway_->md_sequence_forwarded() == 15; }));

  for (proto::Seq expected = 11; expected <= 15; ++expected) {
    const std::optional<DecodedFrame> frame = next_event_frame(md);
    ASSERT_TRUE(frame.has_value());
    EXPECT_EQ(frame->seq, expected);
    EXPECT_EQ(frame->type, proto::MessageType::BookUpdate);
  }
}

TEST_F(GatewayMarketDataTest, EverySubscriberSeesTradesAndBookUpdatesInOrder) {
  WireClient first(md_port());
  WireClient second(md_port());
  ASSERT_TRUE(first.connected());
  ASSERT_TRUE(second.connected());
  ASSERT_TRUE(wait_until([&] { return gateway_->subscriber_count() == 2; }));

  Entry maker(entry_port());
  Entry taker(entry_port());
  ASSERT_TRUE(maker.client().connected());
  ASSERT_TRUE(maker.submit(10, lob::Side::Sell, 100, 5));
  ASSERT_TRUE(maker.read_ack());
  ASSERT_TRUE(taker.client().connected());
  ASSERT_TRUE(taker.submit(11, lob::Side::Buy, 100, 5));
  ASSERT_TRUE(taker.read_ack());

  ASSERT_TRUE(wait_until([&] { return gateway_->md_sequence_forwarded() == 3; }));

  for (WireClient* subscriber : {&first, &second}) {
    const std::optional<DecodedFrame> update_before = next_event_frame(*subscriber);
    ASSERT_TRUE(update_before.has_value());
    EXPECT_EQ(update_before->seq, 1u);
    EXPECT_EQ(update_before->type, proto::MessageType::BookUpdate);
    const std::optional<proto::BookUpdate> before = decode_as<proto::BookUpdate>(*update_before);
    ASSERT_TRUE(before.has_value());
    EXPECT_EQ(before->side, lob::Side::Sell);
    EXPECT_EQ(before->price, 100);
    EXPECT_EQ(before->qty, 5);

    const std::optional<DecodedFrame> trade_frame = next_event_frame(*subscriber);
    ASSERT_TRUE(trade_frame.has_value());
    EXPECT_EQ(trade_frame->seq, 2u);
    EXPECT_EQ(trade_frame->type, proto::MessageType::Trade);
    const std::optional<proto::Trade> trade = decode_as<proto::Trade>(*trade_frame);
    ASSERT_TRUE(trade.has_value());
    // Engine order ids: session 1 placed the maker, session 2 the taker.
    EXPECT_EQ(trade->maker_id, (std::uint64_t{1} << 40) | 10);
    EXPECT_EQ(trade->taker_id, (std::uint64_t{2} << 40) | 11);
    EXPECT_EQ(trade->price, 100);
    EXPECT_EQ(trade->qty, 5);

    const std::optional<DecodedFrame> update_after = next_event_frame(*subscriber);
    ASSERT_TRUE(update_after.has_value());
    EXPECT_EQ(update_after->seq, 3u);
    EXPECT_EQ(update_after->type, proto::MessageType::BookUpdate);
    const std::optional<proto::BookUpdate> after = decode_as<proto::BookUpdate>(*update_after);
    ASSERT_TRUE(after.has_value());
    EXPECT_EQ(after->side, lob::Side::Sell);
    EXPECT_EQ(after->price, 100);
    EXPECT_EQ(after->qty, 0);
  }
}

TEST_F(GatewayMarketDataTest, StalledSubscriberIsDroppedWithoutAffectingOrderEntry) {
  // A 16 KiB receive buffer and a subscriber that never reads make the
  // gateway's 64 KiB send buffer and 1 MiB output cap fill quickly.
  WireClient stalled(md_port(), 16 * 1024);
  ASSERT_TRUE(stalled.connected());
  ASSERT_TRUE(wait_until([&] { return gateway_->subscriber_count() == 1; }));

  Entry entry(entry_port());
  ASSERT_TRUE(entry.client().connected());
  constexpr std::uint64_t kOrders = 40000;
  constexpr std::uint64_t kChunk = 500;
  for (std::uint64_t base = 0; base < kOrders; base += kChunk) {
    for (std::uint64_t i = 0; i < kChunk; ++i) {
      const std::uint64_t id = base + i + 1;
      const std::int64_t price = 100 + static_cast<std::int64_t>(id % 1000);
      ASSERT_TRUE(entry.submit(id, lob::Side::Buy, price, 1));
    }
    for (std::uint64_t i = 0; i < kChunk; ++i) {
      ASSERT_TRUE(entry.read_ack());
    }
  }

  EXPECT_TRUE(wait_until([&] { return gateway_->subscriber_count() == 0; }));

  // The engine and the surviving order-entry session are unaffected.
  ASSERT_TRUE(entry.submit(kOrders + 1, lob::Side::Buy, 500, 1));
  EXPECT_TRUE(entry.read_ack());
}

TEST_F(GatewayMarketDataTest, IdleStreamEmitsHeartbeatsThatRepeatTheLastSequence) {
  WireClient md(md_port());
  ASSERT_TRUE(md.connected());
  ASSERT_TRUE(wait_until([&] { return gateway_->subscriber_count() == 1; }));

  Entry entry(entry_port());
  ASSERT_TRUE(entry.client().connected());
  ASSERT_TRUE(entry.submit(1, lob::Side::Buy, 100, 5));
  ASSERT_TRUE(entry.read_ack());

  const std::optional<DecodedFrame> update = next_frame(md);
  ASSERT_TRUE(update.has_value());
  EXPECT_EQ(update->type, proto::MessageType::BookUpdate);
  EXPECT_EQ(update->seq, 1u);

  // The stream is now idle. After the configured interval the gateway must
  // emit a heartbeat that repeats sequence 1 rather than consuming sequence 2.
  const std::optional<DecodedFrame> beat = next_frame(md, 2000);
  ASSERT_TRUE(beat.has_value());
  EXPECT_EQ(beat->type, proto::MessageType::Heartbeat);
  EXPECT_EQ(beat->seq, 1u);
  const std::optional<proto::Heartbeat> decoded = decode_as<proto::Heartbeat>(*beat);
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(decoded->nonce, 0u);
  EXPECT_EQ(gateway_->md_sequence_published(), 1u);
  EXPECT_EQ(gateway_->md_sequence_forwarded(), 1u);

  // Idle heartbeats never take a sequence number: the next real event is 2,
  // and any heartbeat still in flight repeats 1.
  ASSERT_TRUE(entry.submit(2, lob::Side::Buy, 101, 3));
  ASSERT_TRUE(entry.read_ack());
  const std::optional<DecodedFrame> next_update = next_event_frame(md, 2000);
  ASSERT_TRUE(next_update.has_value());
  EXPECT_EQ(next_update->type, proto::MessageType::BookUpdate);
  EXPECT_EQ(next_update->seq, 2u);
  EXPECT_EQ(gateway_->md_sequence_published(), 2u);
}

TEST_F(GatewayMarketDataTest, NoHeartbeatBeforeTheFirstEvent) {
  // A heartbeat needs a published sequence number to repeat. With no traffic
  // there is nothing to say, so an idle gateway stays silent (documented
  // choice: seq 0 heartbeats are not emitted).
  WireClient md(md_port());
  ASSERT_TRUE(md.connected());
  ASSERT_TRUE(wait_until([&] { return gateway_->subscriber_count() == 1; }));

  EXPECT_FALSE(next_frame(md, 300).has_value());
  EXPECT_FALSE(md.eof());
  EXPECT_EQ(gateway_->md_sequence_published(), 0u);
}

}  // namespace
