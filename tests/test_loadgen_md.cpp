// Market-data half of the load generator: the FeedHandler monitor against the
// fake publisher, including recovery of a dropped datagram, plus the md part of
// the report and the CLI flags.

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "net/loadgen/cli.hpp"
#include "net/loadgen/md_monitor.hpp"
#include "net/loadgen/report.hpp"
#include "net/md_client/testing/fake_publisher.hpp"

namespace {

namespace loadgen = lob::loadgen;
namespace md_testing = lob::md::testing;
using namespace std::chrono_literals;

loadgen::MdMonitorOptions test_options(std::uint16_t recovery_port) {
  loadgen::MdMonitorOptions options;
  options.udp_port = 0;
  options.recovery_port = recovery_port;
  options.poll_interval = 5ms;
  options.quiet_period = 20ms;
  options.max_drain = 300ms;
  return options;
}

/// `count` book updates with publisher sequence numbers starting at 1.
std::vector<std::vector<std::byte>> level_updates(int count) {
  std::vector<std::vector<std::byte>> frames;
  frames.reserve(static_cast<std::size_t>(count));
  for (int index = 0; index < count; ++index) {
    frames.push_back(md_testing::book_update(1000 + index, 10, lob::Side::Buy,
                                             static_cast<lob::net::Seq>(index + 1)));
  }
  return frames;
}

TEST(LoadgenMd, CountsAnInOrderStream) {
  md_testing::FakePublisher publisher;
  std::string error;
  const auto monitor = loadgen::MdMonitor::start(test_options(publisher.recovery_port()), error);
  ASSERT_NE(monitor, nullptr) << error;
  EXPECT_NE(monitor->udp_port(), 0);

  publisher.send_frames(monitor->udp_port(), level_updates(100));
  const loadgen::MdSummary summary = monitor->finish();

  EXPECT_EQ(summary.received, 100u);
  EXPECT_EQ(summary.final_seq, 100u);
  EXPECT_EQ(summary.gaps_detected, 0u);
  EXPECT_EQ(summary.gaps_recovered, 0u);
  EXPECT_EQ(summary.resets, 0u);
  EXPECT_EQ(summary.buffered_at_end, 0u);
  EXPECT_EQ(summary.sequence_gaps_unrecovered, 0u);
  EXPECT_EQ(summary.malformed, 0u);
}

TEST(LoadgenMd, RecoversADroppedDatagram) {
  md_testing::FakePublisher publisher;
  const std::vector<std::vector<std::byte>> frames = level_updates(100);
  publisher.store_frames(frames);

  std::string error;
  const auto monitor = loadgen::MdMonitor::start(test_options(publisher.recovery_port()), error);
  ASSERT_NE(monitor, nullptr) << error;

  std::vector<std::vector<std::byte>> live;
  live.reserve(frames.size());
  for (std::size_t index = 0; index < frames.size(); ++index) {
    if (index == 49) {
      continue;  // sequence 50 is lost; the recovery service has it
    }
    live.push_back(frames[index]);
  }
  publisher.send_frames(monitor->udp_port(), live);
  const loadgen::MdSummary summary = monitor->finish();

  EXPECT_EQ(summary.received, 99u);
  EXPECT_EQ(summary.final_seq, 100u);
  EXPECT_EQ(summary.gaps_detected, 1u);
  EXPECT_EQ(summary.gaps_recovered, 1u);
  EXPECT_GE(summary.recovery_requests, 1u);
  EXPECT_EQ(summary.recovery_failures, 0u);
  EXPECT_EQ(summary.buffered_at_end, 0u);
  EXPECT_EQ(summary.sequence_gaps_unrecovered, 0u);
}

TEST(LoadgenMd, ReportsAResetWhenTheStoreCannotServeTheGap) {
  md_testing::FakePublisher publisher;
  const std::vector<std::vector<std::byte>> frames = level_updates(100);
  publisher.store_frames(frames);
  publisher.erase_frame(50);

  std::string error;
  const auto monitor = loadgen::MdMonitor::start(test_options(publisher.recovery_port()), error);
  ASSERT_NE(monitor, nullptr) << error;

  std::vector<std::vector<std::byte>> live;
  for (std::size_t index = 0; index < frames.size(); ++index) {
    if (index == 49) {
      continue;
    }
    live.push_back(frames[index]);
  }
  publisher.send_frames(monitor->udp_port(), live);
  const loadgen::MdSummary summary = monitor->finish();

  EXPECT_EQ(summary.gaps_detected, 1u);
  EXPECT_GE(summary.resets, 1u);
  EXPECT_EQ(summary.final_seq, 100u);
  EXPECT_EQ(summary.buffered_at_end, 0u);
  EXPECT_EQ(summary.sequence_gaps_unrecovered, 0u);
}

TEST(LoadgenMd, CountsTradesAndPackedDatagrams) {
  md_testing::FakePublisher publisher;
  std::string error;
  const auto monitor = loadgen::MdMonitor::start(test_options(publisher.recovery_port()), error);
  ASSERT_NE(monitor, nullptr) << error;

  std::vector<std::vector<std::byte>> frames;
  for (int index = 0; index < 20; ++index) {
    frames.push_back(md_testing::trade(1, 2, 1000, 5, static_cast<lob::net::Seq>(index + 1)));
  }
  publisher.send_packed(monitor->udp_port(), frames, 5);
  const loadgen::MdSummary summary = monitor->finish();

  EXPECT_EQ(summary.received, 20u);
  EXPECT_EQ(summary.trades, 20u);
  EXPECT_EQ(summary.final_seq, 20u);
  EXPECT_EQ(summary.sequence_gaps_unrecovered, 0u);
}

TEST(LoadgenReport, IncludesTheMarketDataCounters) {
  loadgen::Report report;
  report.gateway = "127.0.0.1:9000";
  report.has_md = true;
  report.md.received = 120;
  report.md.duplicates = 1;
  report.md.gaps_detected = 2;
  report.md.gaps_recovered = 2;
  report.md.resets = 0;
  report.md.final_seq = 120;
  report.md.trades = 7;

  const std::string json = loadgen::to_json(report);
  EXPECT_NE(json.find("\"md\": {"), std::string::npos);
  EXPECT_NE(json.find("\"received\": 120"), std::string::npos);
  EXPECT_NE(json.find("\"duplicates\": 1"), std::string::npos);
  EXPECT_NE(json.find("\"gaps_detected\": 2"), std::string::npos);
  EXPECT_NE(json.find("\"gaps_recovered\": 2"), std::string::npos);
  EXPECT_NE(json.find("\"final_seq\": 120"), std::string::npos);
  EXPECT_NE(json.find("\"sequence_gaps_unrecovered\": 0"), std::string::npos);
  EXPECT_NE(json.find("\"trades\": 7"), std::string::npos);
  EXPECT_NE(json.find("\"market_data\""), std::string::npos);
}

TEST(LoadgenReport, OmitsTheMdBlockWhenMdWasNotMonitored) {
  loadgen::Report report;
  report.gateway = "127.0.0.1:9000";
  const std::string json = loadgen::to_json(report);
  EXPECT_NE(json.find("\"md\": null"), std::string::npos);
}

TEST(LoadgenCli, ParsesTheMarketDataFlags) {
  loadgen::CliOptions options;
  std::string error;
  std::vector<std::string> arguments{"--gateway",  "127.0.0.1:9000", "--orders",      "100",
                                     "--report",   "r.json",         "--md-udp-port", "9100",
                                     "--recovery", "127.0.0.1"};
  std::vector<char*> argv;
  argv.push_back(const_cast<char*>("lob_loadgen"));
  for (std::string& argument : arguments) {
    argv.push_back(argument.data());
  }
  ASSERT_EQ(loadgen::parse_cli(static_cast<int>(argv.size()), argv.data(), options, error),
            loadgen::CliStatus::Ok)
      << error;
  EXPECT_EQ(options.md_udp_port, 9100);
  EXPECT_EQ(options.recovery.host, "127.0.0.1");
  EXPECT_EQ(options.recovery.port, loadgen::kDefaultRecoveryPort);
}

TEST(LoadgenCli, RejectsABadMarketDataPort) {
  loadgen::CliOptions options;
  std::string error;
  std::vector<std::string> arguments{"--gateway", "host",   "--orders",      "1",
                                     "--report",  "r.json", "--md-udp-port", "70000"};
  std::vector<char*> argv;
  argv.push_back(const_cast<char*>("lob_loadgen"));
  for (std::string& argument : arguments) {
    argv.push_back(argument.data());
  }
  EXPECT_EQ(loadgen::parse_cli(static_cast<int>(argv.size()), argv.data(), options, error),
            loadgen::CliStatus::Error);
  EXPECT_FALSE(error.empty());
}

}  // namespace
