#pragma once

// In-process fake publisher for the md_client and md_listen tests.
//
// It is header-only test support (nothing links it into a shipped binary): the
// test owns a FakePublisher, sends its scripted frames over a real UDP socket
// on loopback, and serves the same frames from a real TCP recovery listener on
// an ephemeral port. That keeps the feed handler tests end-to-end over the
// sockets without depending on the real publisher, which lands separately.

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <mutex>
#include <span>
#include <thread>
#include <utility>
#include <vector>

#include "net/protocol/codec.hpp"
#include "net/protocol/messages.hpp"

namespace lob::md::testing {

/// Encodes one message into its wire frame.
template <class Message>
std::vector<std::byte> encode_frame(const Message& message, net::Seq seq) {
  std::vector<std::byte> frame(Message::kWireSize);
  const std::size_t written = net::encode(message, seq, frame);
  frame.resize(written);
  return frame;
}

/// Frame for one level update. Tests read better with this than with the full
/// BookUpdate aggregate at every call site.
inline std::vector<std::byte> book_update(Price price, Qty qty, Side side, net::Seq seq) {
  return encode_frame(net::BookUpdate{price, qty, side}, seq);
}

inline std::vector<std::byte> sequence_reset(net::Seq next_seq, net::Seq seq) {
  return encode_frame(net::SequenceReset{next_seq}, seq);
}

inline std::vector<std::byte> trade(OrderId maker, OrderId taker, Price price, Qty qty,
                                    net::Seq seq) {
  return encode_frame(net::Trade{maker, taker, price, qty}, seq);
}

class FakePublisher {
 public:
  FakePublisher() {
    udp_fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
    listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;

    const int reuse = 1;
    ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    ::bind(listen_fd_, reinterpret_cast<const sockaddr*>(&address), sizeof(address));
    ::listen(listen_fd_, 8);

    socklen_t length = sizeof(address);
    ::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&address), &length);
    recovery_port_ = ntohs(address.sin_port);

    thread_ = std::thread([this] { serve(); });
  }

  ~FakePublisher() {
    stop_.store(true);
    if (thread_.joinable()) {
      thread_.join();
    }
    if (listen_fd_ >= 0) {
      ::close(listen_fd_);
    }
    if (udp_fd_ >= 0) {
      ::close(udp_fd_);
    }
  }

  FakePublisher(const FakePublisher&) = delete;
  FakePublisher& operator=(const FakePublisher&) = delete;

  std::uint16_t recovery_port() const { return recovery_port_; }

  /// Adds a frame to the retransmit store, keyed by its header sequence.
  void store_frame(const std::vector<std::byte>& frame) {
    net::Header header;
    if (net::decode_header(frame, header) != net::DecodeStatus::Ok) {
      return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    store_[header.seq] = frame;
  }

  void store_frames(const std::vector<std::vector<std::byte>>& frames) {
    for (const std::vector<std::byte>& frame : frames) {
      store_frame(frame);
    }
  }

  /// Drops a frame from the store so recovery has to answer with a reset.
  void erase_frame(net::Seq seq) {
    std::lock_guard<std::mutex> lock(mutex_);
    store_.erase(seq);
  }

  /// Sequence number the fake sends in a SequenceReset when it cannot serve a
  /// requested range. 0 derives it from the highest stored frame.
  void set_reset_next(net::Seq next_seq) {
    std::lock_guard<std::mutex> lock(mutex_);
    reset_next_ = next_seq;
  }

  /// Always answer recovery with a reset, even when the frames are stored.
  void set_force_reset(bool force) {
    std::lock_guard<std::mutex> lock(mutex_);
    force_reset_ = force;
  }

  std::uint64_t recovery_requests() const { return recovery_requests_.load(); }

  /// Sends each frame in its own datagram.
  void send_frames(std::uint16_t port, const std::vector<std::vector<std::byte>>& frames) {
    for (const std::vector<std::byte>& frame : frames) {
      send_datagram(port, frame);
    }
  }

  /// Packs `per_datagram` frames (at least one) into each datagram, mirroring
  /// the publisher's batching rule.
  void send_packed(std::uint16_t port, const std::vector<std::vector<std::byte>>& frames,
                   std::size_t per_datagram) {
    per_datagram = std::max<std::size_t>(per_datagram, 1);
    std::vector<std::byte> datagram;
    std::size_t in_datagram = 0;
    for (const std::vector<std::byte>& frame : frames) {
      if (in_datagram > 0 && in_datagram + frame.size() > 1200) {
        send_datagram(port, datagram);
        datagram.clear();
        in_datagram = 0;
      }
      datagram.insert(datagram.end(), frame.begin(), frame.end());
      ++in_datagram;
      if (in_datagram == per_datagram) {
        send_datagram(port, datagram);
        datagram.clear();
        in_datagram = 0;
      }
    }
    if (!datagram.empty()) {
      send_datagram(port, datagram);
    }
  }

  void send_datagram(std::uint16_t port, std::span<const std::byte> datagram) {
    sockaddr_in target{};
    target.sin_family = AF_INET;
    target.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    target.sin_port = htons(port);
    ::sendto(udp_fd_, datagram.data(), datagram.size(), 0,
             reinterpret_cast<const sockaddr*>(&target), sizeof(target));
  }

 private:
  void serve() {
    while (!stop_.load()) {
      pollfd entry{};
      entry.fd = listen_fd_;
      entry.events = POLLIN;
      if (::poll(&entry, 1, 20) <= 0) {
        continue;
      }
      const int client = ::accept(listen_fd_, nullptr, nullptr);
      if (client < 0) {
        continue;
      }
      serve_client(client);
      ::close(client);
    }
  }

  void serve_client(int client) {
    std::vector<std::byte> pending;
    while (!stop_.load()) {
      pollfd entry{};
      entry.fd = client;
      entry.events = POLLIN;
      if (::poll(&entry, 1, 20) <= 0) {
        continue;
      }
      std::byte buffer[1024];
      const ssize_t bytes = ::recv(client, buffer, sizeof(buffer), 0);
      if (bytes <= 0) {
        return;
      }
      pending.insert(pending.end(), buffer, buffer + bytes);

      std::size_t offset = 0;
      while (pending.size() - offset >= net::kHeaderSize) {
        net::Header header;
        if (net::decode_header(std::span<const std::byte>(pending).subspan(offset), header) !=
            net::DecodeStatus::Ok) {
          return;
        }
        const std::size_t frame_size = net::kHeaderSize + header.length;
        if (pending.size() - offset < frame_size) {
          break;
        }
        if (header.type == net::MessageType::RecoveryRequest) {
          net::RecoveryRequest request;
          net::Seq seq = 0;
          if (net::decode(std::span<const std::byte>(pending).subspan(offset, frame_size), request,
                          seq) == net::DecodeStatus::Ok) {
            ++recovery_requests_;
            answer(client, request);
          }
        }
        offset += frame_size;
      }
      pending.erase(pending.begin(), pending.begin() + static_cast<std::ptrdiff_t>(offset));
    }
  }

  void answer(int client, const net::RecoveryRequest& request) {
    std::vector<std::vector<std::byte>> frames;
    net::Seq reset_next = 0;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      bool complete = !force_reset_;
      if (complete) {
        for (net::Seq seq = request.from_seq; seq <= request.to_seq; ++seq) {
          const auto found = store_.find(seq);
          if (found == store_.end()) {
            complete = false;
            break;
          }
          frames.push_back(found->second);
        }
      }
      if (!complete) {
        frames.clear();
        reset_next =
            reset_next_ != 0 ? reset_next_ : (store_.empty() ? 0 : store_.rbegin()->first + 1);
        if (reset_next == 0) {
          reset_next = request.to_seq + 1;
        }
      }
    }
    if (reset_next != 0) {
      send_bytes(client, sequence_reset(reset_next, request.to_seq));
      return;
    }
    for (const std::vector<std::byte>& frame : frames) {
      if (!send_bytes(client, frame)) {
        return;
      }
    }
  }

  static bool send_bytes(int fd, std::span<const std::byte> bytes) {
    std::size_t sent = 0;
    while (sent < bytes.size()) {
      const ssize_t n = ::send(fd, bytes.data() + sent, bytes.size() - sent, kSendFlags);
      if (n <= 0) {
        return false;
      }
      sent += static_cast<std::size_t>(n);
    }
    return true;
  }

#ifdef MSG_NOSIGNAL
  static constexpr int kSendFlags = MSG_NOSIGNAL;
#else
  static constexpr int kSendFlags = 0;
#endif

  int udp_fd_ = -1;
  int listen_fd_ = -1;
  std::uint16_t recovery_port_ = 0;
  std::atomic<bool> stop_{false};
  std::thread thread_;
  std::mutex mutex_;
  std::map<net::Seq, std::vector<std::byte>> store_;
  net::Seq reset_next_ = 0;
  bool force_reset_ = false;
  std::atomic<std::uint64_t> recovery_requests_{0};
};

}  // namespace lob::md::testing
