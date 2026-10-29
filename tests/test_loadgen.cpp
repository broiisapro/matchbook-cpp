// Unit tests for the load generator's pure parts: endpoint and CLI parsing,
// rate pacing and the seeded order flow. The runner and the report are covered
// in test_loadgen_runner.cpp.

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "net/loadgen/cli.hpp"
#include "net/loadgen/endpoint.hpp"
#include "net/loadgen/order_flow.hpp"
#include "net/loadgen/rate_limiter.hpp"

namespace {

namespace loadgen = lob::loadgen;
using namespace std::chrono_literals;

loadgen::CliStatus parse(std::vector<std::string> arguments, loadgen::CliOptions& options,
                         std::string& error) {
  std::vector<char*> argv;
  argv.push_back(const_cast<char*>("lob_loadgen"));
  for (std::string& argument : arguments) {
    argv.push_back(argument.data());
  }
  return loadgen::parse_cli(static_cast<int>(argv.size()), argv.data(), options, error);
}

TEST(LoadgenEndpoint, ParsesHostAndPort) {
  loadgen::Endpoint endpoint;
  std::string error;
  ASSERT_TRUE(loadgen::parse_endpoint("127.0.0.1:9001", endpoint, error)) << error;
  EXPECT_EQ(endpoint.host, "127.0.0.1");
  EXPECT_EQ(endpoint.port, 9001);
}

TEST(LoadgenEndpoint, BareHostUsesTheGatewayDefaultPort) {
  loadgen::Endpoint endpoint;
  std::string error;
  ASSERT_TRUE(loadgen::parse_endpoint("gateway", endpoint, error)) << error;
  EXPECT_EQ(endpoint.host, "gateway");
  EXPECT_EQ(endpoint.port, loadgen::kDefaultGatewayPort);
}

TEST(LoadgenEndpoint, ParsesBracketedIpv6) {
  loadgen::Endpoint endpoint;
  std::string error;
  ASSERT_TRUE(loadgen::parse_endpoint("[::1]:9100", endpoint, error)) << error;
  EXPECT_EQ(endpoint.host, "::1");
  EXPECT_EQ(endpoint.port, 9100);

  ASSERT_TRUE(loadgen::parse_endpoint("[::1]", endpoint, error)) << error;
  EXPECT_EQ(endpoint.host, "::1");
  EXPECT_EQ(endpoint.port, loadgen::kDefaultGatewayPort);
}

TEST(LoadgenEndpoint, RejectsBadPortsAndHosts) {
  loadgen::Endpoint endpoint;
  std::string error;
  EXPECT_FALSE(loadgen::parse_endpoint(":9000", endpoint, error));
  EXPECT_FALSE(loadgen::parse_endpoint("host:", endpoint, error));
  EXPECT_FALSE(loadgen::parse_endpoint("host:0", endpoint, error));
  EXPECT_FALSE(loadgen::parse_endpoint("host:65536", endpoint, error));
  EXPECT_FALSE(loadgen::parse_endpoint("host:http", endpoint, error));
  EXPECT_FALSE(loadgen::parse_endpoint("host:12x", endpoint, error));
  EXPECT_FALSE(loadgen::parse_endpoint("[::1", endpoint, error));
  EXPECT_FALSE(loadgen::parse_endpoint("[::1]x", endpoint, error));
  EXPECT_FALSE(loadgen::parse_endpoint("", endpoint, error));
}

TEST(LoadgenCli, ParsesTheDocumentedInvocation) {
  loadgen::CliOptions options;
  std::string error;
  ASSERT_EQ(parse({"--gateway", "127.0.0.1:9000", "--sessions", "4", "--orders", "1000", "--rate",
                   "2500", "--seed", "7", "--report", "/tmp/out.json"},
                  options, error),
            loadgen::CliStatus::Ok)
      << error;
  EXPECT_EQ(options.gateway.host, "127.0.0.1");
  EXPECT_EQ(options.gateway.port, 9000);
  EXPECT_EQ(options.sessions, 4u);
  EXPECT_EQ(options.orders, 1000u);
  EXPECT_DOUBLE_EQ(options.rate, 2500.0);
  EXPECT_EQ(options.seed, 7u);
  EXPECT_EQ(options.report_path, "/tmp/out.json");
  EXPECT_EQ(options.response_timeout, 5000ms);
  EXPECT_EQ(options.min_price, 1);
  EXPECT_EQ(options.max_price, 200000);
}

TEST(LoadgenCli, AppliesDefaults) {
  loadgen::CliOptions options;
  std::string error;
  ASSERT_EQ(
      parse({"--gateway", "localhost", "--orders", "1", "--report", "r.json"}, options, error),
      loadgen::CliStatus::Ok)
      << error;
  EXPECT_EQ(options.sessions, 1u);
  EXPECT_DOUBLE_EQ(options.rate, 0.0);
  EXPECT_EQ(options.seed, 1u);
  EXPECT_EQ(options.gateway.port, loadgen::kDefaultGatewayPort);
}

TEST(LoadgenCli, HelpIsNotAnError) {
  loadgen::CliOptions options;
  std::string error;
  EXPECT_EQ(parse({"--help"}, options, error), loadgen::CliStatus::Help);
  EXPECT_TRUE(options.help);
  EXPECT_EQ(parse({"-h"}, options, error), loadgen::CliStatus::Help);
}

TEST(LoadgenCli, RejectsMissingAndInvalidOptions) {
  loadgen::CliOptions options;
  std::string error;

  EXPECT_EQ(parse({"--orders", "10", "--report", "r.json"}, options, error),
            loadgen::CliStatus::Error);
  EXPECT_FALSE(error.empty());

  EXPECT_EQ(parse({"--gateway", "host", "--report", "r.json"}, options, error),
            loadgen::CliStatus::Error);
  EXPECT_NE(error.find("--orders"), std::string::npos);

  EXPECT_EQ(parse({"--gateway", "host", "--orders", "10"}, options, error),
            loadgen::CliStatus::Error);
  EXPECT_NE(error.find("--report"), std::string::npos);

  EXPECT_EQ(parse({"--gateway", "host", "--orders", "0", "--report", "r.json"}, options, error),
            loadgen::CliStatus::Error);
  EXPECT_EQ(parse({"--gateway", "host", "--orders", "10", "--report", "r.json", "--sessions", "0"},
                  options, error),
            loadgen::CliStatus::Error);
  EXPECT_EQ(parse({"--gateway", "host", "--orders", "10", "--report", "r.json", "--rate", "-1"},
                  options, error),
            loadgen::CliStatus::Error);
  EXPECT_EQ(parse({"--gateway", "host", "--orders", "10", "--report", "r.json", "--seed", "x"},
                  options, error),
            loadgen::CliStatus::Error);
  EXPECT_EQ(parse({"--gateway", "host", "--orders", "10", "--report", "r.json", "--nope"}, options,
                  error),
            loadgen::CliStatus::Error);
  EXPECT_EQ(parse({"--gateway", "host", "--orders", "10", "--report", "r.json", "--min-price",
                   "100", "--max-price", "10"},
                  options, error),
            loadgen::CliStatus::Error);
  EXPECT_EQ(
      parse({"--gateway", "host", "--orders", "10", "--report", "r.json", "--timeout-ms", "0"},
            options, error),
      loadgen::CliStatus::Error);

  EXPECT_EQ(parse({"--gateway"}, options, error), loadgen::CliStatus::Error);
  EXPECT_FALSE(error.empty());
}

TEST(LoadgenRateLimiter, UnlimitedNeverWaits) {
  loadgen::RateLimiter limiter(0.0);
  EXPECT_TRUE(limiter.unlimited());
  EXPECT_EQ(limiter.rate(), 0.0);
  EXPECT_EQ(limiter.due_after(0), 0ns);
  EXPECT_EQ(limiter.due_after(123456), 0ns);
}

TEST(LoadgenRateLimiter, SpacesRequestsEvenly) {
  loadgen::RateLimiter limiter(200.0);
  EXPECT_FALSE(limiter.unlimited());
  EXPECT_EQ(limiter.due_after(0), 0ns);
  EXPECT_EQ(limiter.due_after(1), 5ms);
  EXPECT_EQ(limiter.due_after(3), 15ms);
  EXPECT_EQ(limiter.due_after(201), 1005ms);

  loadgen::RateLimiter slow(0.5);
  EXPECT_EQ(slow.due_after(2), 4s);

  loadgen::RateLimiter fast(1'000'000.0);
  EXPECT_EQ(fast.due_after(1000), 1ms);
}

TEST(LoadgenOrderFlow, SameSeedProducesTheSameStream) {
  loadgen::OrderFlow first(7, 1, 200000);
  loadgen::OrderFlow second(7, 1, 200000);
  for (int index = 0; index < 5000; ++index) {
    ASSERT_EQ(first.next(), second.next()) << "request " << index;
  }
}

TEST(LoadgenOrderFlow, DifferentSeedsProduceDifferentStreams) {
  loadgen::OrderFlow first(7, 1, 200000);
  loadgen::OrderFlow second(8, 1, 200000);
  int differences = 0;
  for (int index = 0; index < 500; ++index) {
    if (!(first.next() == second.next())) {
      ++differences;
    }
  }
  EXPECT_GT(differences, 100);
}

TEST(LoadgenOrderFlow, RequestsAreWellFormedAndTargetLiveOrders) {
  constexpr lob::Price kMin = 100;
  constexpr lob::Price kMax = 1000;
  loadgen::OrderFlow flow(11, kMin, kMax);

  std::unordered_set<lob::OrderId> submitted;
  std::unordered_map<lob::OrderId, loadgen::Request> live;
  std::uint64_t submits = 0;
  std::uint64_t cancels = 0;
  std::uint64_t modifies = 0;
  std::uint64_t markets = 0;
  std::uint64_t iocs = 0;

  for (int index = 0; index < 20000; ++index) {
    const loadgen::Request request = flow.next();
    if (request.kind == lob::net::MessageType::NewOrder) {
      ++submits;
      EXPECT_GT(request.order_id, 0u);
      EXPECT_LE(request.order_id, loadgen::kMaxClientOrderId);
      EXPECT_TRUE(submitted.insert(request.order_id).second) << "duplicate client id";
      EXPECT_GT(request.qty, 0);
      if (request.order_type == lob::OrderType::Market) {
        ++markets;
        EXPECT_EQ(request.price, 0);
      } else {
        EXPECT_GE(request.price, kMin);
        EXPECT_LE(request.price, kMax);
      }
      if (request.order_type == lob::OrderType::IOC) {
        ++iocs;
      }
      if (request.order_type == lob::OrderType::Limit) {
        live.emplace(request.order_id, request);
      }
    } else {
      // The flow only cancels or modifies orders it believes are resting.
      ASSERT_FALSE(live.empty());
      const auto found = live.find(request.order_id);
      ASSERT_NE(found, live.end()) << "cancel/modify of a non-live id";
      EXPECT_LE(request.order_id, loadgen::kMaxClientOrderId);
      if (request.kind == lob::net::MessageType::Cancel) {
        ++cancels;
        live.erase(found);
      } else {
        ++modifies;
        ASSERT_EQ(request.kind, lob::net::MessageType::Modify);
        EXPECT_GT(request.qty, 0);
        EXPECT_GE(request.price, kMin);
        EXPECT_LE(request.price, kMax);
        found->second = request;
      }
    }
  }

  EXPECT_GT(submits, 10000u);
  EXPECT_GT(cancels, 2000u);
  EXPECT_GT(modifies, 800u);
  EXPECT_GT(markets, 0u);
  EXPECT_GT(iocs, 0u);
  EXPECT_EQ(flow.generated(), 20000u);
  EXPECT_GT(flow.live_orders(), 0u);
}

TEST(LoadgenOrderFlow, FirstRequestIsAlwaysASubmission) {
  for (std::uint64_t seed = 0; seed < 50; ++seed) {
    loadgen::OrderFlow flow(seed, 1, 200000);
    EXPECT_EQ(flow.next().kind, lob::net::MessageType::NewOrder) << "seed " << seed;
  }
}

TEST(LoadgenOrderFlow, ClampsPricesToASingletonRange) {
  loadgen::OrderFlow flow(3, 500, 500);
  for (int index = 0; index < 1000; ++index) {
    const loadgen::Request request = flow.next();
    if (request.kind == lob::net::MessageType::NewOrder &&
        request.order_type != lob::OrderType::Market) {
      EXPECT_EQ(request.price, 500);
    }
  }
}

}  // namespace
