#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "apps/md_listen/listener.hpp"
#include "net/md_client/testing/fake_publisher.hpp"

namespace {

namespace md = lob::md;
using md::testing::book_update;

using Frames = std::vector<std::vector<std::byte>>;

std::optional<md::ListenerOptions> parse(const std::vector<std::string>& arguments,
                                         std::string& error) {
  std::vector<std::string> storage = arguments;
  std::vector<char*> argv;
  argv.push_back(const_cast<char*>("lob_md_listen"));
  for (std::string& argument : storage) {
    argv.push_back(argument.data());
  }
  return md::parse_listener_options(static_cast<int>(argv.size()), argv.data(), error);
}

TEST(md_listen_options, DefaultsMatchTheDocumentedCommandLine) {
  std::string error;
  const std::optional<md::ListenerOptions> options = parse({}, error);
  ASSERT_TRUE(options.has_value()) << error;
  EXPECT_EQ(options->udp_port, 9100);
  EXPECT_EQ(options->bind_address, "0.0.0.0");
  EXPECT_EQ(options->recovery_host, "127.0.0.1");
  EXPECT_EQ(options->recovery_port, 9002);
  EXPECT_EQ(options->top, 5U);
  EXPECT_EQ(options->seconds, 0);
}

TEST(md_listen_options, ParsesEveryDocumentedFlag) {
  std::string error;
  const std::optional<md::ListenerOptions> options =
      parse({"--udp-port", "9200", "--recovery", "feed.local:9400", "--top", "3", "--seconds", "7",
             "--bind", "127.0.0.1"},
            error);
  ASSERT_TRUE(options.has_value()) << error;
  EXPECT_EQ(options->udp_port, 9200);
  EXPECT_EQ(options->bind_address, "127.0.0.1");
  EXPECT_EQ(options->recovery_host, "feed.local");
  EXPECT_EQ(options->recovery_port, 9400);
  EXPECT_EQ(options->top, 3U);
  EXPECT_EQ(options->seconds, 7);
}

TEST(md_listen_options, RejectsMalformedValues) {
  std::string error;
  EXPECT_FALSE(parse({"--recovery", "feed.local"}, error).has_value());
  EXPECT_FALSE(error.empty());
  EXPECT_FALSE(parse({"--recovery", "feed.local:0"}, error).has_value());
  EXPECT_FALSE(parse({"--recovery", "feed.local:99999"}, error).has_value());
  EXPECT_FALSE(parse({"--recovery", ":9002"}, error).has_value());
  EXPECT_FALSE(parse({"--udp-port", "http"}, error).has_value());
  EXPECT_FALSE(parse({"--top", "0"}, error).has_value());
  EXPECT_FALSE(parse({"--seconds", "-1"}, error).has_value());
  EXPECT_FALSE(parse({"--udp-port"}, error).has_value());
  EXPECT_FALSE(parse({"--nonsense", "1"}, error).has_value());
}

TEST(md_listen, ReportsAnUnbindableAddressWithoutPolling) {
  md::ListenerOptions options;
  options.udp_port = 0;
  options.bind_address = "203.0.113.1";  // TEST-NET-3, never a local address
  options.poll_timeout_ms = 5;

  std::ostringstream out;
  std::ostringstream err;
  const int rc = md::run_listener(options, out, err);

  EXPECT_EQ(rc, 1);
  EXPECT_NE(err.str().find("lob_md_listen:"), std::string::npos) << err.str();
  EXPECT_TRUE(out.str().empty());
}

TEST(md_listen, PrintsTopOfBookAndTheSummaryLine) {
  md::testing::FakePublisher publisher;
  const Frames frames{
      book_update(100, 9, lob::Side::Buy, 1),  book_update(99, 7, lob::Side::Buy, 2),
      book_update(101, 6, lob::Side::Sell, 3), book_update(102, 3, lob::Side::Sell, 4),
      book_update(100, 11, lob::Side::Buy, 5),
  };
  publisher.store_frames(frames);

  md::ListenerOptions options;
  options.udp_port = 0;
  options.bind_address = "127.0.0.1";
  options.recovery_port = publisher.recovery_port();
  options.top = 2;
  options.poll_timeout_ms = 5;
  options.stop_after_messages = frames.size();

  std::ostringstream out;
  std::ostringstream err;
  const int rc = md::run_listener(
      options, out, err, [&](std::uint16_t port) { publisher.send_packed(port, frames, 5); });

  ASSERT_EQ(rc, 0);
  EXPECT_TRUE(err.str().empty()) << err.str();
  const std::string text = out.str();
  EXPECT_NE(text.find("listening on udp://127.0.0.1:"), std::string::npos) << text;
  EXPECT_NE(text.find("book bid: 100 x 11, 99 x 7 | ask: 101 x 6, 102 x 3"), std::string::npos)
      << text;
  EXPECT_NE(text.find("counters received=5"), std::string::npos) << text;
  EXPECT_NE(text.find("gaps: 0 recovered: 0 resets: 0"), std::string::npos) << text;
}

TEST(md_listen, ReportsAGapThatWasRecovered) {
  md::testing::FakePublisher publisher;
  const Frames stream{
      book_update(100, 5, lob::Side::Buy, 1),
      book_update(101, 4, lob::Side::Buy, 2),
      book_update(102, 3, lob::Side::Sell, 3),
      book_update(103, 2, lob::Side::Sell, 4),
  };
  publisher.store_frames(stream);

  md::ListenerOptions options;
  options.udp_port = 0;
  options.bind_address = "127.0.0.1";
  options.recovery_port = publisher.recovery_port();
  options.top = 2;
  options.poll_timeout_ms = 5;
  // Frame 2 is dropped on the wire; recovery supplies 2 and 3, so the live
  // feed still delivers three frames and the local book is complete.
  options.stop_after_messages = 3;

  std::ostringstream out;
  std::ostringstream err;
  const int rc = md::run_listener(options, out, err, [&](std::uint16_t port) {
    publisher.send_frames(port, {stream[0], stream[2], stream[3]});
  });

  ASSERT_EQ(rc, 0);
  const std::string text = out.str();
  EXPECT_NE(text.find("book bid: 101 x 4, 100 x 5 | ask: 102 x 3, 103 x 2"), std::string::npos)
      << text;
  EXPECT_NE(text.find("gaps: 1 recovered: 1 resets: 0"), std::string::npos) << text;
}

}  // namespace
