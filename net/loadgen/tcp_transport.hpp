#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>

#include "net/loadgen/endpoint.hpp"
#include "net/loadgen/transport.hpp"
#include "net/protocol/framer.hpp"

namespace lob::loadgen {

/// Blocking TCP connection to the gateway. The runner waits for readability
/// with select() before calling receive(), so a receive never blocks; sends
/// block only when the socket buffer is full, which throttles the generator at
/// the gateway instead of dropping frames.
class TcpTransport final : public OrderEntryTransport {
 public:
  /// Resolves `endpoint` and connects. On failure returns nullptr and fills
  /// `error`. SO_NOSIGPIPE (macOS) / MSG_NOSIGNAL (Linux) keep a closed
  /// connection from killing the process, and TCP_NODELAY keeps small
  /// request frames from waiting on Nagle's algorithm.
  static std::unique_ptr<TcpTransport> connect(const Endpoint& endpoint, std::string& error);

  ~TcpTransport() override;
  TcpTransport(const TcpTransport&) = delete;
  TcpTransport& operator=(const TcpTransport&) = delete;

  bool send(std::span<const std::byte> frame) override;
  int fd() const override { return fd_; }
  bool receive(lob::net::FrameSink& sink) override;
  const char* last_error() const override { return error_.c_str(); }

 private:
  explicit TcpTransport(int fd);

  int fd_ = -1;
  lob::net::StreamFramer framer_;
  std::array<std::byte, 8192> buffer_{};
  std::string error_;
};

}  // namespace lob::loadgen
