#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "net/md_client/feed_handler.hpp"
#include "net/md_client/testing/fake_publisher.hpp"

namespace {

namespace md = lob::md;
namespace proto = lob::net;
using md::testing::book_update;
using md::testing::sequence_reset;

using Frames = std::vector<std::vector<std::byte>>;

std::unique_ptr<md::FeedHandler> make_handler(md::testing::FakePublisher& publisher,
                                              bool auto_recovery = true,
                                              std::size_t max_buffered = 4096) {
  md::FeedHandlerConfig config;
  config.udp_port = 0;
  config.bind_address = "127.0.0.1";
  config.recovery_host = "127.0.0.1";
  config.recovery_port = publisher.recovery_port();
  config.recovery_timeout_ms = 3000;
  config.auto_recovery = auto_recovery;
  config.max_buffered_messages = max_buffered;

  auto handler = std::make_unique<md::FeedHandler>(config);
  std::string error;
  if (!handler->open(error)) {
    ADD_FAILURE() << "cannot open the feed socket: " << error;
    return nullptr;
  }
  return handler;
}

/// Polls the handler until `done` holds. Poll timeouts are 1 ms so the tests
/// stay responsive without sleeping; the deadline is a hard stop against a
/// hang, not a timing assertion.
bool poll_until(md::FeedHandler& handler, const std::function<bool()>& done,
                int timeout_ms = 3000) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (!done()) {
    if (std::chrono::steady_clock::now() >= deadline) {
      return false;
    }
    handler.poll(1);
  }
  return true;
}

TEST(md_client_level_book, AppliesChangesAndRemovesZeroQuantity) {
  md::LevelBook book;
  book.apply(proto::BookUpdate{100, 5, lob::Side::Buy});
  book.apply(proto::BookUpdate{99, 8, lob::Side::Buy});
  book.apply(proto::BookUpdate{101, 3, lob::Side::Sell});
  book.apply(proto::BookUpdate{102, 4, lob::Side::Sell});
  book.apply(proto::BookUpdate{100, 9, lob::Side::Buy});
  book.apply(proto::BookUpdate{102, 0, lob::Side::Sell});

  EXPECT_EQ(book.quantity_at(lob::Side::Buy, 100), std::optional<lob::Qty>(9));
  EXPECT_EQ(book.quantity_at(lob::Side::Buy, 99), std::optional<lob::Qty>(8));
  EXPECT_EQ(book.quantity_at(lob::Side::Sell, 102), std::nullopt);
  EXPECT_EQ(book.bid_levels(), 2U);
  EXPECT_EQ(book.ask_levels(), 1U);
  EXPECT_EQ(book.top(lob::Side::Buy, 5), (std::vector<md::Level>{{100, 9}, {99, 8}}));
  EXPECT_EQ(book.top(lob::Side::Sell, 1), (std::vector<md::Level>{{101, 3}}));
  EXPECT_EQ(book.top(lob::Side::Sell, 0), std::vector<md::Level>{});

  book.clear();
  EXPECT_TRUE(book.empty());
}

TEST(md_client_feed_handler, InOrderStreamBuildsTheBook) {
  md::testing::FakePublisher publisher;
  auto handler = make_handler(publisher);
  ASSERT_NE(handler, nullptr);

  const Frames frames{
      book_update(100, 5, lob::Side::Buy, 1),  book_update(99, 7, lob::Side::Buy, 2),
      book_update(101, 4, lob::Side::Sell, 3), book_update(100, 9, lob::Side::Buy, 4),
      book_update(99, 0, lob::Side::Buy, 5),   book_update(101, 6, lob::Side::Sell, 6),
  };
  publisher.store_frames(frames);
  // Two messages per datagram: the handler must split a datagram into frames.
  publisher.send_packed(handler->udp_port(), frames, 2);

  ASSERT_TRUE(poll_until(*handler, [&] { return handler->expected_seq() == 7; }))
      << "counters: received=" << handler->counters().received;
  const md::FeedCounters& counters = handler->counters();
  EXPECT_EQ(counters.datagrams, 3U);
  EXPECT_EQ(counters.received, 6U);
  EXPECT_EQ(counters.duplicates, 0U);
  EXPECT_EQ(counters.gaps_detected, 0U);
  EXPECT_EQ(counters.gaps_recovered, 0U);
  EXPECT_EQ(counters.recovery_requests, 0U);
  EXPECT_EQ(counters.malformed, 0U);

  const md::LevelBook& book = handler->book();
  EXPECT_EQ(book.quantity_at(lob::Side::Buy, 100), std::optional<lob::Qty>(9));
  EXPECT_EQ(book.quantity_at(lob::Side::Buy, 99), std::nullopt);
  EXPECT_EQ(book.quantity_at(lob::Side::Sell, 101), std::optional<lob::Qty>(6));
  EXPECT_EQ(book.top(lob::Side::Buy, 5), (std::vector<md::Level>{{100, 9}}));
  EXPECT_EQ(book.top(lob::Side::Sell, 5), (std::vector<md::Level>{{101, 6}}));
}

TEST(md_client_feed_handler, DroppedDatagramsAreRecoveredOverTcp) {
  md::testing::FakePublisher publisher;
  auto handler = make_handler(publisher);
  ASSERT_NE(handler, nullptr);

  const Frames stream{
      book_update(100, 10, lob::Side::Buy, 1), book_update(101, 5, lob::Side::Buy, 2),
      book_update(102, 4, lob::Side::Sell, 3), book_update(103, 2, lob::Side::Sell, 4),
      book_update(100, 12, lob::Side::Buy, 5), book_update(102, 7, lob::Side::Sell, 6),
      book_update(99, 0, lob::Side::Buy, 7),   book_update(99, 3, lob::Side::Buy, 8),
  };
  publisher.store_frames(stream);

  // Datagrams 3 and 4 never reach the subscriber.
  const Frames delivered{stream[0], stream[1], stream[4], stream[5], stream[6], stream[7]};
  publisher.send_frames(handler->udp_port(), delivered);

  ASSERT_TRUE(poll_until(*handler, [&] { return handler->expected_seq() == 9; }));
  const md::FeedCounters& counters = handler->counters();
  EXPECT_EQ(counters.received, 6U);
  EXPECT_EQ(counters.duplicates, 0U);
  EXPECT_EQ(counters.gaps_detected, 1U);
  EXPECT_EQ(counters.gaps_recovered, 1U);
  EXPECT_GE(counters.recovery_requests, 1U);
  EXPECT_GE(counters.recovery_messages, 2U);
  EXPECT_EQ(counters.recovery_failures, 0U);
  EXPECT_GE(publisher.recovery_requests(), 1U);

  const md::LevelBook& book = handler->book();
  EXPECT_EQ(book.quantity_at(lob::Side::Buy, 100), std::optional<lob::Qty>(12));
  EXPECT_EQ(book.quantity_at(lob::Side::Sell, 102), std::optional<lob::Qty>(7));
  EXPECT_EQ(book.quantity_at(lob::Side::Sell, 103), std::optional<lob::Qty>(2));
  EXPECT_EQ(book.quantity_at(lob::Side::Buy, 99), std::optional<lob::Qty>(3));
}

TEST(md_client_feed_handler, DuplicatedAndReorderedDatagramsAreAppliedOnce) {
  md::testing::FakePublisher publisher;
  auto handler = make_handler(publisher, /*auto_recovery=*/false);
  ASSERT_NE(handler, nullptr);

  const Frames frames{
      book_update(102, 4, lob::Side::Sell, 3),
      book_update(100, 10, lob::Side::Buy, 1),
      book_update(101, 5, lob::Side::Buy, 2),
      book_update(101, 5, lob::Side::Buy, 2),  // duplicate datagram
      book_update(103, 2, lob::Side::Sell, 4),
  };
  publisher.send_frames(handler->udp_port(), frames);

  ASSERT_TRUE(poll_until(*handler, [&] { return handler->expected_seq() == 5; }));
  const md::FeedCounters& counters = handler->counters();
  EXPECT_EQ(counters.received, 5U);
  EXPECT_EQ(counters.duplicates, 1U);
  EXPECT_EQ(counters.gaps_detected, 1U);
  EXPECT_EQ(counters.gaps_recovered, 0U);
  EXPECT_EQ(counters.recovery_requests, 0U);
  EXPECT_EQ(handler->buffered(), 0U);

  const md::LevelBook& book = handler->book();
  EXPECT_EQ(book.quantity_at(lob::Side::Buy, 100), std::optional<lob::Qty>(10));
  EXPECT_EQ(book.quantity_at(lob::Side::Buy, 101), std::optional<lob::Qty>(5));
  EXPECT_EQ(book.quantity_at(lob::Side::Sell, 102), std::optional<lob::Qty>(4));
  EXPECT_EQ(book.quantity_at(lob::Side::Sell, 103), std::optional<lob::Qty>(2));
}

TEST(md_client_feed_handler, OutOfOrderBufferIsBounded) {
  md::testing::FakePublisher publisher;
  auto handler = make_handler(publisher, /*auto_recovery=*/false, /*max_buffered=*/2);
  ASSERT_NE(handler, nullptr);

  publisher.send_frames(handler->udp_port(), {book_update(100, 1, lob::Side::Buy, 5),
                                              book_update(100, 2, lob::Side::Buy, 6),
                                              book_update(100, 3, lob::Side::Buy, 7)});
  ASSERT_TRUE(poll_until(*handler, [&] { return handler->counters().received == 3; }));
  EXPECT_EQ(handler->buffered(), 2U);
  EXPECT_EQ(handler->counters().dropped, 1U);
  EXPECT_EQ(handler->expected_seq(), 1U);

  publisher.send_frames(
      handler->udp_port(),
      {book_update(100, 4, lob::Side::Buy, 1), book_update(100, 5, lob::Side::Buy, 2),
       book_update(100, 6, lob::Side::Buy, 3), book_update(100, 7, lob::Side::Buy, 4)});
  ASSERT_TRUE(poll_until(*handler, [&] { return handler->expected_seq() == 7; }));
  EXPECT_EQ(handler->buffered(), 0U);
  EXPECT_EQ(handler->book().quantity_at(lob::Side::Buy, 100), std::optional<lob::Qty>(2));
}

TEST(md_client_feed_handler, SequenceResetJumpsForwardInTheLiveStream) {
  md::testing::FakePublisher publisher;
  auto handler = make_handler(publisher, /*auto_recovery=*/false);
  ASSERT_NE(handler, nullptr);

  const Frames frames{
      book_update(100, 5, lob::Side::Buy, 1),   book_update(101, 4, lob::Side::Buy, 2),
      book_update(102, 3, lob::Side::Sell, 3),  sequence_reset(10, 4),
      book_update(102, 9, lob::Side::Sell, 10), book_update(99, 2, lob::Side::Buy, 11),
  };
  publisher.send_frames(handler->udp_port(), frames);

  ASSERT_TRUE(poll_until(*handler, [&] { return handler->expected_seq() == 12; }));
  EXPECT_EQ(handler->counters().resets, 1U);
  EXPECT_EQ(handler->counters().malformed, 0U);
  const md::LevelBook& book = handler->book();
  EXPECT_EQ(book.quantity_at(lob::Side::Buy, 100), std::optional<lob::Qty>(5));
  EXPECT_EQ(book.quantity_at(lob::Side::Buy, 101), std::optional<lob::Qty>(4));
  EXPECT_EQ(book.quantity_at(lob::Side::Sell, 102), std::optional<lob::Qty>(9));
  EXPECT_EQ(book.quantity_at(lob::Side::Buy, 99), std::optional<lob::Qty>(2));
}

TEST(md_client_feed_handler, RecoveryAnswersWithAResetWhenFramesAreGone) {
  md::testing::FakePublisher publisher;
  auto handler = make_handler(publisher);
  ASSERT_NE(handler, nullptr);

  publisher.store_frame(book_update(100, 5, lob::Side::Buy, 1));
  publisher.store_frame(book_update(101, 3, lob::Side::Sell, 2));
  publisher.store_frame(book_update(99, 2, lob::Side::Buy, 8));
  publisher.set_reset_next(8);

  // 3..7 were dropped and are no longer held, so recovery answers with a
  // reset pointing at the next live frame instead of the missing range.
  publisher.send_frames(handler->udp_port(), {book_update(100, 5, lob::Side::Buy, 1),
                                              book_update(101, 3, lob::Side::Sell, 2),
                                              book_update(99, 2, lob::Side::Buy, 8)});

  ASSERT_TRUE(poll_until(*handler, [&] { return handler->expected_seq() == 9; }));
  const md::FeedCounters& counters = handler->counters();
  EXPECT_EQ(counters.resets, 1U);
  EXPECT_EQ(counters.gaps_detected, 1U);
  EXPECT_EQ(counters.gaps_recovered, 1U);
  EXPECT_GE(counters.recovery_requests, 1U);
  EXPECT_EQ(handler->book().quantity_at(lob::Side::Buy, 99), std::optional<lob::Qty>(2));
}

TEST(md_client_feed_handler, TrailingGapOpenedByAHeartbeatIsRecovered) {
  md::testing::FakePublisher publisher;
  auto handler = make_handler(publisher);
  ASSERT_NE(handler, nullptr);

  const Frames stream{
      book_update(100, 10, lob::Side::Buy, 1),
      book_update(101, 5, lob::Side::Buy, 2),
      book_update(102, 4, lob::Side::Sell, 3),
      book_update(103, 2, lob::Side::Sell, 4),
  };
  publisher.store_frames(stream);

  // The final datagram is lost and no later event ever arrives: only the
  // trailing heartbeat can reveal that sequence 4 was published.
  publisher.send_frames(handler->udp_port(), {stream[0], stream[1], stream[2]});
  ASSERT_TRUE(poll_until(*handler, [&] { return handler->last_seen_seq() == 3; }));
  EXPECT_EQ(handler->highest_known_seq(), 3U);
  EXPECT_EQ(handler->counters().gaps_detected, 0U);

  publisher.send_frames(handler->udp_port(), {md::testing::heartbeat(4)});
  ASSERT_TRUE(poll_until(*handler, [&] { return handler->last_seen_seq() == 4; }));

  const md::FeedCounters& counters = handler->counters();
  EXPECT_EQ(counters.heartbeats, 1U);
  EXPECT_EQ(counters.gaps_detected, 1U);
  EXPECT_EQ(counters.gaps_recovered, 1U);
  EXPECT_GE(counters.recovery_requests, 1U);
  EXPECT_EQ(handler->highest_known_seq(), 4U);
  EXPECT_EQ(handler->last_seen_seq(), 4U);
  EXPECT_EQ(handler->expected_seq(), 5U);
  EXPECT_EQ(handler->buffered(), 0U);
  // The recovered frame was applied, so the local book has the lost update.
  EXPECT_EQ(handler->book().quantity_at(lob::Side::Sell, 103), std::optional<lob::Qty>(2));
}

TEST(md_client_feed_handler, HeartbeatAtTheLastAppliedSequenceIsANoOp) {
  md::testing::FakePublisher publisher;
  auto handler = make_handler(publisher);
  ASSERT_NE(handler, nullptr);

  const Frames stream{
      book_update(100, 10, lob::Side::Buy, 1),
      book_update(101, 5, lob::Side::Buy, 2),
      book_update(102, 4, lob::Side::Sell, 3),
  };
  publisher.store_frames(stream);
  publisher.send_frames(handler->udp_port(), stream);
  ASSERT_TRUE(poll_until(*handler, [&] { return handler->last_seen_seq() == 3; }));

  // The stream is current: heartbeats repeating 3 (and a stale 1) must not
  // open a gap, ask for recovery or change the book.
  publisher.send_frames(handler->udp_port(), {md::testing::heartbeat(3), md::testing::heartbeat(1),
                                              md::testing::heartbeat(3, 0xABCD)});
  ASSERT_TRUE(poll_until(*handler, [&] { return handler->counters().heartbeats == 3; }));

  const md::FeedCounters& counters = handler->counters();
  EXPECT_EQ(counters.gaps_detected, 0U);
  EXPECT_EQ(counters.gaps_recovered, 0U);
  EXPECT_EQ(counters.recovery_requests, 0U);
  EXPECT_EQ(counters.recovery_failures, 0U);
  EXPECT_EQ(handler->buffered(), 0U);
  EXPECT_EQ(handler->last_seen_seq(), 3U);
  EXPECT_EQ(handler->highest_known_seq(), 3U);
  EXPECT_EQ(handler->book().quantity_at(lob::Side::Buy, 100), std::optional<lob::Qty>(10));
  EXPECT_EQ(handler->book().quantity_at(lob::Side::Sell, 102), std::optional<lob::Qty>(4));
}

TEST(md_client_feed_handler, MalformedDatagramIsCountedAndIgnored) {
  md::testing::FakePublisher publisher;
  auto handler = make_handler(publisher, /*auto_recovery=*/false);
  ASSERT_NE(handler, nullptr);

  const std::vector<std::byte> truncated(8, std::byte{0});
  publisher.send_datagram(handler->udp_port(), truncated);
  publisher.send_datagram(
      handler->udp_port(),
      std::vector<std::byte>{std::byte{0xff}, std::byte{0xff}, std::byte{0x00}});

  ASSERT_TRUE(poll_until(*handler, [&] { return handler->counters().datagrams == 2; }));
  EXPECT_EQ(handler->counters().malformed, 2U);
  EXPECT_EQ(handler->counters().received, 0U);
  EXPECT_EQ(handler->expected_seq(), 1U);
  EXPECT_TRUE(handler->book().empty());
}

TEST(md_client_feed_handler, TradeFramesAdvanceTheSequenceWithoutTouchingTheBook) {
  md::testing::FakePublisher publisher;
  auto handler = make_handler(publisher, /*auto_recovery=*/false);
  ASSERT_NE(handler, nullptr);

  publisher.send_frames(handler->udp_port(), {md::testing::trade(11, 12, 100, 3, 1),
                                              book_update(100, 7, lob::Side::Buy, 2)});
  ASSERT_TRUE(poll_until(*handler, [&] { return handler->expected_seq() == 3; }));
  EXPECT_EQ(handler->counters().trades, 1U);
  EXPECT_EQ(handler->book().quantity_at(lob::Side::Buy, 100), std::optional<lob::Qty>(7));
}

}  // namespace
