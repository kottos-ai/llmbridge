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
        /// Logged, never sent to the client. Must outlive the call, and must not carry
        /// credential material.
        const char* reason = "policy denied";
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

      protected:
        Policy() = default;
        Policy(const Policy&) = default;
        Policy& operator=(const Policy&) = default;
    };
} // namespace llmbridge
