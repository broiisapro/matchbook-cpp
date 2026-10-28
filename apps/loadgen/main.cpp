// lob_loadgen: N order-entry sessions against a gateway, wire-to-wire latency
// into an HdrHistogram, JSON report. See docs/architecture.md §15 and the
// methodology section of the report.

#include <cstdint>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "net/loadgen/cli.hpp"
#include "net/loadgen/hdr_latency.hpp"
#include "net/loadgen/report.hpp"
#include "net/loadgen/runner.hpp"
#include "net/loadgen/tcp_transport.hpp"

namespace {

namespace loadgen = lob::loadgen;

bool write_file(const std::string& path, const std::string& contents) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) {
    std::cerr << "lob_loadgen: cannot open report file '" << path << "'\n";
    return false;
  }
  out << contents;
  out.close();
  if (!out) {
    std::cerr << "lob_loadgen: cannot write report file '" << path << "'\n";
    return false;
  }
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  loadgen::CliOptions cli;
  std::string error;
  switch (loadgen::parse_cli(argc, argv, cli, error)) {
    case loadgen::CliStatus::Help:
      std::cout << loadgen::usage();
      return 0;
    case loadgen::CliStatus::Error:
      std::cerr << "lob_loadgen: " << error << "\n\n" << loadgen::usage();
      return 2;
    case loadgen::CliStatus::Ok:
      break;
  }

  std::vector<std::unique_ptr<loadgen::OrderEntryTransport>> transports;
  transports.reserve(cli.sessions);
  for (std::uint32_t session = 0; session < cli.sessions; ++session) {
    std::unique_ptr<loadgen::TcpTransport> transport =
        loadgen::TcpTransport::connect(cli.gateway, error);
    if (transport == nullptr) {
      std::cerr << "lob_loadgen: session " << session << ": " << error << '\n';
      return 1;
    }
    transports.push_back(std::move(transport));
  }

  loadgen::HdrLatencySink latency;
  if (!latency.ok()) {
    std::cerr << "lob_loadgen: could not allocate the latency histogram\n";
    return 1;
  }

  loadgen::RunnerOptions options;
  options.orders = cli.orders;
  options.seed = cli.seed;
  options.rate = cli.rate;
  options.response_timeout = cli.response_timeout;
  options.min_price = cli.min_price;
  options.max_price = cli.max_price;

  loadgen::LoadRunner runner(std::move(transports), options, latency);
  loadgen::RunResult result = runner.run();

  loadgen::Report report;
  report.gateway = cli.gateway_text;
  report.sessions = cli.sessions;
  report.seed = cli.seed;
  report.rate_limit = cli.rate;
  report.stats = result.stats;
  report.latency = latency.summary();
  report.latency_clamped_samples = latency.clamped_samples();
  report.error = result.error;

  if (!write_file(cli.report_path, loadgen::to_json(report))) {
    return 1;
  }

  const loadgen::RunStats& stats = result.stats;
  std::cout << "lob_loadgen: gateway=" << cli.gateway_text << " sessions=" << cli.sessions
            << " sent=" << stats.requests_sent << '/' << stats.orders_requested
            << " answered=" << stats.first_responses
            << " unanswered=" << stats.orders_without_response() << " p50=" << report.latency.p50_ns
            << "ns p99=" << report.latency.p99_ns << "ns elapsed=" << stats.elapsed_seconds
            << "s report=" << cli.report_path << '\n';

  if (!result.error.empty()) {
    std::cerr << "lob_loadgen: " << result.error << '\n';
  }
  if (stats.timed_out) {
    std::cerr << "lob_loadgen: run timed out with " << stats.orders_without_response()
              << " unanswered request(s)\n";
  }
  return result.ok() ? 0 : 1;
}
