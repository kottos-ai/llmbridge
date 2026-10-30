// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

#pragma once

// A venue as the Gateway is configured with it: what it speaks, where it is, and
// the TLS on either leg.

#include <cstdint>
#include <string>
#include <vector>

#include "provider/dialect.hpp"

namespace llmbridge
{
    /// What a venue speaks: provider::Venue, whose table (provider/dialect.hpp) says
    /// how each one is reached. OpenAI byte-forwards; the rest translate.
    using UpstreamDialect = provider::Venue;

    /// TLS on either leg, declared without ifdefs.
    struct TlsConfig
    {
        /// Outbound, gateway to provider: we are the TLS client and verify them.
        bool upstream_tls = false;
        std::string sni_host; // DNS name for SNI + certificate hostname verification
        std::string ca_file;  // empty = system trust store (tests pass their own CA)

        /// Inbound, client to gateway: we are the TLS server. Set, the single
        /// listener is TLS-only and there is no plaintext port.
        bool client_tls = false;
        std::string cert_file; // PEM chain, leaf first (Let's Encrypt fullchain.pem)
        std::string key_file;  // PEM key, mode 600 or startup refuses it
    };

    /// One place a request can be sent. The gateway holds an ordered table and a policy
    /// picks the index per request; llmbridge never chooses.
    struct Upstream
    {
        /// The address in use. The gateway moves to the next entry of `ips` after a
        /// failed connect and, when `host` is set, re-resolves it off the loop. Empty with
        /// `host` set: none yet, resolved off the loop from run(), and refused until then.
        std::string ip{};
        uint16_t port = 0;
        bool tls = false;     ///< originate TLS to this venue
        std::string sni_host{}; ///< DNS name for SNI, hostname verification and the
                              ///< Host header. Empty for the bare IP:PORT form.
        UpstreamDialect dialect = UpstreamDialect::OpenAI;
        /// Prefixed to this venue's request target. Empty, or "/..." with no trailing
        /// slash; net::parse_upstream enforces that.
        std::string base_path{};

        std::string host_hdr{}; ///< derived at construction, and with `ip`; host_header_for()

        /// Query for this venue, without the '?'. Only a mode that builds its own
        /// target may carry one; the constructor refuses it on a byte-forwarding venue.
        std::string query{};

        /// AWS region for SigV4, derived from the hostname at construction. Empty
        /// makes a Bedrock venue refuse every request instead of signing with a guess.
        std::string aws_region{};

        /// A Gemini or Vertex venue that takes a Google OAuth token: a named key goes as
        /// `Authorization: Bearer`, and a translated request sends the client's Bearer,
        /// never an x-goog-api-key beside it. Off, the key goes as x-goog-api-key.
        bool auth_bearer = false;


        std::vector<std::string> ips{};
        std::string host{};
    };

    /// Event-loop backend. Auto is io_uring when the kernel has it, else epoll.
    enum class IoBackend
    {
        Auto,
        Epoll,
        Uring,
    };
} // namespace llmbridge
