#include "net/loadgen/runner.hpp"

#include <sys/select.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <span>

#include "net/protocol/codec.hpp"

namespace lob::loadgen {
namespace {

using Clock = std::chrono::steady_clock;

/// Distinct but well-spread seeds for the per-session flows: two sessions must
/// not send the same stream, or the two flows would hit id collisions and
/// identical prices at exactly the same moment.
constexpr std::uint64_t kSeedStride = 0x9e3779b97f4a7c15ULL;

/// Upper bound on one wait when there is no rate-imposed deadline. select()
/// wakes on readable data long before this on a real connection.
constexpr std::chrono::milliseconds kMaxWait{50};

/// Poll interval for an in-process transport (no socket to wait on) while a
/// request is outstanding. The fake answers synchronously, so this only bounds
/// the wait of a test that deliberately drops a response.
constexpr std::chrono::microseconds kFakePollInterval{200};

std::size_t encode_request(const Request& request, lob::net::Seq seq, std::span<std::byte> out) {
  switch (request.kind) {
    case lob::net::MessageType::NewOrder:
      return lob::net::encode(lob::net::NewOrder{request.order_id, request.price, request.qty,
                                                 request.side, request.order_type},
                              seq, out);
    case lob::net::MessageType::Cancel:
      return lob::net::encode(lob::net::Cancel{request.order_id}, seq, out);
    case lob::net::MessageType::Modify:
      return lob::net::encode(lob::net::Modify{request.order_id, request.price, request.qty}, seq,
                              out);
    default:
      return 0;
  }
}

}  // namespace

struct LoadRunner::SessionState {
  SessionState(std::unique_ptr<OrderEntryTransport> transport_in, std::uint64_t seed,
               lob::Price min_price, lob::Price max_price)
      : transport(std::move(transport_in)), flow(seed, min_price, max_price) {}

  std::unique_ptr<OrderEntryTransport> transport;
  OrderFlow flow;
  lob::net::Seq next_seq = 1;
  std::optional<PendingRequest> pending;
  /// High bits of a gateway order id (session_id << 40), learned from the first
  /// response. Responses then match exactly instead of by the low 40 bits
  /// alone, so a Fill addressed to another session cannot be mistaken for the
  /// answer to our request.
  std::optional<lob::OrderId> id_prefix;
};

class LoadRunner::ResponseSink final : public lob::net::FrameSink {
 public:
  ResponseSink(LoadRunner& runner, SessionState& session) : runner_(runner), session_(session) {}

  bool on_frame(std::span<const std::byte> frame) override {
    runner_.handle_frame(session_, frame);
    return true;
  }

 private:
  LoadRunner& runner_;
  SessionState& session_;
};

bool ResponseCounts::add(lob::net::MessageType type) {
  switch (type) {
    case lob::net::MessageType::Ack:
      ++ack;
      return true;
    case lob::net::MessageType::Reject:
      ++reject;
      return true;
    case lob::net::MessageType::Fill:
      ++fill;
      return true;
    case lob::net::MessageType::Cancelled:
      ++cancelled;
      return true;
    default:
      return false;
  }
}

LoadRunner::LoadRunner(std::vector<std::unique_ptr<OrderEntryTransport>> transports,
                       RunnerOptions options, LatencySink& latency)
    : options_(options), latency_(latency) {
  sessions_.reserve(transports.size());
  for (std::size_t index = 0; index < transports.size(); ++index) {
    const std::uint64_t seed = options_.seed + (index + 1) * kSeedStride;
    sessions_.push_back(std::make_unique<SessionState>(std::move(transports[index]), seed,
                                                       options_.min_price, options_.max_price));
  }
}

LoadRunner::~LoadRunner() = default;

void LoadRunner::handle_frame(SessionState& session, std::span<const std::byte> frame) {
  lob::net::Header header;
  if (lob::net::decode_header(frame, header) != lob::net::DecodeStatus::Ok) {
    ++stats_.protocol_errors;
    return;
  }

  lob::net::Seq seq = 0;
  lob::OrderId referenced = 0;
  switch (header.type) {
    case lob::net::MessageType::Ack: {
      lob::net::Ack message;
      if (lob::net::decode(frame, message, seq) != lob::net::DecodeStatus::Ok) {
        ++stats_.protocol_errors;
        return;
      }
      referenced = message.order_id;
      break;
    }
    case lob::net::MessageType::Reject: {
      lob::net::Reject message;
      if (lob::net::decode(frame, message, seq) != lob::net::DecodeStatus::Ok) {
        ++stats_.protocol_errors;
        return;
      }
      referenced = message.order_id;
      break;
    }
    case lob::net::MessageType::Fill: {
      lob::net::Fill message;
      if (lob::net::decode(frame, message, seq) != lob::net::DecodeStatus::Ok) {
        ++stats_.protocol_errors;
        return;
      }
      // A fill answers the taker's order. When we are the maker the taker is
      // another session and the id cannot match ours.
      referenced = message.taker_id;
      break;
    }
    case lob::net::MessageType::Cancelled: {
      lob::net::Cancelled message;
      if (lob::net::decode(frame, message, seq) != lob::net::DecodeStatus::Ok) {
        ++stats_.protocol_errors;
        return;
      }
      referenced = message.order_id;
      break;
    }
    default:
      ++stats_.protocol_errors;
      return;
  }

  ++stats_.responses_received;
  stats_.responses_by_type.add(header.type);

  if (!session.pending.has_value()) {
    ++stats_.unsolicited_responses;
    return;
  }

  const PendingRequest& pending = *session.pending;
  bool matches = false;
  if (session.id_prefix.has_value()) {
    matches = referenced == (*session.id_prefix | pending.request.order_id);
  } else {
    // Before the first response tells us how the gateway encodes the session,
    // only the two first responses a new order can have are accepted: the
    // first request of every flow is a new order and the gateway must ack or
    // reject it before any trade in it is reported. A Fill that references
    // another session's taker with the same low 40 bits therefore cannot be
    // mistaken for our answer.
    const bool unambiguous =
        header.type == lob::net::MessageType::Ack || header.type == lob::net::MessageType::Reject;
    matches = unambiguous && client_order_id_of(referenced) == pending.request.order_id;
  }
  if (!matches) {
    ++stats_.unsolicited_responses;
    return;
  }

  if (!session.id_prefix.has_value()) {
    session.id_prefix = referenced & ~kMaxClientOrderId;
  }

  const auto now = Clock::now();
  const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(now - pending.sent_at);
  latency_.record(elapsed.count() > 0 ? static_cast<std::uint64_t>(elapsed.count()) : 0);
  stats_.first_response_by_type.add(header.type);
  ++stats_.first_responses;
  session.pending.reset();
}

RunResult LoadRunner::run() {
  stats_ = RunStats{};
  stats_.orders_requested = options_.orders;

  const RateLimiter limiter(options_.rate);
  const auto start = Clock::now();
  std::uint64_t sent_total = 0;
  std::string error;

  if (sessions_.empty() && options_.orders > 0) {
    RunResult result;
    result.stats = stats_;
    result.error = "no sessions";
    return result;
  }
  const bool has_fd = std::any_of(sessions_.begin(), sessions_.end(), [](const auto& session) {
    return session->transport->fd() >= 0;
  });

  // select() cannot wait on a descriptor at or above FD_SETSIZE; fail before
  // any request is sent rather than part way through a run.
  const bool too_many_fds =
      std::any_of(sessions_.begin(), sessions_.end(),
                  [](const auto& session) { return session->transport->fd() >= FD_SETSIZE; });
  if (too_many_fds) {
    RunResult result;
    result.stats = stats_;
    result.error = "too many sessions for select()";
    return result;
  }

  const auto any_pending = [this]() {
    return std::any_of(sessions_.begin(), sessions_.end(),
                       [](const auto& session) { return session->pending.has_value(); });
  };

  while (true) {
    bool failed = false;

    // 1. Send every request whose pace deadline has passed, one per idle
    //    session per pass.
    for (auto& session : sessions_) {
      if (sent_total >= options_.orders) {
        break;
      }
      if (session->pending.has_value()) {
        continue;
      }
      const auto due = start + limiter.due_after(sent_total);
      if (Clock::now() < due) {
        continue;
      }

      const Request request = session->flow.next();
      std::array<std::byte, lob::net::kMaxWireSize> frame{};
      const std::size_t bytes = encode_request(request, session->next_seq++, frame);
      if (bytes == 0) {
        error = "internal error: could not encode a request";
        failed = true;
        break;
      }

      const auto sent_at = Clock::now();
      if (!session->transport->send(std::span<const std::byte>(frame.data(), bytes))) {
        ++stats_.send_errors;
        error = std::string("session send failed: ") + session->transport->last_error();
        failed = true;
        break;
      }
      session->pending = PendingRequest{request, sent_at};
      ++sent_total;
      ++stats_.requests_sent;
    }
    if (failed) {
      break;
    }

    // 2. Done when everything was sent and answered.
    if (sent_total >= options_.orders && !any_pending()) {
      break;
    }

    // 3. A request that has been outstanding for the response timeout ends
    //    the run: the report still counts it as unanswered.
    const auto now = Clock::now();
    bool expired = false;
    std::chrono::microseconds wait{std::chrono::duration_cast<std::chrono::microseconds>(kMaxWait)};
    for (auto& session : sessions_) {
      if (!session->pending.has_value()) {
        continue;
      }
      const auto deadline = session->pending->sent_at + options_.response_timeout;
      if (now >= deadline) {
        expired = true;
        break;
      }
      const auto remaining = std::chrono::duration_cast<std::chrono::microseconds>(deadline - now);
      wait = std::min(wait, remaining);
    }
    if (expired) {
      stats_.timed_out = true;
      break;
    }

    // 4. Pace the next send. A rate-limited generator sleeps until the next
    //    due time; an in-process transport has no socket to wake us, so the
    //    wait is bounded by the poll interval.
    if (sent_total < options_.orders) {
      const auto due = start + limiter.due_after(sent_total);
      if (due > now) {
        const auto until_due = std::chrono::duration_cast<std::chrono::microseconds>(due - now);
        wait = std::min(wait, until_due);
      } else {
        wait = std::chrono::microseconds::zero();
      }
    }
    if (!has_fd && any_pending()) {
      wait = std::min(wait, kFakePollInterval);
    }
    if (wait.count() < 0) {
      wait = std::chrono::microseconds::zero();
    }

    // 5. Wait for readable sockets and for the in-process transports' queued
    //    responses, then deliver every available frame.
    fd_set readable;
    FD_ZERO(&readable);
    int max_fd = -1;
    bool too_many = false;
    for (auto& session : sessions_) {
      const int fd = session->transport->fd();
      if (fd < 0) {
        continue;
      }
      if (fd >= FD_SETSIZE) {
        too_many = true;
        break;
      }
      FD_SET(fd, &readable);
      max_fd = std::max(max_fd, fd);
    }
    if (too_many) {
      error = "too many sessions for select()";
      break;
    }

    timeval timeout{};
    timeout.tv_sec = static_cast<decltype(timeout.tv_sec)>(wait.count() / 1'000'000);
    timeout.tv_usec = static_cast<decltype(timeout.tv_usec)>(wait.count() % 1'000'000);
    const int ready = ::select(max_fd + 1, &readable, nullptr, nullptr, &timeout);
    if (ready < 0) {
      if (errno == EINTR) {
        continue;
      }
      error = std::string("select: ") + std::strerror(errno);
      break;
    }

    for (auto& session : sessions_) {
      const int fd = session->transport->fd();
      if (fd >= 0 && (ready <= 0 || !FD_ISSET(fd, &readable))) {
        continue;
      }
      ResponseSink sink(*this, *session);
      if (!session->transport->receive(sink)) {
        error = std::string("session receive failed: ") + session->transport->last_error();
        failed = true;
        break;
      }
    }
    if (failed) {
      break;
    }
  }

  const auto end = Clock::now();
  stats_.elapsed_seconds = std::chrono::duration<double>(end - start).count();
  stats_.requests_per_second =
      stats_.elapsed_seconds > 0.0
          ? static_cast<double>(stats_.requests_sent) / stats_.elapsed_seconds
          : 0.0;

  RunResult result;
  result.stats = stats_;
  result.error = std::move(error);
  return result;
}

}  // namespace lob::loadgen
