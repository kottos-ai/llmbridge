// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "net/http.hpp"

/// May this request proceed? llmbridge does not answer that: it authenticates nobody
/// and meters nobody. Anyone who needs it supplies a Policy and keeps it in their own
/// code. A stock build installs none, so nothing is called and requests forward.
namespace llmbridge
{
    /// Metadata only: no route to the body, so "no prompt text" is a property of the
    /// type. `head` does carry the client's Authorization, so never log a RequestFacts
    /// and never retain a view: it points into the connection buffer.
    ///
    /// No lookup helper on purpose; net::http::find_header(head, name) is the safe one.
    struct RequestFacts
    {
        std::string_view head;  ///< request line + headers, through the CRLFCRLF
        size_t body_bytes = 0;  ///< Content-Length as framed. Bytes, not tokens
    };

    struct Decision
    {
        /// `= false` so `Decision d;` refuses too; `Decision{}` zeroes it either way.
        bool allow = false;
        /// Must be 400-599; the gateway substitutes 403 for anything else and warns.
        int deny_status = 401;
        /// Which upstream serves this request, as an index into the gateway's table.
        /// Out of range (including the -1 default) means the FIRST upstream, so a
        /// policy that only authenticates need not know the table exists.
        ///
        /// Ignored when `allow` is false: a refused request reaches no venue.
        int upstream_index = -1;

        /// Rewrite the request's `model` to this before sending it. Empty leaves the
        /// client's untouched, which is what a stock build with no policy always does.
        ///
        /// The same product is named differently at each venue: `gpt-4o` at OpenAI, a
        /// deployment at Azure, `us.anthropic.claude-haiku-4-5-20251001-v1:0` at
        /// Bedrock. Without this the caller must know which venue it will land on,
        /// which defeats routing.
        ///
        /// Must stay valid until the gateway returns from framing this request; it is
        /// copied into the outgoing bytes there and never retained.
        std::string_view model{};

        /// Logged, never sent to the client. Must outlive the call, and must not carry
        /// credential material.
        const char* reason = "policy denied";
        uint64_t tag = 0;
    };

    /// A venue failed before the client saw a single byte. Handed to the policy so it
    /// can send the request somewhere else, which is what failover is.
    ///
    /// Only "the venue did not answer" reaches here: a refused connect, a failed write,
    /// an EOF before the response, an idle timeout. A venue that DID answer with
    /// something we could not parse does NOT, because retrying elsewhere would mask a
    /// real incompatibility as a transient blip.
    struct FailureFacts
    {
        int upstream_index = -1; ///< the venue that just failed
        int status = 502;        ///< what the client gets if nothing is retried
        const char* reason = ""; ///< the gateway's short literal for the failure
        int attempt = 0;         ///< venues already tried for this request, 0 on the first
        uint64_t tag = 0;        ///< Decision::tag of this request, verbatim; 0 if unset
    };

    /// What to do about it. Value-initialised means give up and let the client see the
    /// error, so a policy that ignores failures behaves exactly as before this existed.
    struct Retry
    {
        bool retry = false;
        int upstream_index = -1; ///< must be in range, and not the one that just failed
    };

    /// Supplied at Gateway construction, non-owning, no setter. Called on that
    /// Gateway's loop thread, once per framed request. A policy SHARED across workers
    /// is called from several threads and must handle that itself: a lock here would
    /// sit on the per-request path.
    class Policy
    {
      public:
        virtual ~Policy() = default;

        /// noexcept and allocation-free on the allow path: every request pays for it.
        virtual Decision decide(const RequestFacts& facts) noexcept = 0;

        /// Called only after a venue failed with nothing yet sent to the client. The
        /// DEFAULT NEVER RETRIES: llmbridge has no opinion about which venue is healthy,
        /// because health is measured and it measures nothing. Ordering, ejection
        /// thresholds and cooldown belong to whoever implements this.
        virtual Retry on_failure(const FailureFacts&) noexcept { return {}; }

      protected:
        Policy() = default;
        Policy(const Policy&) = default;
        Policy& operator=(const Policy&) = default;
    };
} // namespace llmbridge
