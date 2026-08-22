// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

#pragma once

// Parsing + DNS resolution for the --upstream argument.
//
// Accepted forms (unchanged legacy first):
//   IP:PORT                 e.g. 127.0.0.1:9001          -> plain HTTP
//   HOST:PORT               e.g. mock.internal:9001      -> plain HTTP
//   http://HOST[:PORT]      default port 80              -> plain HTTP
//   https://HOST[:PORT]     default port 443             -> TLS
//   https://HOST/BASE       e.g. https://api.groq.com/openai
//
// The BASE PATH is a prefix, not a target: it is joined in front of whatever path
// this request would otherwise use, so "/openai" + "/v1/chat/completions" reaches
// "/openai/v1/chat/completions". It exists because several providers serve an
// OpenAI-compatible API below the root (Groq at /openai, OpenRouter at /api,
// Fireworks at /inference) and were unreachable without it.
//
// A base path is REBUILT, never echoed: it lands in a request line, so anything
// that could split or retarget that line is refused at parse time, never
// sanitised. See normalize_base_path in the .cpp for the exact rule.
//
// Deliberately rejected, with a reason in `error` instead of a guess:
//   - userinfo ("https://a@b"), the classic URL-confusion trick where the
//     eyeball host and the connect host differ
//   - a FRAGMENT, in any position: it never travels on the wire, so a URL carrying
//     one is a paste error worth naming, and dropping it silently is worse
//   - a QUERY on a venue whose mode BYTE-FORWARDS. Azure OpenAI needs
//     "?api-version=", and that is now parsed and kept, because a translating mode
//     builds the whole request target itself and has no client query to merge with.
//     Byte-forward does, so the Gateway refuses that pairing at startup, where the
//     mode is known; parse_upstream only reports what it found
//   - IPv6 literals: the transport stack is sockaddr_in/AF_INET end to end;
//     half-accepting "[::1]:443" would fail later with a worse message
//   - hosts with characters outside [A-Za-z0-9.-]: the host string is later
//     written into an HTTP Host header and the TLS SNI field, so a stray CR/LF
//     here is a header-injection primitive, not a typo
//
// Everything here is SETUP path (parsed once at startup): allocation is fine,
// getaddrinfo may block, and errors are strings meant for a human at a terminal.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace llmbridge::net
{
    struct UpstreamSpec
    {
        std::string host;   ///< as written. DNS name or IPv4 literal; feeds Host header + SNI
        /// Normalized base path: empty, or "/..." with no trailing slash. Prefixed
        /// to the request target; empty means the target is used as-is.
        std::string path;
        /// Query as written, WITHOUT the '?'. Empty when there is none. Only a mode
        /// that builds its own target may use it; see the note above.
        std::string query;
        uint16_t port{0};
        bool tls{false};
        std::string error;  ///< non-empty => parse failed, other fields unspecified

        [[nodiscard]] bool ok() const noexcept { return error.empty(); }
    };

    /// Parse an --upstream argument. Never throws; failures come back in .error.
    [[nodiscard]] UpstreamSpec parse_upstream(std::string_view arg);

    /// Resolve a host to IPv4 dotted-quad strings via getaddrinfo (A records only,
    /// deduplicated, resolver order preserved; order matters once failover lands).
    /// An IPv4 literal passes through as itself without touching the resolver.
    /// Empty result => failure, with the getaddrinfo reason in *err if given.
    [[nodiscard]] std::vector<std::string> resolve_host_ipv4(const std::string& host,
                                                             std::string* err = nullptr);
} // namespace llmbridge::net
