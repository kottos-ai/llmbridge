// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

// Gateway's construction, teardown and run(), the sequencer, the profile print and
// the warm-buffer rotation: what is neither one request's path nor one backend's.

#include "gateway/gateway.hpp"

#include "loop.hpp"
#include "net/secure.hpp"
#include "request.hpp"

#include <sys/epoll.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>

#include <csignal>
#include <cstdint>
#include <cstdio>
#include <sstream>
#include <stdexcept>
#include <string_view>

#include "net/socket_util.hpp"
#include "net/uring.hpp" // self-guarded by LLMBRIDGE_HAVE_URING

namespace llmbridge
{
    using namespace detail;

    namespace detail
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
    } // namespace detail

    // The single-upstream form, kept because it is what most callers want and because
    // rewriting every existing call site to build a one-entry vector would be churn
    // with no reader benefit. Outbound TLS still arrives through TlsConfig here; the
    // table form takes it per upstream.
    Gateway::Gateway(uint16_t listen_port, std::string upstream_ip, uint16_t upstream_port,
                     int64_t warmup_ns, UpstreamDialect dialect, IoBackend io,
                     int64_t upstream_idle_ns, TlsConfig tls, bool timing_headers,
                     Policy* policy, std::vector<std::string> strip_headers)
        : Gateway(listen_port,
                  std::vector<Upstream>{Upstream{.ip = std::move(upstream_ip),
                                                 .port = upstream_port,
                                                 .tls = tls.upstream_tls,
                                                 .sni_host = tls.sni_host,
                                                 .dialect = dialect}},
                  warmup_ns, io, upstream_idle_ns, tls, timing_headers, policy,
                  std::move(strip_headers))
    {
    }

    Gateway::Gateway(uint16_t listen_port, std::vector<Upstream> upstreams, int64_t warmup_ns,
                     IoBackend io, int64_t upstream_idle_ns, TlsConfig tls, bool timing_headers,
                     Policy* policy, std::vector<std::string> strip_headers)
        : _listen_port(listen_port), _upstreams(std::move(upstreams)), _warmup_ns(warmup_ns),
          _io(io), _upstream_idle_ns(upstream_idle_ns), _tls(std::move(tls)),
          _timing_headers(timing_headers), _policy(policy),
          _wants_prefix_hash(policy != nullptr && policy->wants_prefix_hash())
    {
        // Every path below indexes the table without a bounds special case, so an empty
        // one is a programming error caught here and not a crash on the first request.
        if (_upstreams.empty()) throw std::runtime_error("Gateway: no upstreams configured");
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
        _idle_upstreams.resize(_upstreams.size());
        for (Upstream& u : _upstreams)
        {
            if (u.ips.empty()) u.ips.push_back(u.ip);
            if (u.ip.empty()) u.ip = u.ips.front();
        }
        _rr_inflight.assign(_upstreams.size(), 0);
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
        _epfd = ::epoll_create1(0);
        if (_epfd < 0) throw std::runtime_error("epoll_create1() failed");
        _listen_fd = net::make_listener(_listen_port);
        if (_listen_fd < 0) throw std::runtime_error("failed to bind listen port");
        _listen_conn = new Connection();
        _listen_conn->fd = _listen_fd;
        ep_add_read(_listen_conn);
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
            LB_INFO("  upstream[", static_cast<int64_t>(i), "] ", _upstreams[i].ip, ":",
                    _upstreams[i].port, _upstreams[i].tls ? " tls" : " plaintext",
                    " dialect=", dialect_name(_upstreams[i].dialect));

        // Resolve the event-loop backend: io_uring for Uring/Auto when the kernel
        // supports it, else epoll. Uring requested but unavailable -> epoll.
#ifdef LLMBRIDGE_HAVE_URING
        const bool uring_ok = net::uring::available();
        if (_io == IoBackend::Uring || _io == IoBackend::Auto) _uring_active = uring_ok;
        if (_io == IoBackend::Uring && !uring_ok)
            LB_WARN("io_uring requested but unavailable; falling back to epoll");
        else if (_io == IoBackend::Auto && !uring_ok)
            LB_WARN("io_uring unavailable, using epoll. Inside a container this is "
                    "usually the default seccomp profile blocking io_uring_setup: run "
                    "with --security-opt seccomp=unconfined, or a profile that permits "
                    "the io_uring syscalls, to get the faster backend");
#endif
        const char* want = _io == IoBackend::Uring ? "uring" : _io == IoBackend::Epoll ? "epoll" : "auto";
        LB_INFO("backend requested=", want, " active=", _uring_active ? "io_uring" : "epoll");
    }

    Gateway::~Gateway()
    {
        {
            std::lock_guard<std::mutex> g(_rr.mutex);
            _rr.stop = true;
        }
        _rr.cv.notify_all();
        if (_rr.thread.joinable()) _rr.thread.join();
        for (auto& [id, c] : _clients)
        {
            // An in-flight (acquired, not pooled) upstream is reachable only via
            // peer: free it too, or it leaks when we stop mid-request. (The
            // io_uring loop already nulls these during its drain.)
            if (Connection* u = c->peer) { if (u->fd >= 0) ::close(u->fd); delete u; }
            if (c->fd >= 0) ::close(c->fd);
            delete c;
        }
        for (auto& pool : _idle_upstreams)
            for (Connection* u : pool) { if (u->fd >= 0) ::close(u->fd); delete u; }
        for (Connection* d : _doomed) delete d;
        if (_listen_fd >= 0) ::close(_listen_fd);
        if (_epfd >= 0) ::close(_epfd);
        delete _listen_conn;
    }

    uint16_t Gateway::bound_port() const noexcept
    {
        if (_listen_fd < 0) return 0;
        sockaddr_in addr{};
        socklen_t len = sizeof(addr);
        if (::getsockname(_listen_fd, reinterpret_cast<sockaddr*>(&addr), &len) < 0) return 0;
        return ntohs(addr.sin_port);
    }

    bool Gateway::pooled_buffer_contains(std::string_view needle) const noexcept
    {
        if (needle.empty()) return false;
        for (const auto& pool : _idle_upstreams)
            for (const Connection* u : pool)
                if (u->wbuf.find(needle) != std::string::npos) return true;
        return false;
    }

    void Gateway::print_profile(std::FILE* out, const char* title) const noexcept
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
           << " upstream_reresolved=" << _stats.upstream_reresolved << "\n";
        std::fputs(os.str().c_str(), out);
    }

    void Gateway::prefault(std::string& s) const
    {
        if (s.capacity() >= _prefault_bytes) return;
        // resize writes every byte, which is what maps the pages; clear keeps them.
        s.resize(_prefault_bytes);
        s.clear();
    }

    size_t Gateway::prefault_resident_bytes_for_test()
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

    void Gateway::retire_wbuf(Connection* u) noexcept
    {
        if (u->is_client || _warm.size() >= kWarmBufs || u->wbuf.capacity() < kWarmMin) return;
        // A closing connection may still hold a request, credential included; the pool
        // scrub runs at release, and a connection closed mid-request never got there.
        // The ciphertext is the same request encrypted, so it is scrubbed too.
        WarmSet set;
        net::secure_clear(u->wbuf);
        set.wbuf = std::move(u->wbuf);
#ifdef LLMBRIDGE_HAVE_TLS
        net::secure_clear(u->tls_out);
        set.tls_out = std::move(u->tls_out);
#endif
        _warm.push_back(std::move(set));
    }

    void Gateway::adopt_warm(Connection* u) noexcept
    {
        if (_warm.empty()) return;
        WarmSet set = std::move(_warm.back());
        _warm.pop_back();
        u->wbuf = std::move(set.wbuf);
#ifdef LLMBRIDGE_HAVE_TLS
        u->tls_out = std::move(set.tls_out);
#endif
        ++_stats.warm_reuses;
    }

    void Gateway::note_build(size_t need) noexcept
    {
        if (_rebuild.capacity() < need) ++_stats.cold_builds;
    }

    int Gateway::run()
    {
        prefault(_rebuild);
        prefault(_xlate);
        // One spare set for the first connection, so its first request costs nothing.
        if (_prefault_bytes && _warm.empty())
        {
            WarmSet spare;
            prefault(spare.wbuf);
#ifdef LLMBRIDGE_HAVE_TLS
            prefault(spare.tls_out);
#endif
            _warm.push_back(std::move(spare));
        }
#ifdef LLMBRIDGE_HAVE_URING
        if (_uring_active) return run_uring();
#endif
        return run_epoll();
    }

} // namespace llmbridge
