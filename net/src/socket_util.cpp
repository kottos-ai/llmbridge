// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

#include "net/socket_util.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>

namespace llmbridge::net
{
    bool set_nonblocking(int fd) noexcept
    {
        int flags = ::fcntl(fd, F_GETFL, 0);
        if (flags < 0) return false;
        return ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
    }

    void set_nodelay(int fd) noexcept
    {
        int one = 1;
        ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    }

    void set_nosigpipe(int fd) noexcept
    {
#ifdef SO_NOSIGPIPE
        int one = 1;
        ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#else
        // Linux: no SO_NOSIGPIPE. SIGPIPE is ignored process-wide instead, so a
        // write to a peer-closed fd returns EPIPE instead of killing us.
        (void)fd;
#endif
    }

    int make_listener(uint16_t port, int backlog) noexcept
    {
        int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (fd < 0) return -1;
        int one = 1;
        ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        ::setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        addr.sin_port = htons(port);
        if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0)
        {
            ::close(fd);
            return -1;
        }
        if (::listen(fd, backlog) < 0)
        {
            ::close(fd);
            return -1;
        }
        return fd;
    }

    int start_connect(const char* ip, uint16_t port) noexcept
    {
        const int fd = make_client_socket();
        if (fd < 0) return -1;

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        if (::inet_pton(AF_INET, ip, &addr.sin_addr) != 1)
        {
            ::close(fd);
            return -1;
        }
        int rc = ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
        if (rc < 0 && errno != EINPROGRESS)
        {
            ::close(fd);
            return -1;
        }
        return fd;
    }

    int connect_result(int fd) noexcept
    {
        int err = 0;
        socklen_t len = sizeof(err);
        if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) < 0) return errno;
        return err;
    }

    // Non-blocking and close-on-exec from creation: no window where a fork+exec in an
    // embedding process inherits it, and two fcntl calls fewer per connection.
    int make_client_socket() noexcept
    {
        const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (fd < 0) return -1;
        set_nodelay(fd);
        set_nosigpipe(fd);
        return fd;
    }

    bool resolve_ipv4(const char* ip, uint16_t port, sockaddr_in& out) noexcept
    {
        out = sockaddr_in{};
        out.sin_family = AF_INET;
        out.sin_port = htons(port);
        return ::inet_pton(AF_INET, ip, &out.sin_addr) == 1;
    }
} // namespace llmbridge::net