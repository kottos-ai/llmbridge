// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

#include "core/pool.hpp"

#include "net/secure.hpp"

namespace llmbridge
{
    Connection* UpstreamPool::acquire(int venue) noexcept
    {
        if (venue < 0 || static_cast<size_t>(venue) >= _lists.size()) return nullptr;
        Connection* u = _lists[static_cast<size_t>(venue)].newest;
        if (u) remove(*u);
        return u;
    }

    // Any doubt closes it. A connection is pooled only if it is open, the response and
    // the client both allow keep-alive, the body reached a real message boundary (a
    // close-delimited one cannot tell finished from died), the venue has not sent EOF,
    // nothing past the response is buffered, our own request fully left, and the pool
    // has room. Aborted, corrupt and timed-out exchanges arrive here as not keep-alive.
    UpstreamPool::Refusal UpstreamPool::release(Connection& u, bool keep_alive, bool body_ended,
                                                bool request_sent, int64_t now) noexcept
    {
        unpair(&u);
        if (u.doomed || u.fd < 0 || u.pooled) return Refusal::Closed;
        if (!keep_alive) return Refusal::NotKeepAlive;
        if (!body_ended) return Refusal::BodyOpen;
        if (u.peer_eof) return Refusal::PeerClosed;
        if (!u.rbuf.empty()) return Refusal::Leftover;
        if (!request_sent) return Refusal::Unsent;
        if (_count >= _cap) return Refusal::Full;
        if (u.upstream_slot < 0 || static_cast<size_t>(u.upstream_slot) >= _lists.size())
            return Refusal::Closed;

        u.rdec.reset();
        // The request held the client's credential and the next client is someone
        // else. ProxyAuth.CredentialIsScrubbedFromAPooledUpstreamBuffer guards it.
        u.out.scrub();
        u.msg = net::http::Message{};
#ifdef LLMBRIDGE_HAVE_TLS
        u.tls_out.clear(); // per-request ciphertext; the Session is kept, so no new handshake
#endif
        u.ts_pooled = now;

        List& l = _lists[static_cast<size_t>(u.upstream_slot)];
        u.pool_older = l.newest;
        u.pool_newer = nullptr;
        if (l.newest) l.newest->pool_newer = &u;
        else l.oldest = &u;
        l.newest = &u;
        u.pooled = true;
        ++_count;
        return Refusal::None;
    }

    void UpstreamPool::remove(Connection& u) noexcept
    {
        if (!u.pooled) return;
        List& l = _lists[static_cast<size_t>(u.upstream_slot)];
        if (u.pool_newer) u.pool_newer->pool_older = u.pool_older;
        else l.newest = u.pool_older;
        if (u.pool_older) u.pool_older->pool_newer = u.pool_newer;
        else l.oldest = u.pool_newer;
        u.pool_newer = u.pool_older = nullptr;
        u.pooled = false;
        --_count;
    }
} // namespace llmbridge
