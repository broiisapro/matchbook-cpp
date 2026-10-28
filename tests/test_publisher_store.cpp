#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "net/protocol/codec.hpp"
#include "net/protocol/messages.hpp"
#include "net/publisher/datagram_packer.hpp"
#include "net/publisher/endpoint.hpp"
#include "net/publisher/options.hpp"
#include "net/publisher/retransmit_store.hpp"

namespace {

namespace pub = lob::publisher;
namespace proto = lob::net;

std::vector<std::byte> make_frame(proto::Seq seq, lob::Price price, lob::Qty qty) {
  const proto::BookUpdate message{price, qty, lob::Side::Sell};
  std::array<std::byte, proto::BookUpdate::kWireSize> buffer{};
  const std::size_t written = proto::encode(message, seq, buffer);
  EXPECT_EQ(written, buffer.size());
  return std::vector<std::byte>(buffer.begin(), buffer.end());
}

bool parse_options(std::vector<std::string> arguments, pub::PublisherOptions& options,
                   std::string& error) {
  std::vector<char*> argv;
  argv.reserve(arguments.size() + 1);
  argv.push_back(const_cast<char*>("lob_publisher"));
  for (std::string& argument : arguments) {
    argv.push_back(argument.data());
  }
  return pub::parse_publisher_options(static_cast<int>(argv.size()), argv.data(), options, error);
}

// ---------------------------------------------------------------- endpoint

TEST(PublisherEndpoint, ParsesHostAndPort) {
  pub::Endpoint endpoint;
  ASSERT_TRUE(pub::parse_endpoint("127.0.0.1:9001", endpoint));
  EXPECT_EQ(endpoint.host, "127.0.0.1");
  EXPECT_EQ(endpoint.port, 9001);

  ASSERT_TRUE(pub::parse_endpoint("localhost:1", endpoint));
  EXPECT_EQ(endpoint.host, "localhost");
  EXPECT_EQ(endpoint.port, 1);
}

TEST(PublisherEndpoint, ParsesBracketedIpv6) {
  pub::Endpoint endpoint;
  ASSERT_TRUE(pub::parse_endpoint("[::1]:65535", endpoint));
  EXPECT_EQ(endpoint.host, "::1");
  EXPECT_EQ(endpoint.port, 65535);
  EXPECT_EQ(pub::to_string(endpoint), "[::1]:65535");
}

TEST(PublisherEndpoint, RejectsMalformedText) {
  pub::Endpoint endpoint{"unchanged", 7};
  EXPECT_FALSE(pub::parse_endpoint("", endpoint));
  EXPECT_FALSE(pub::parse_endpoint("localhost", endpoint));
  EXPECT_FALSE(pub::parse_endpoint(":9001", endpoint));
  EXPECT_FALSE(pub::parse_endpoint("localhost:", endpoint));
  EXPECT_FALSE(pub::parse_endpoint("localhost:0", endpoint));
  EXPECT_FALSE(pub::parse_endpoint("localhost:65536", endpoint));
  EXPECT_FALSE(pub::parse_endpoint("localhost:12x", endpoint));
  EXPECT_FALSE(pub::parse_endpoint("[::1]9001", endpoint));
  EXPECT_EQ(endpoint.host, "unchanged");
  EXPECT_EQ(endpoint.port, 7);
}

TEST(PublisherEndpoint, RoundTrips) {
  const pub::Endpoint endpoint{"192.0.2.10", 5000};
  pub::Endpoint parsed;
  ASSERT_TRUE(pub::parse_endpoint(pub::to_string(endpoint), parsed));
  EXPECT_EQ(parsed, endpoint);
}

// ------------------------------------------------------ retransmit store

TEST(RetransmitStore, KeepsFramesInInsertionOrder) {
  pub::RetransmitStore store(8);
  std::vector<std::vector<std::byte>> frames;
  for (proto::Seq seq = 1; seq <= 5; ++seq) {
    frames.push_back(make_frame(seq, 100 + static_cast<lob::Price>(seq), seq));
    store.append(seq, frames.back());
  }

  EXPECT_EQ(store.capacity(), 8U);
  EXPECT_EQ(store.size(), 5U);
  EXPECT_EQ(store.oldest(), 1U);
  EXPECT_EQ(store.newest(), 5U);
  for (proto::Seq seq = 1; seq <= 5; ++seq) {
    ASSERT_TRUE(store.holds(seq));
    const auto stored = store.frame(seq);
    ASSERT_TRUE(stored.has_value());
    EXPECT_EQ(std::vector<std::byte>(stored->begin(), stored->end()), frames[seq - 1]);
  }
  EXPECT_FALSE(store.holds(0));
  EXPECT_FALSE(store.holds(6));
}

TEST(RetransmitStore, WrapsAndForgetsTheOldestFrames) {
  pub::RetransmitStore store(4);
  for (proto::Seq seq = 1; seq <= 6; ++seq) {
    const std::vector<std::byte> frame = make_frame(seq, 100, 1);
    store.append(seq, frame);
  }

  EXPECT_EQ(store.size(), 4U);
  EXPECT_EQ(store.oldest(), 3U);
  EXPECT_EQ(store.newest(), 6U);
  EXPECT_FALSE(store.holds(2));
  for (proto::Seq seq = 3; seq <= 6; ++seq) {
    EXPECT_TRUE(store.holds(seq)) << seq;
  }
}

TEST(RetransmitStore, IgnoresDuplicates) {
  pub::RetransmitStore store(4);
  const std::vector<std::byte> frame = make_frame(5, 100, 1);
  store.append(5, frame);
  const std::vector<std::byte> duplicate = make_frame(5, 999, 999);
  store.append(5, duplicate);
  EXPECT_EQ(store.size(), 1U);
  EXPECT_EQ(store.oldest(), 5U);
  const auto stored = store.frame(5);
  ASSERT_TRUE(stored.has_value());
  EXPECT_EQ(std::vector<std::byte>(stored->begin(), stored->end()), frame);
}

TEST(RetransmitStore, StartsANewEpochWhenTheStreamRestarts) {
  pub::RetransmitStore store(4);
  for (proto::Seq seq = 1; seq <= 6; ++seq) {
    const std::vector<std::byte> frame = make_frame(seq, 100, 1);
    store.append(seq, frame);
  }
  store.append(2, make_frame(2, 200, 2));
  EXPECT_EQ(store.size(), 1U);
  EXPECT_EQ(store.oldest(), 2U);
  EXPECT_EQ(store.newest(), 2U);
  EXPECT_FALSE(store.holds(3));
}

TEST(RetransmitStore, PlansAResetForAnEmptyStore) {
  const pub::RetransmitStore store(4);
  const pub::RecoveryPlan plan = store.plan(1, 10);
  EXPECT_EQ(plan.kind, pub::RecoveryPlan::Kind::Reset);
  EXPECT_EQ(plan.next_seq, 1U);
}

TEST(RetransmitStore, PlansAResetWhenTheStartIsGone) {
  pub::RetransmitStore store(4);
  for (proto::Seq seq = 1; seq <= 6; ++seq) {
    const std::vector<std::byte> frame = make_frame(seq, 100, 1);
    store.append(seq, frame);
  }
  const pub::RecoveryPlan plan = store.plan(1, 6);
  EXPECT_EQ(plan.kind, pub::RecoveryPlan::Kind::Reset);
  EXPECT_EQ(plan.next_seq, 3U);
}

TEST(RetransmitStore, ClampsTheEndToTheNewestHeldFrame) {
  pub::RetransmitStore store(4);
  for (proto::Seq seq = 1; seq <= 6; ++seq) {
    const std::vector<std::byte> frame = make_frame(seq, 100, 1);
    store.append(seq, frame);
  }
  const pub::RecoveryPlan plan = store.plan(4, 100);
  EXPECT_EQ(plan.kind, pub::RecoveryPlan::Kind::Replay);
  EXPECT_EQ(plan.from, 4U);
  EXPECT_EQ(plan.to, 6U);
}

TEST(RetransmitStore, PlansNothingForAFutureRangeOrAReversedOne) {
  pub::RetransmitStore store(4);
  for (proto::Seq seq = 1; seq <= 3; ++seq) {
    const std::vector<std::byte> frame = make_frame(seq, 100, 1);
    store.append(seq, frame);
  }
  EXPECT_EQ(store.plan(10, 12).kind, pub::RecoveryPlan::Kind::Nothing);
  EXPECT_EQ(store.plan(3, 2).kind, pub::RecoveryPlan::Kind::Nothing);
}

// ------------------------------------------------------ datagram packing

TEST(DatagramPacker, PacksWholeFramesUpToTheLimit) {
  pub::DatagramPacker packer(1200);
  std::vector<std::byte> frame(proto::kMaxWireSize, std::byte{0x42});

  std::size_t frames = 0;
  while (packer.add(frame)) {
    ++frames;
  }
  EXPECT_EQ(frames, 1200 / proto::kMaxWireSize);  // 21 frames, 1176 bytes
  EXPECT_EQ(packer.size(), frames * proto::kMaxWireSize);
  EXPECT_FALSE(packer.add(frame));
  EXPECT_EQ(packer.size(), frames * proto::kMaxWireSize);  // a refused frame changes nothing

  packer.clear();
  EXPECT_TRUE(packer.empty());
  EXPECT_TRUE(packer.add(frame));
}

TEST(DatagramPacker, RefusesAFrameLargerThanTheDatagram) {
  pub::DatagramPacker packer(1200);
  const std::vector<std::byte> oversized(1201, std::byte{0});
  EXPECT_FALSE(packer.add(oversized));
  EXPECT_TRUE(packer.empty());

  const std::vector<std::byte> exact(1200, std::byte{0});
  EXPECT_TRUE(packer.add(exact));
  EXPECT_EQ(packer.size(), 1200U);
}

TEST(DatagramPacker, RejectsImpossibleSizes) {
  EXPECT_THROW(pub::DatagramPacker{proto::kMaxWireSize - 1}, std::invalid_argument);
  EXPECT_THROW(pub::DatagramPacker{65508}, std::invalid_argument);
  EXPECT_NO_THROW(pub::DatagramPacker{proto::kMaxWireSize});
  EXPECT_NO_THROW(pub::DatagramPacker{65507});
}

// ------------------------------------------------------------- options

TEST(PublisherOptions, ParsesTheMinimumCommandLine) {
  pub::PublisherOptions options;
  std::string error;
  ASSERT_TRUE(parse_options({"--udp-dest", "127.0.0.1:5000"}, options, error)) << error;
  EXPECT_FALSE(options.help);
  ASSERT_EQ(options.config.udp_destinations.size(), 1U);
  EXPECT_EQ(options.config.udp_destinations[0], (pub::Endpoint{"127.0.0.1", 5000}));
  EXPECT_EQ(options.config.gateway, (pub::Endpoint{"127.0.0.1", 9001}));
  EXPECT_EQ(options.config.recovery_port, 9002);
  EXPECT_EQ(options.config.retransmit_capacity, 65536U);
  EXPECT_FALSE(options.config.multicast_group.has_value());
}

TEST(PublisherOptions, ParsesEveryOption) {
  pub::PublisherOptions options;
  std::string error;
  ASSERT_TRUE(parse_options(
      {"--gateway", "10.0.0.1:9001", "--udp-dest", "10.0.0.2:6000", "--udp-dest", "10.0.0.3:6001",
       "--mcast-group", "239.0.0.1:7000", "--recovery-port", "9100", "--retransmit", "1024"},
      options, error))
      << error;
  EXPECT_EQ(options.config.gateway, (pub::Endpoint{"10.0.0.1", 9001}));
  ASSERT_EQ(options.config.udp_destinations.size(), 2U);
  EXPECT_EQ(options.config.udp_destinations[1], (pub::Endpoint{"10.0.0.3", 6001}));
  ASSERT_TRUE(options.config.multicast_group.has_value());
  EXPECT_EQ(*options.config.multicast_group, (pub::Endpoint{"239.0.0.1", 7000}));
  EXPECT_EQ(options.config.recovery_port, 9100);
  EXPECT_EQ(options.config.retransmit_capacity, 1024U);
}

TEST(PublisherOptions, ParsesHelp) {
  pub::PublisherOptions options;
  std::string error;
  ASSERT_TRUE(parse_options({"--help"}, options, error)) << error;
  EXPECT_TRUE(options.help);
}

TEST(PublisherOptions, RejectsBadCommandLines) {
  pub::PublisherOptions options;
  std::string error;

  EXPECT_FALSE(parse_options({}, options, error));
  EXPECT_NE(error.find("--udp-dest"), std::string::npos) << error;

  EXPECT_FALSE(parse_options({"--udp-dest"}, options, error));
  EXPECT_NE(error.find("requires a value"), std::string::npos) << error;

  EXPECT_FALSE(
      parse_options({"--gateway", "localhost", "--udp-dest", "127.0.0.1:1"}, options, error));
  EXPECT_NE(error.find("--gateway"), std::string::npos) << error;

  EXPECT_FALSE(
      parse_options({"--recovery-port", "65536", "--udp-dest", "127.0.0.1:1"}, options, error));
  EXPECT_NE(error.find("--recovery-port"), std::string::npos) << error;

  EXPECT_FALSE(parse_options({"--retransmit", "0", "--udp-dest", "127.0.0.1:1"}, options, error));
  EXPECT_NE(error.find("--retransmit"), std::string::npos) << error;

  EXPECT_FALSE(
      parse_options({"--retransmit", "1048577", "--udp-dest", "127.0.0.1:1"}, options, error));
  EXPECT_NE(error.find("--retransmit"), std::string::npos) << error;

  EXPECT_FALSE(parse_options({"--bogus", "value", "--udp-dest", "127.0.0.1:1"}, options, error));
  EXPECT_NE(error.find("unknown option"), std::string::npos) << error;

  EXPECT_FALSE(parse_options({"stray", "--udp-dest", "127.0.0.1:1"}, options, error));
  EXPECT_NE(error.find("unknown option"), std::string::npos) << error;
}

TEST(PublisherOptions, AllowsAnEphemeralRecoveryPort) {
  pub::PublisherOptions options;
  std::string error;
  ASSERT_TRUE(parse_options({"--recovery-port", "0", "--udp-dest", "127.0.0.1:1"}, options, error))
      << error;
  EXPECT_EQ(options.config.recovery_port, 0);
}

}  // namespace
