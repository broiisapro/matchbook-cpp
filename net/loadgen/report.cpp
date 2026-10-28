#include "net/loadgen/report.hpp"

#include <cmath>
#include <iomanip>
#include <sstream>
#include <string_view>

namespace lob::loadgen {
namespace {

/// Minimal JSON string escaping: quotes, backslashes and the control
/// characters JSON forbids raw. Endpoints and error text are the only strings
/// that can carry user input.
void write_string(std::ostream& out, std::string_view text) {
  out << '"';
  for (const char raw : text) {
    const auto byte = static_cast<unsigned char>(raw);
    switch (byte) {
      case '"':
        out << "\\\"";
        break;
      case '\\':
        out << "\\\\";
        break;
      case '\n':
        out << "\\n";
        break;
      case '\r':
        out << "\\r";
        break;
      case '\t':
        out << "\\t";
        break;
      default:
        if (byte < 0x20) {
          out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << byte << std::dec
              << std::setfill(' ');
        } else {
          out << raw;
        }
        break;
    }
  }
  out << '"';
}

void write_double(std::ostream& out, double value) {
  if (!std::isfinite(value)) {
    out << "null";
    return;
  }
  const std::streamsize precision = out.precision();
  out << std::setprecision(9) << value << std::setprecision(precision);
}

void write_latency(std::ostream& out, const LatencySummary& latency) {
  out << "{\"count\": " << latency.count << ", \"p50\": " << latency.p50_ns
      << ", \"p90\": " << latency.p90_ns << ", \"p99\": " << latency.p99_ns
      << ", \"p99_9\": " << latency.p999_ns << ", \"max\": " << latency.max_ns << ", \"mean\": ";
  write_double(out, latency.mean_ns);
  out << '}';
}

void write_counts(std::ostream& out, const ResponseCounts& counts) {
  out << "{\"ack\": " << counts.ack << ", \"reject\": " << counts.reject
      << ", \"fill\": " << counts.fill << ", \"cancelled\": " << counts.cancelled << '}';
}

}  // namespace

std::string to_json(const Report& report) {
  std::ostringstream out;
  out << std::setprecision(9);

  out << "{\n";
  out << "  \"schema_version\": 1,\n";
  out << "  \"gateway\": ";
  write_string(out, report.gateway);
  out << ",\n";
  out << "  \"sessions\": " << report.sessions << ",\n";
  out << "  \"seed\": " << report.seed << ",\n";
  out << "  \"rate_limit_per_second\": ";
  if (report.rate_limit > 0.0) {
    write_double(out, report.rate_limit);
  } else {
    out << "null";
  }
  out << ",\n";

  out << "  \"orders_requested\": " << report.stats.orders_requested << ",\n";
  out << "  \"requests_sent\": " << report.stats.requests_sent << ",\n";
  out << "  \"responses_received\": " << report.stats.responses_received << ",\n";
  out << "  \"first_responses\": " << report.stats.first_responses << ",\n";
  out << "  \"orders_without_response\": " << report.stats.orders_without_response() << ",\n";
  out << "  \"unsolicited_responses\": " << report.stats.unsolicited_responses << ",\n";
  out << "  \"timed_out\": " << (report.stats.timed_out ? "true" : "false") << ",\n";
  out << "  \"send_errors\": " << report.stats.send_errors << ",\n";
  out << "  \"protocol_errors\": " << report.stats.protocol_errors << ",\n";
  out << "  \"complete\": "
      << (report.error.empty() && !report.stats.timed_out &&
                  report.stats.orders_without_response() == 0
              ? "true"
              : "false")
      << ",\n";
  out << "  \"error\": ";
  write_string(out, report.error);
  out << ",\n";

  out << "  \"elapsed_seconds\": ";
  write_double(out, report.stats.elapsed_seconds);
  out << ",\n";
  out << "  \"requests_per_second\": ";
  write_double(out, report.stats.requests_per_second);
  out << ",\n";

  out << "  \"latency_ns\": ";
  write_latency(out, report.latency);
  out << ",\n";
  out << "  \"latency_clamped_samples\": " << report.latency_clamped_samples << ",\n";
  out << "  \"first_response_by_type\": ";
  write_counts(out, report.stats.first_response_by_type);
  out << ",\n";
  out << "  \"responses_by_type\": ";
  write_counts(out, report.stats.responses_by_type);
  out << ",\n";

  out << "  \"methodology\": {\n";
  out << "    \"clock\": "
         "\"std::chrono::steady_clock (monotonic, nanoseconds)\",\n";
  out << "    \"measurement_point\": "
         "\"from the timestamp taken immediately before send() to the first response frame that "
         "references the request's order id, decoded after select() reports the socket "
         "readable\",\n";
  out << "    \"network\": "
         "\"TCP loopback (localhost), one connection per session, TCP_NODELAY enabled\",\n";
  out << "    \"loop\": "
         "\"closed: one request in flight per session; the next request is sent only after its "
         "predecessor's first response\",\n";
  out << "    \"in_flight_per_session\": 1,\n";
  out << "    \"coordinated_omission\": "
         "\"A closed-loop client paces the next send behind the previous response, so a slow "
         "response delays the next request instead of queueing behind it. The percentiles "
         "describe an interactive client's steady state, not an open-loop stream's "
         "worst-case service time, and should not be read as a saturation benchmark.\"\n";
  out << "  }\n";
  out << "}\n";
  return out.str();
}

}  // namespace lob::loadgen
