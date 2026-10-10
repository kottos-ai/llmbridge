// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

// Its own file because glibc's struct tcp_info stops before tcpi_min_rtt, and the kernel's
// <linux/tcp.h>, which has it, cannot share a translation unit with <netinet/tcp.h>.

#include "net/socket_util.hpp"

#include <linux/tcp.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <cstddef>

namespace llmbridge::net
{
    TcpRtt tcp_rtt(int fd) noexcept
    {
        TcpRtt out;
        tcp_info info{};
        socklen_t len = sizeof(info);
        if (fd < 0 || ::getsockopt(fd, IPPROTO_TCP, TCP_INFO, &info, &len) != 0) return out;
        // A kernel copies only the struct it knows, so read a field only if it arrived.
        if (len >= offsetof(tcp_info, tcpi_rtt) + sizeof(info.tcpi_rtt)) out.srtt_us = info.tcpi_rtt;
        // The kernel starts the minimum at ~0U, before its first sample.
        if (len >= offsetof(tcp_info, tcpi_min_rtt) + sizeof(info.tcpi_min_rtt) &&
            info.tcpi_min_rtt != ~0U)
            out.min_us = info.tcpi_min_rtt;
        return out;
    }
} // namespace llmbridge::net
