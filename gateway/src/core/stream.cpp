// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

// Stream bookkeeping both backends share: the encoding warning, the latency record
// and the one way to truncate.

#include "gateway/gateway.hpp"

#include "loop.hpp"
#include "request.hpp"
#include "stream.hpp"

namespace llmbridge
{
    using namespace detail;

    // Token counts cannot be read from a compressed stream. request_without drops
    // Accept-Encoding, so the provider compressed unasked; say so.
    void Gateway::stream_warn_if_encoded(const Connection* client,
                                         const net::http::ResponseHead& h) noexcept
    {
        if (!h.encoded) return;
        LB_WARN(ReqId{client->req_seq},
                " upstream compressed the response, so token counts are not readable "
                "for this request; Accept-Encoding was not forwarded");
    }

    // request-path and the handshake do not care what shape the response takes, so a
    // stream records them like any other request. resp_path and added-total it cannot
    // have: both end at the instant a response was built. See LATENCY.md section 4.
    void Gateway::stream_record_latency(const Connection* client) noexcept
    {
        if (now_ns() - _t_start < _warmup_ns) return;
        // t4 stands in for the absent t5; only the request-side fields are read.
        const TimingSplit sp = timing_split(client->ts_req_recvd, client->ts_req_built,
                                            client->ts_wire_ready, client->ts_up_sent,
                                            client->ts_up_recvd, client->ts_up_recvd);
        if (sp.req_path_ns >= 0) _stats.req_path.record(static_cast<uint64_t>(sp.req_path_ns));
        if (sp.connect_ns >= 0) _stats.connect.record(static_cast<uint64_t>(sp.connect_ns));
        // Zero means no content chunk ever arrived, not an instant answer.
        if (client->ts_first_token > 0 && client->ts_req_recvd > 0 &&
            client->ts_first_token >= client->ts_req_recvd)
            _stats.first_token.record(
                static_cast<uint64_t>(client->ts_first_token - client->ts_req_recvd));
    }

    void Gateway::stream_truncate(Connection* client) noexcept
    {
        // One call for the three mutations and the log line: GATEWAY-INTERNALS.md 6b.
        // CorruptStreamFromAProviderThatHoldsTheConnectionStillClosesTheClient guards
        // the stream_ended latch.
        LB_WARN(ReqId{client->req_seq}, " stream TRUNCATED (no [DONE] emitted)",
                " tokens_in=", stream_tokens(client).in,
                " tokens_out=", stream_tokens(client).out,
                " on ", *client);
        client->stream_ended = true;      // no more output will be produced
        client->close_after_resp = true;  // close once the client drains what we have
        ++_stats.errors;                  // and it counts as a failure, not a finish
    }

} // namespace llmbridge
