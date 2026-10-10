// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

// The policy seam on a freshly framed request.

#include "gateway/gateway.hpp"

#include "loop.hpp"
#include "request.hpp"
#include "response.hpp"

namespace llmbridge
{
    using namespace detail;

    Decision Gateway::policy_decision(Connection* c, const net::http::Message& m) noexcept
    {
        // Head, plus the model and two streaming flags and nothing else from the body.
        // This is the line where "metadata only, no prompt text". All three were read
        // by capture_model, which both backends run first when a policy exists.
        const RequestFacts facts{std::string_view(c->rbuf.data(), m.header_len), m.body_len,
                                 c->req.f.model.view(),
                                 c->req.f.prefix_hash, c->req.f.req_seq, c->req.f.asked_stream, c->req.f.asked_usage};

        Decision d = _policy->decide(facts);
        if (d.allow)
        {
            c->req.f.policy_tag = d.tag; // handed back verbatim in FailureFacts
            return d;
        }

        // A status the responder cannot render would emit a misleading reply, so
        // substitute and say so loudly. The refusal still stands: fail closed.
        if (d.deny_status < 400 || d.deny_status > 599)
        {
            LB_WARN(ReqId{c->req.f.req_seq}, " policy returned out-of-range status ", d.deny_status,
                    " (", d.reason, "); refusing with 403");
            d.deny_status = 403;
        }
        // The one policy string a client sees, spliced into JSON unescaped. Never
        // printed here: a message that failed the check is not safe in a log line.
        if (d.message && !deny_message_ok(d.message))
        {
            LB_WARN(ReqId{c->req.f.req_seq}, " policy message refused (", d.reason,
                    "); sending the generic one");
            d.message = nullptr;
        }
        // Never log `facts`: the head carries the client's Authorization.
        ++_stats.policy_denied;
        return d;
    }

} // namespace llmbridge
