// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

// The Engine's construction, teardown and run(), the sequencer, the profile print and
// the warm-buffer rotation: what is neither one request's path nor one backend's.

#include "engine.hpp"

#include "core/io.hpp"
#include "core/limits.hpp"
#include "core/registry.hpp"
#include "net/secure.hpp"
#include "request.hpp"

#include <arpa/inet.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>

#include <csignal>
#include <cstdint>
#include <cstdio>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <type_traits>

#include "io/epoll_io.hpp"
#include "io/uring_io.hpp" // self-guarded by LLMBRIDGE_HAVE_URING
#include "net/socket_util.hpp"

namespace llmbridge::detail
{
    // Total order across all workers, independent of any clock.
    //
    // std::atomic, not volatile: volatile provides neither atomicity nor
    // inter-thread ordering in C++ (it is for memory-mapped I/O), and workers are
    // std::threads sharing this process, so a plain or volatile counter would be a
    // data race that hands two requests the same number.
    //
    // relaxed is sufficient and is the cheapest correct choice: every atomic has a
    // single total modification order, so fetch_add yields unique, increasing
    // values in the order the increments occurred. We need uniqueness and
    // ordering of the counter itself, not ordering of surrounding memory, so no
    // fences are warranted.
    //
    // Why this exists at all: two requests can share a nanosecond, and clocks on
    // different hosts cannot be trusted to sub-millisecond agreement without PTP.
    // (t0, seq) is a total order that needs neither. This is the sequencer
    // pattern: an exchange defines order by arrival at a sequencing point, not by
    // comparing timestamps, and it is why the tape for inference should sequence
    // instead of timestamp.
    std::atomic<uint64_t> g_seq{0};

    Engine::Engine(uint16_t listen_port, std::vector<Upstream> upstreams, int64_t warmup_ns,
                   IoBackend io, int64_t upstream_idle_ns, TlsConfig tls, bool timing_headers,
                   Policy* policy, std::vector<std::string> strip_headers)
        : _listen_port(listen_port), _upstreams(std::move(upstreams)), _warmup_ns(warmup_ns),
          _io(io), _upstream_idle_ns(upstream_idle_ns), _tls(std::move(tls)),
          _timing_headers(timing_headers), _policy(policy),
          _wants_prefix_hash(policy != nullptr && policy->wants_prefix_hash()),
          _decides_on_head(policy != nullptr && policy->decides_on_head())
    {
        // Every path below indexes the table without a bounds special case, so an empty
        // one is a programming error caught here and not a crash on the first request.
        if (_upstreams.empty()) throw std::runtime_error("Gateway: no upstreams configured");
        // The address first: the bare IP:PORT form's Host header is built from it.
        // A venue given only a name has neither yet; run() asks the resolver for it.
        for (Upstream& u : _upstreams)
        {
            if (u.ips.empty() && !u.ip.empty()) u.ips.push_back(u.ip);
            if (u.ip.empty() && !u.ips.empty()) u.ip = u.ips.front();
        }
        for (Upstream& u : _upstreams)
        {
            u.host_hdr = host_header_for(u);
            u.aws_region = aws_region_for(u);
            if (!u.query.empty() && u.dialect != UpstreamDialect::Azure)
                throw std::runtime_error(
                    "upstream '" + u.sni_host + "' has a query (" + u.query +
                    ") but its mode does not build its own request target; only "
                    "--translate azure may carry one");
        }
        _clients = std::make_unique<Registry<ClientConn>>();
        _upconns = std::make_unique<Registry<UpstreamConn>>();
        _doomed = std::make_unique<Registry<ConnBase>>();
        _pool = std::make_unique<UpstreamPool>();
        _pool->init(_upstreams.size(), Gateway::kMaxIdleUpstreams);
        _rr_inflight.assign(_upstreams.size(), 0);
        _rr_next_ns.assign(_upstreams.size(), 0);
        _rr_backoff_ns.assign(_upstreams.size(), 0);
        _rr->done.reserve(_upstreams.size());
        _rr_batch.reserve(_upstreams.size());
        // Normalize once, at construction: lower-case with the colon, so the hot path
        // compares against a raw header line with no per-request work.
        for (std::string& h : strip_headers)
        {
            for (char& c : h) c = (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
            if (h.empty()) continue;
            if (h.back() != ':') h.push_back(':');
            _strip_headers.push_back(std::move(h));
        }

#ifdef LLMBRIDGE_HAVE_TLS
        if (any_upstream_tls())
        {
            // Setup path: a bad trust store must fail construction, not the first
            // request. Same throw discipline as the listener below.
            net::tls::Context::ClientOptions o;
            o.ca_file = _tls.ca_file;
            if (!_tls_upstream_ctx.init_client(o))
                throw std::runtime_error("TLS context init failed: " + _tls_upstream_ctx.last_error());
        }
        if (_tls.client_tls)
        {
            // Same discipline as above and for a sharper reason: a missing or
            // unreadable certificate must stop the process, never downgrade the
            // listener to plaintext. A client that dialled https and got a
            // plaintext answer would send its credential in the clear.
            net::tls::Context::ServerOptions so;
            so.cert_file = _tls.cert_file;
            so.key_file = _tls.key_file;
            if (!_tls_client_ctx.init_server(so))
                throw std::runtime_error("inbound TLS init failed: " + _tls_client_ctx.last_error());
        }
#else
        if (any_upstream_tls())
            throw std::runtime_error("TLS upstream requested but built without LLMBRIDGE_TLS");
#endif
        // Linux has no SO_NOSIGPIPE; ignore SIGPIPE process-wide so a write to a
        // peer-closed socket returns EPIPE instead of killing us. Idempotent, so
        // safe to set here (covers the daemon and the test harness alike).
        std::signal(SIGPIPE, SIG_IGN);
        _listen_fd = net::make_listener(_listen_port);
        if (_listen_fd < 0) throw std::runtime_error("failed to bind listen port");
        LB_INFO("listening port=", _listen_port, " upstreams=",
                static_cast<int64_t>(_upstreams.size()),
                _tls.client_tls ? " inbound=tls" : " inbound=plaintext");
        if (!_tls.client_tls)
            LB_WARN("inbound TLS is off and the listener accepts every interface, so a "
                    "client's Authorization crosses the network in clear. Safe as a "
                    "loopback sidecar behind something that terminates TLS; unsafe as a "
                    "remote endpoint. Build -DLLMBRIDGE_TLS=ON and run --listen-tls "
                    "--tls-cert --tls-key to terminate it here.");
        for (size_t i = 0; i < _upstreams.size(); ++i)
        {
            const Upstream& u = _upstreams[i];
            const bool unresolved = !has_address(u) && !u.host.empty();
            LB_INFO("  upstream[", static_cast<int64_t>(i), "] ", unresolved ? u.host : u.ip, ":", u.port,
                    unresolved ? " (not resolved yet)" : "", u.tls ? " tls" : " plaintext",
                    " dialect=", dialect_name(u.dialect));
            // Nothing will ever give it one: no name to look up.
            if (!has_address(u) && u.host.empty())
                LB_WARN("  upstream[", static_cast<int64_t>(i), "] address '", u.ip,
                        "' is not an IPv4 address and it has no host to resolve; every request to it is refused");
        }

        // Resolve the event-loop backend: io_uring for Uring/Auto when the kernel has it,
        // else epoll for Auto. Uring never falls back: whoever asked for io_uring and got
        // epoll would take the slower backend's numbers for the faster one's.
#ifdef LLMBRIDGE_HAVE_URING
        const int uring_err = net::uring::setup_errno();
        if (_io == IoBackend::Uring || _io == IoBackend::Auto) _uring_active = uring_err == 0;
        if (uring_err && _io == IoBackend::Uring)
            LB_ERROR("io_uring requested and unavailable, and Uring never falls back to epoll, "
                     "so this worker will not serve: ", net::uring::why_unavailable(uring_err));
        else if (uring_err && _io == IoBackend::Auto)
            LB_WARN("io_uring unavailable, using epoll: ", net::uring::why_unavailable(uring_err));
#else
        if (_io == IoBackend::Uring)
            LB_ERROR("io_uring requested, and this build has none (linux/io_uring.h was missing "
                     "when it was compiled), so this worker will not serve");
#endif
        const char* want = _io == IoBackend::Uring ? "uring" : _io == IoBackend::Epoll ? "epoll" : "auto";
        const char* active = _uring_active ? "io_uring" : _io == IoBackend::Uring ? "none" : "epoll";
        LB_INFO("backend requested=", want, " active=", active);
    }

    Engine::~Engine()
    {
        // A lookup in flight can take the resolver's timeouts while DNS is down; it is left
        // to finish on its own reference to the state, so shutdown never waits on it.
        bool busy = false;
        size_t queued = 0;
        {
            std::lock_guard<std::mutex> g(_rr->mutex);
            _rr->stop = true;
            busy = _rr->busy;
            queued = _rr->pending.size();
        }
        _rr->cv.notify_all();
        if (_rr_thread.joinable())
        {
            if (busy)
            {
                LB_INFO("resolver: a name lookup is still running at shutdown; its thread is left to "
                        "finish on its own, not waited for, lookups dropped=",
                        static_cast<int64_t>(queued));
                _rr_thread.detach();
            }
            else _rr_thread.join();
        }
        // Every connection is in one registry, pooled and in-flight upstreams included,
        // so this frees each once. The pool's links into the freed are never read again.
        const auto release = [](auto* c) {
            if (c->fd >= 0) ::close(c->fd);
            visit(*c, [](auto& k) { delete &k; });
        };
        _clients->clear(release);
        _upconns->clear(release);
        _doomed->clear(release);
        bury_shadows();
        if (_listen_fd >= 0) ::close(_listen_fd);
    }

    const Upstream& Engine::venue_at(int slot) const noexcept
    {
        const size_t i = (slot >= 0) ? static_cast<size_t>(slot) : 0;
        return _upstreams[i < _upstreams.size() ? i : 0];
    }

    const Upstream& Engine::upstream_of(const ClientConn* c) const noexcept { return venue_at(c->upstream_slot); }

    const Upstream& Engine::upstream_of(const UpstreamConn* u) const noexcept { return venue_at(u->venue); }

    uint16_t Engine::bound_port() const noexcept
    {
        if (_listen_fd < 0) return 0;
        sockaddr_in addr{};
        socklen_t len = sizeof(addr);
        if (::getsockname(_listen_fd, reinterpret_cast<sockaddr*>(&addr), &len) < 0) return 0;
        return ntohs(addr.sin_port);
    }

    bool Engine::pooled_buffer_contains(std::string_view needle) const noexcept
    {
        if (needle.empty()) return false;
        bool found = false;
        _pool->for_each([&](const UpstreamConn* u) {
            found = found || u->out.bytes().find(needle) != std::string::npos;
        });
        return found;
    }

    size_t Engine::pooled_upstream_count() const noexcept { return _pool->size(); }

    void Engine::set_pool_cap(size_t n) noexcept { _pool->set_cap(n); }

    void Engine::print_profile(std::FILE* out, const char* title) const noexcept
    {
        std::ostringstream os;
        os << "\n=== llmbridge " << title << " ===\n";
        if (_stats.overhead.total() > 0) _stats.overhead.print(os, "added-total   ");
        if (_stats.req_path.total() > 0) _stats.req_path.print(os, "req-path      ");
        if (_stats.resp_path.total() > 0) _stats.resp_path.print(os, "resp-path     ");
        if (_stats.connect.total() > 0) _stats.connect.print(os, "connect(TLS)  ");
        if (_stats.accept_tls.total() > 0) _stats.accept_tls.print(os, "accept(TLS)   ");
        if (_stats.first_token.total() > 0) _stats.first_token.print(os, "first-token   ");
        os << "requests=" << _stats.requests << " errors=" << _stats.errors
           << " upstream_conns_opened=" << _stats.upstream_conns_opened
           << " upstream_reused=" << _stats.upstream_reused
           << " cold_builds=" << _stats.cold_builds << " warm_reuses=" << _stats.warm_reuses
           << " tls_out_grows=" << _stats.tls_out_grows << "\n";
        os << "client_setup_timeouts=" << _stats.client_setup_timeouts
           << " client_idle_timeouts=" << _stats.client_idle_timeouts
           << " tls_handshake_failures=" << _stats.client_tls_handshake_failures
           << " upstream_timeouts=" << _stats.upstream_timeouts
           << " connect_timeouts=" << _stats.connect_timeouts
           << " upstream_reresolved=" << _stats.upstream_reresolved
           << " upstream_unresolved=" << _stats.upstream_unresolved
           << " upstream_local_failures=" << _stats.upstream_local_failures << "\n";
        if (_stats.streamed_bodies + _stats.streamed_body_capped > 0)
            os << "streamed_bodies=" << _stats.streamed_bodies << " refused=" << _stats.streamed_body_refusals
               << " capped=" << _stats.streamed_body_capped << " timed_out=" << _stats.streamed_body_timeouts
               << "\n";
        if (_stats.policy_head_decisions > 0)
            os << "policy_head_decisions=" << _stats.policy_head_decisions
               << " parked=" << _stats.streamed_body_parked << " redone=" << _stats.streamed_body_redone
               << " confirm_refusals=" << _stats.streamed_body_confirm_refusals
               << " model_substituted=" << _stats.streamed_model_substituted
               << " model_padded=" << _stats.streamed_model_padded
               << " override_spliced=" << _stats.streamed_override_spliced
               << " override_redone=" << _stats.streamed_override_redone << "\n";
        if (_stats.hedged_requests > 0)
            os << "hedged_requests=" << _stats.hedged_requests << " hedge_legs=" << _stats.hedge_legs
               << " hedge_legs_cut=" << _stats.hedge_legs_cut << "\n";
        std::fputs(os.str().c_str(), out);
    }

    void Engine::prefault(std::string& s) const
    {
        if (s.capacity() >= _prefault_bytes) return;
        // resize writes every byte, which is what maps the pages; clear keeps them.
        s.resize(_prefault_bytes);
        s.clear();
    }

    size_t Engine::prefault_resident_bytes_for_test()
    {
        prefault(_rebuild);
        prefault(_xlate);
        const long page = ::sysconf(_SC_PAGESIZE);
        const auto base = reinterpret_cast<uintptr_t>(_rebuild.data());
        const uintptr_t first = base & ~static_cast<uintptr_t>(page - 1);
        const size_t span = _rebuild.capacity() + (base - first);
        const size_t pages = (span + static_cast<size_t>(page) - 1) / static_cast<size_t>(page);
        std::vector<unsigned char> vec(pages);
        if (::mincore(reinterpret_cast<void*>(first), span, vec.data()) != 0) return 0;
        size_t resident = 0;
        for (const unsigned char v : vec) resident += (v & 1) ? static_cast<size_t>(page) : 0;
        return resident;
    }

    void Engine::retire_wbuf(UpstreamConn* u) noexcept
    {
        if (_warm.size() >= Gateway::kWarmBufs || u->out.capacity() < Gateway::kWarmMin) return;
        // A closing connection may still hold a request, credential included; the pool
        // scrub runs at release, and a connection closed mid-request never got there.
        // The ciphertext is the same request encrypted, so it is scrubbed too. Freed
        // only with nothing in flight, so neither is pinned and both are moved.
        WarmSet set;
        u->out.scrub();
        set.out = u->out.take();
#ifdef LLMBRIDGE_HAVE_TLS
        u->tls_out.scrub();
        set.tls_out = u->tls_out.take();
#endif
        _warm.push_back(std::move(set));
    }

    void Engine::adopt_warm(UpstreamConn* u) noexcept
    {
        if (_warm.empty()) return;
        WarmSet set = std::move(_warm.back());
        _warm.pop_back();
        u->out.adopt(std::move(set.out));
#ifdef LLMBRIDGE_HAVE_TLS
        u->tls_out.adopt(std::move(set.tls_out));
#endif
        ++_stats.warm_reuses;
    }

    // Only an upstream's buffers are worth keeping: they hold the rebuilt requests.
    void Engine::free_conn(ConnBase* c) noexcept
    {
        _doomed->remove(c);
        visit(*c, [this](auto& k) {
            if constexpr (std::is_same_v<std::remove_cvref_t<decltype(k)>, UpstreamConn>) retire_wbuf(&k);
            delete &k;
        });
    }

    void Engine::attach(IoOps* io, const IoCaps& caps) noexcept
    {
        _shadow_io.inner = io;
        _ops = io ? &_shadow_io : nullptr;
        _caps = caps;
    }

    void Engine::note_build(size_t need) noexcept
    {
        if (_rebuild.capacity() < need) ++_stats.cold_builds;
    }

    int Engine::run()
    {
        // On the loop thread, which owns the per-venue resolution state; never for an
        // engine already told to stop, whose lookups would serve nothing.
        if (!_stop.load(std::memory_order_relaxed)) resolve_unaddressed();
        prefault(_rebuild);
        prefault(_xlate);
        // One spare set for the first connection, so its first request costs nothing.
        if (_prefault_bytes && _warm.empty())
        {
            WarmSet spare;
            prefault(spare.out);
#ifdef LLMBRIDGE_HAVE_TLS
            prefault(spare.tls_out);
#endif
            _warm.push_back(std::move(spare));
        }
#ifdef LLMBRIDGE_HAVE_URING
        if (_uring_active)
        {
            UringIo io(*this);
            if (io.ready()) return io.run(); // else it said why
            if (_io == IoBackend::Uring)
            {
                LB_ERROR("io_uring could not be set up (above): the backend needs Linux 6.0 or "
                         "newer and the io_uring syscalls allowed. Uring never falls back to "
                         "epoll, so this worker will not serve");
                return 1;
            }
            LB_WARN("io_uring could not be set up, using epoll");
        }
#endif
        if (_io == IoBackend::Uring) return 1; // the constructor said why
        EpollIo io(*this);
        return io.run();
    }

} // namespace llmbridge::detail
