// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include <cstdint>

#include "gateway/metrics.hpp"

namespace llmbridge
{
    struct Stats
    {
        /// The t0-t6 stamps grouped three ways (LATENCY.md section 4); a cold
        /// handshake is 50 ms, so `connect` stays out of the microsecond ones.
        Histogram overhead;  // req_path + resp_path: everything the gateway does
        Histogram req_path;  // framing/translate/auth plus the write() to the upstream
        /// TCP + TLS handshake only, on its own range (1 us over 262 ms): the default
        /// put every cold handshake in overflow.
        Histogram connect{1'000, 262'144};
        /// Inbound handshake, accept to handshake done. Empty unless --listen-tls.
        Histogram accept_tls{1'000'000, 32'768};
        Histogram resp_path; // upstream-recv -> client-sent
        /// Time to first token, streamed requests only: t0 to the first content chunk.
        Histogram first_token{100'000, 262'144};
        uint64_t requests = 0;
        uint64_t errors = 0;
        uint64_t upstream_conns_opened = 0;
        uint64_t upstream_retries = 0;  // stale pooled connection -> resent on a fresh one
        uint64_t upstream_reused = 0;   // requests served on a pooled keep-alive conn
        uint64_t upstream_unsent = 0;   // response beat our request out; conn closed, not pooled
        /// Peak unsent ciphertext staged for one connection (tls_out not yet written
        /// plus the write BIO): the backlog, not the bytes streamed.
        uint64_t tls_buffered_peak = 0;
        uint64_t upstream_timeouts = 0; // requests/streams aborted on upstream inactivity
        uint64_t connect_timeouts = 0;  // fresh upstream connects abandoned at the deadline
        uint64_t upstream_reresolved = 0; // venue address lists replaced after a connect failure
        uint64_t client_idle_timeouts = 0;  // established clients dropped after going quiet,
                                            // or for not reading a reply in flight
        uint64_t client_setup_timeouts = 0; // clients dropped for never completing a
                                            // first request (stall, or the wrong protocol)
        /// Inbound handshakes that failed, mostly scanners; logged at DEBUG, counted here.
        uint64_t client_tls_handshake_failures = 0;
        uint64_t stream_pauses = 0;     // epoll: upstream reads paused for client backpressure
        uint64_t uring_enobufs = 0;     // io_uring: provided-buffer pool momentarily empty
        uint64_t accept_backoffs = 0;   // listener paused: out of file descriptors
        /// Requests an installed Policy refused; a subset of `errors`, not a sibling.
        /// Denials climbing while `errors - denials` stays flat is a brute-force
        /// attempt, not an outage.
        uint64_t policy_denied = 0;
        /// Re-dispatched to another venue after a failure; 0 unless a policy opts in.
        /// `upstream_retries` is the same venue on a fresh connection.
        uint64_t upstream_failovers = 0;
        /// Requests whose scratch buffer had to grow to hold them, so the pages it
        /// wrote were fresh and faulted in.
        uint64_t cold_builds = 0;
        uint64_t warm_reuses = 0; // new upstream connections given a retired buffer
        /// Requests whose ciphertext buffer had to grow.
        uint64_t tls_out_grows = 0;
    };
} // namespace llmbridge
