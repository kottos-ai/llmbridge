// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include <cstddef>
#include <string_view>

#include "net/http.hpp"

/// The one question the gateway asks and does not answer: may this request proceed?
/// llmbridge authenticates nobody and meters nobody. Anyone who needs that supplies a
/// Policy and keeps it in their own code.
///
/// A stock build installs none: nothing is called and requests forward as before.
/// Where one IS installed, a zeroed `Decision` refuses, so a forgotten branch in the
/// policy cannot become a forwarded request.
namespace llmbridge
{
    /// What a policy gets to see. Metadata only: there is no route to the request body
    /// from here, which makes "no prompt or completion text" a property of the type
    /// instead of a promise.
    ///
    /// `head` does contain the client's Authorization, necessarily. So: never log a
    /// RequestFacts or anything taken from one, and never retain a view, since `head`
    /// points into the connection buffer and dies with the call.
    struct RequestFacts
    {
        std::string_view head;  ///< request line + headers, through the CRLFCRLF
        size_t body_bytes = 0;  ///< Content-Length as framed. Bytes, not tokens

        // No lookup helper here on purpose. net::http::find_header(head, name) is
        // the safe one and is two lines away; a member wrapping it would be OSS
        // surface that only consumers use, and a second place for the rules about
        // reading a header to drift out of sync.
    };

    struct Decision
    {
        // `= false` so `Decision d;` is a refusal too. `Decision{}` zeroes it either
        // way; this covers the other spelling, and costs a token.
        bool allow = false;

        /// Must be 400-599; the gateway substitutes 403 for anything else and warns.
        int deny_status = 401;

        /// Logged, never sent to the client. Must outlive the call and must not carry
        /// credential material.
        const char* reason = "policy denied";
    };

    /// Supplied at Gateway construction, non-owning, no setter.
    ///
    /// Called on the loop thread of the Gateway holding it, once per framed request. A
    /// policy given to one Gateway can keep plain counters; one SHARED across workers
    /// is called from several threads and must handle that itself, because a lock here
    /// would sit on the per-request path.
    class Policy
    {
      public:
        virtual ~Policy() = default;

        /// noexcept and allocation-free on the allow path: every request pays for it.
        virtual Decision decide(const RequestFacts& facts) noexcept = 0;

      protected:
        Policy() = default;
        Policy(const Policy&) = default;
        Policy& operator=(const Policy&) = default;
    };
} // namespace llmbridge
