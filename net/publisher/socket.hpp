#pragma once

// Small POSIX socket helpers shared by the publisher's UDP and TCP paths. They
// are deliberately thin: the components own their sockets and close them in
// their destructors, and every failure is reported to the caller instead of
// aborting.

#include <netinet/in.h>
#include <sys/socket.h>

#include <cstddef>
#include <string>

#include "net/publisher/endpoint.hpp"

namespace lob::publisher {

/// Accepted by send() so a closed recovery client cannot raise SIGPIPE.
inline constexpr int kSendFlags =
#ifdef MSG_NOSIGNAL
    MSG_NOSIGNAL;
#else
    0;
#endif

/// Resolves `endpoint` for a socket of `socket_type` (SOCK_DGRAM or
/// SOCK_STREAM). IPv4 and IPv6 are both accepted; the first result wins.
bool resolve_endpoint(const Endpoint& endpoint, int socket_type, sockaddr_storage& out,
                      socklen_t& out_length, std::string& error);

/// Creates, binds and listens on a non-blocking TCP socket. A port of 0 asks
/// the kernel for an ephemeral port; use bound_port() to discover it. An empty
/// host binds every interface.
int create_tcp_listener(const Endpoint& bind_endpoint, std::string& error);

/// Creates a non-blocking TCP socket for outbound use. SIGPIPE is suppressed
/// where the platform offers a socket option for it.
int create_tcp_socket(const sockaddr_storage& address, std::string& error);

/// Port the socket is bound to, 0 when it could not be read.
std::uint16_t bound_port(int fd);

bool make_nonblocking(int fd);

/// Best-effort SO_NOSIGPIPE (macOS); Linux uses MSG_NOSIGNAL in send().
void suppress_sigpipe(int fd);

/// The C library's text for an errno value.
std::string socket_error_text(int error);

}  // namespace lob::publisher
