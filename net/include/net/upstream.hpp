// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

#pragma once

// Parsing and DNS resolution of --upstream. The accepted and refused forms, and why each
// refusal exists: DESIGN.md "Upstream URLs". Setup path only: allocation and blocking are fine.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace llmbridge::net
{
    struct UpstreamSpec
    {
        std::string host;   ///< as written: DNS name or IPv4 literal; feeds Host and SNI
        std::string path;   ///< "" or "/..." without a trailing slash, prefixed to the target
        std::string query;  ///< without the '?'; only a mode that builds its own target uses it
        uint16_t port{0};
        bool tls{false};
        std::string error;  ///< non-empty => parse failed, other fields unspecified

        [[nodiscard]] bool ok() const noexcept { return error.empty(); }
    };

    /// Parse an --upstream argument. Never throws; failures come back in .error.
    [[nodiscard]] UpstreamSpec parse_upstream(std::string_view arg);

    /// IPv4 addresses for a host via getaddrinfo, deduplicated, in resolver order; an IPv4
    /// literal passes through. Empty means failure, with the reason in *err if given.
    [[nodiscard]] std::vector<std::string> resolve_host_ipv4(const std::string& host,
                                                             std::string* err = nullptr);
} // namespace llmbridge::net
