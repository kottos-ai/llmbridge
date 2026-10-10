// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

#pragma once

// What both event loops read and neither owns: constants, the sequencer, the scrub.

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "core/conn.hpp"
#include "core/pool.hpp"
#include "gateway/gateway.hpp"
#include "net/secure.hpp"

namespace llmbridge::detail
{
    constexpr size_t kInitialBuf = 4096;
    constexpr int kEpMaxEvents = 1024;
    constexpr int kPollTickMs = 200; // so request_stop() is observed promptly
    // No request is larger, so more buffered client bytes than this is never legitimate.
    constexpr size_t kMaxClientBuffered = net::http::kMaxHeaderLen + net::http::kMaxBodyLen;
    constexpr int64_t kAcceptBackoffNs = 100'000'000; // after EMFILE: no busy loop
    // A stale pooled connection fails within a round trip of reuse; later, the
    // provider may have run the request, and a resend would bill it twice.
    constexpr int64_t kStaleRetryWindowNs = 1'000'000'000;

    using llmbridge::net::secure_clear; // GATEWAY-INTERNALS.md §9

    extern std::atomic<uint64_t> g_seq; // engine.cpp says why it is a sequencer
} // namespace llmbridge::detail
