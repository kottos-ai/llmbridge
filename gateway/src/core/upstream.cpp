// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

// What a venue said about a response (limits, its own id, its name for a failure),
// and where a request goes after its venue failed. Shared by both backends.

#include "gateway/gateway.hpp"

#include "core/limits.hpp"
#include "scan.hpp"

#include <cstring>
#include <string_view>

namespace llmbridge
{
    using namespace detail;

    // What the provider said about its own limits, kept from the response head.
    // Shared by both backends and by the streaming and non-streaming paths, so the
    // four cannot disagree about what a refusal meant.
    void Gateway::note_quota(Connection* client, const net::http::ResponseHead& h) noexcept
    {
        client->req.f.quota_exhausted = static_cast<uint8_t>(h.quota_exhausted);
        client->req.f.retry_after_s = h.retry_after_s;
    }

    // Bedrock is first: a Claude-via-Bedrock response carries x-amzn and
    // Anthropic's own id together, and the AWS one is what CloudTrail indexes.
    void Gateway::note_venue_req_id(Connection* client, std::string_view head) noexcept
    {
        client->req.f.venue_req_id.set({});
        static constexpr std::string_view kNames[] = {"x-amzn-requestid", "request-id",
                                                      "x-request-id"};
        for (const std::string_view name : kNames)
        {
            const std::string_view v = net::http::find_header(head, name);
            if (v.empty()) continue;
            client->req.f.venue_req_id.set(v);
            return;
        }
    }

    void Gateway::note_upstream_error(Connection* client, const net::http::ResponseHead& h,
                                      std::string_view body) noexcept
    {
        client->req.f.upstream_error.set({});
        // 0 is a head we never framed, and a 3xx is not the venue naming a failure.
        if (h.status < 400) return;
        client->req.f.upstream_error.set(scan_error_type(body));
    }

    bool Gateway::pool_upstream(Connection* u, bool keep_alive, bool body_ended) noexcept
    {
        using R = UpstreamPool::Refusal;
        switch (_pool->release(*u, keep_alive, body_ended, upstream_request_sent(u), now_ns()))
        {
            case R::None:
                return true;
            case R::Unsent:
                // The provider answered before our request finished going out, so it
                // saw a truncated request: GATEWAY-INTERNALS.md 8.
                LB_WARN("closing instead of pooling, request was still going out ", *u);
                ++_stats.upstream_unsent;
                return false;
            case R::Full:
                LB_WARN("CAP pool full, closing instead of pooling ", *u,
                        " limit=", static_cast<uint64_t>(_pool->cap()), " (reuse stops here)");
                return false;
            default:
                return false;
        }
    }

    Retry Gateway::failover_target(Connection* client, int status, const char* why) noexcept
    {
        // Every precondition here is about safety, not policy. Re-sending a request the
        // client has already begun receiving would duplicate output; re-sending one we
        // no longer hold is impossible; and an unbounded chain turns one dead provider
        // into a latency multiplier.
        if (!_policy || _upstreams.size() < 2) return {};
        if (!client || client->doomed) return {};
        // Nothing may have reached the client, or a re-send duplicates output.
        //
        // The `streaming` half is unreachable today and is kept deliberately: all six
        // call sites already divert a streaming client to abort_pair or
        // stream_on_upstream_eof before they get here, so no test can distinguish it
        // from `true`. It stays for the same reason tls_invariant_ok() does, six call
        // sites must each keep being right for it to remain unreachable, and the cost
        // of being wrong is a client receiving one answer twice. The `wbuf` half is
        // reachable: a pipelined earlier response can still be draining.
        if (client->req.f.streaming || !client->wbuf.empty()) return {};
        if (client->req.saved.empty()) return {};
        if (client->req.f.failover_attempts >= kMaxFailoverAttempts - 1) return {};

        const FailureFacts f{client->upstream_slot, status, why, client->req.f.failover_attempts,
                             client->req.f.policy_tag};
        const Retry r = _policy->on_failure(f);
        if (!r.retry) return {};
        if (r.upstream_index < 0 || static_cast<size_t>(r.upstream_index) >= _upstreams.size())
        {
            LB_WARN(ReqId{client->req.f.req_seq}, " failover to out-of-range upstream ",
                    static_cast<int64_t>(r.upstream_index), "; giving up");
            return {};
        }
        // Sending it back to the venue that just failed is the one answer that cannot
        // help, and it is how a policy accidentally writes an infinite loop.
        if (r.upstream_index == client->upstream_slot)
        {
            LB_WARN(ReqId{client->req.f.req_seq}, " failover named the venue that just failed; "
                                            "giving up");
            return {};
        }
        // The new venue's overrides, never the failed one's: its model name is wrong for
        // another venue. Copied now, since the views die once on_failure has returned.
        if (!client->req.f.model_override.set(r.model) ||
            !client->req.f.tier_override.set(r.service_tier))
        {
            LB_WARN(ReqId{client->req.f.req_seq}, " failover override longer than the gateway "
                                                  "holds; giving up");
            return {};
        }
        return r;
    }

} // namespace llmbridge
