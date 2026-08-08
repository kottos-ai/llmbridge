// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

#pragma once

// Thin BSD-socket helpers for the llmbridge proxy. Linux/epoll target; the calls
// used here (fcntl O_NONBLOCK, SO_REUSEPORT, TCP_NODELAY) are all standard on
// Linux. SIGPIPE is suppressed process-wide (the event loop ignores it) rather
// than per-socket, since Linux has no SO_NOSIGPIPE. Kept deliberately small so
// swapping the poller doesn't touch this file.
//
// Connection *setup* is not on the hot path (it happens once per connection,
// not per request), so these live out-of-line in socket_util.cpp; the per-
// request framing that IS hot stays header-only inline in net/http.hpp.

#include <cstdint>

struct sockaddr_in; // fwd-declared; callers that use resolve_ipv4 include <netinet/in.h>

namespace llmbridge::net
{
    // Set O_NONBLOCK. Returns false on fcntl failure.
    bool set_nonblocking(int fd) noexcept;

    // Disable Nagle (TCP_NODELAY); we never want coalescing delay on a proxy.
    void set_nodelay(int fd) noexcept;

    // Suppress SIGPIPE for this fd where the platform supports it
    // (SO_NOSIGPIPE). On Linux this is a no-op: the process ignores SIGPIPE
    // globally instead (see Gateway's constructor and each tool's main()).
    void set_nosigpipe(int fd) noexcept;

    // Create a non-blocking IPv4 listening socket bound to 0.0.0.0:port, with
    // SO_REUSEADDR + SO_REUSEPORT (clean restarts; multiple loops can share the
    // port later). Returns the fd, or -1 on error.
    int make_listener(uint16_t port, int backlog = 1024) noexcept;

    // Begin a non-blocking connect to ip:port (dotted-quad ip). Returns the fd;
    // the connect may still be in progress (EINPROGRESS); wait for writability
    // then check connect_result(). Returns -1 only on immediate failure.
    int start_connect(const char* ip, uint16_t port) noexcept;

    // After a connect socket reports writable, returns 0 on success or the
    // SO_ERROR errno otherwise.
    int connect_result(int fd) noexcept;

    // Create a non-blocking TCP socket with TCP_NODELAY but DON'T connect it, for
    // the io_uring path, which issues the connect as a ring op (IORING_OP_CONNECT).
    // Returns the fd, or -1 on error.
    int make_client_socket() noexcept;

    // Fill `out` (a sockaddr_in) for ip:port. Returns false on a bad dotted-quad.
    bool resolve_ipv4(const char* ip, uint16_t port, sockaddr_in& out) noexcept;
} // namespace llmbridge::net