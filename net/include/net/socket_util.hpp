// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

#pragma once

// Socket setup helpers; out of line because setup runs once per connection, not per request.

#include <cstdint>

struct sockaddr_in; // fwd-declared; callers that use resolve_ipv4 include <netinet/in.h>

namespace llmbridge::net
{
    bool set_nonblocking(int fd) noexcept; // false on fcntl failure
    void set_nodelay(int fd) noexcept;     // TCP_NODELAY: a proxy never wants Nagle's delay
    void set_nosigpipe(int fd) noexcept;   // SO_NOSIGPIPE; no-op on Linux, which ignores SIGPIPE
    int make_listener(uint16_t port, int backlog = 1024) noexcept; // 0.0.0.0:port, -1 on error

    // Non-blocking connect to a dotted-quad ip. It may still be in progress: wait for
    // writability, then call connect_result. -1 only on immediate failure.
    int start_connect(const char* ip, uint16_t port) noexcept;
    int connect_result(int fd) noexcept; // after writability: 0, or the SO_ERROR errno
    int make_client_socket() noexcept;   // unconnected, for io_uring's IORING_OP_CONNECT
    bool resolve_ipv4(const char* ip, uint16_t port, sockaddr_in& out) noexcept; // false: bad ip
} // namespace llmbridge::net