// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

#pragma once

// Gateway: the whole llmbridge proxy behind one class. A single-threaded, non-blocking
// event loop (epoll or io_uring): accept, frame, optionally translate the provider
// dialect, forward over a keep-alive upstream pool, read back, translate, reply.
// The machinery is detail::Engine (src/engine.hpp); how it fits, GATEWAY-INTERNALS.md;
// what each timing number means, LATENCY.md, which is normative.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "gateway/metrics.hpp"
#include "gateway/policy.hpp"
#include "gateway/refusals.hpp"
#include "gateway/sink.hpp"
#include "gateway/stats.hpp"
#include "gateway/venue.hpp"

namespace llmbridge
{
    /// How much of the body RequestFacts::prefix_hash covers.
    /// The field promises identity of "the leading bytes" and not this number.
    inline constexpr size_t kPrefixHashBytes = 4096;

    struct UpstreamConn; // src/core/conn.hpp
    namespace detail
    {
        class Engine; // src/engine.hpp
    }

    class Gateway
    {
    public:
        /// How long a request may sit with no bytes from the upstream before the
        /// gateway gives up (0 = disabled). Once streaming, it bounds the gap between events.
        static constexpr int64_t kDefaultUpstreamIdleNs = 120LL * 1000 * 1000 * 1000; // 120 s
        /// How long a fresh connect may take, TCP and TLS handshake together, before
        /// the request fails over or gets a 502 (0 = disabled). Linux retransmits a
        /// lost SYN at 1 s, so not less than 3 s.
        static constexpr int64_t kDefaultConnectNs = 5LL * 1000 * 1000 * 1000; // 5 s
        /// Keep-alive pool bound. 256 cost the non-streaming path 2.4x its throughput,
        /// git-bisected: do not lower it without ./bench/saturate.sh with BACKENDS=4.
        static constexpr size_t kMaxIdleUpstreams = 8192;
        /// Retired send buffers kept for the next upstream, each as large as the
        /// largest request seen, so this bounds resident memory.
        static constexpr size_t kWarmBufs = 8;
        static constexpr size_t kWarmMin = 64 * 1024; // smaller is not worth keeping
        /// How long a pooled upstream may sit unused; every crossing of it costs the
        /// next request a reconnect, hence a knob.
        static constexpr int64_t kDefaultPoolIdleNs = 30LL * 1000 * 1000 * 1000; // 30 s
        static constexpr int64_t kDefaultHeartbeatNs = 300LL * 1000 * 1000 * 1000; // 5 min
        /// A client that never completes a request: a stuck handshake, a half-sent
        /// request, the wrong protocol. Generous for slow mobile clients.
        static constexpr int64_t kClientSetupNs = 30LL * 1000 * 1000 * 1000; // 30 s
        /// An established client gone quiet. A descriptor bound for an exposed
        /// listener, not a load-balancing device; 0 disables it.
        static constexpr int64_t kDefaultClientIdleNs = 3LL * 24 * 3600 * 1000 * 1000 * 1000;
        /// Bodies a worker forwards as they arrive at once, each holding a venue connection.
        static constexpr size_t kDefaultStreamBodyMax = 256;
        /// A streamed body gets the grace, and a second more for each kDefaultBodyMinRate
        /// bytes it delivers; a total limit is off. GATEWAY-INTERNALS.md 12 says why.
        static constexpr int64_t kDefaultBodyTimeoutNs = 0;
        static constexpr int64_t kDefaultBodyGraceNs = 10LL * 1000 * 1000 * 1000; // 10 s
        static constexpr uint64_t kDefaultBodyMinRate = 1024; // bytes a second

        /// `policy` is non-owning and must outlive the Gateway; null means none.
        Gateway(uint16_t listen_port, std::string upstream_ip, uint16_t upstream_port,
                int64_t warmup_ns = 0, UpstreamDialect dialect = UpstreamDialect::OpenAI,
                IoBackend io = IoBackend::Auto,
                int64_t upstream_idle_ns = kDefaultUpstreamIdleNs,
                TlsConfig tls = {}, bool timing_headers = false,
                Policy* policy = nullptr, std::vector<std::string> strip_headers = {});
        /// Several upstreams, chosen per request by the policy. Not empty, and fixed for
        /// the process lifetime. `tls` here is the inbound leg only. A venue may be given
        /// by `host` alone: run() resolves it off the loop, and it is refused until then.
        Gateway(uint16_t listen_port, std::vector<Upstream> upstreams, int64_t warmup_ns,
                IoBackend io, int64_t upstream_idle_ns, TlsConfig tls, bool timing_headers,
                Policy* policy, std::vector<std::string> strip_headers);
        /// Never waits on a name lookup in flight, which finishes on its own thread.
        ~Gateway();
        Gateway(const Gateway&) = delete;
        Gateway& operator=(const Gateway&) = delete;

        /// Run the event loop until request_stop() is called. Returns 0, or 1 at once when
        /// no loop could start: IoBackend::Uring where io_uring is unavailable (it never
        /// falls back to epoll), or no epoll instance.
        int run();
        /// Flips a flag the loop observes within one poll tick; safe from any thread.
        void request_stop() noexcept;
        /// Print this worker's histograms on its next sweep; safe from any thread.
        void request_stats_dump() noexcept;
        /// Safe from the owning worker, or from any thread once it has been joined.
        void print_profile(std::FILE* out, const char* title) const noexcept;
        [[nodiscard]] const Stats& stats() const noexcept;
        /// The configured backend (the requested mode; Auto stays Auto).
        [[nodiscard]] IoBackend io_backend() const noexcept;
        /// The bound listen port (resolves an ephemeral 0).
        [[nodiscard]] uint16_t bound_port() const noexcept;

        // Test seams and operational dials; call before run(). 0 disables pool reaping
        // and the heartbeat respectively.
        void set_client_setup_ns(int64_t ns) noexcept;
        void set_client_idle_ns(int64_t ns) noexcept;
        void set_pool_idle_ns(int64_t ns) noexcept;
        void set_connect_ns(int64_t ns) noexcept;
        void set_heartbeat_ns(int64_t ns) noexcept;
        /// Bytes to reserve and touch in the scratch buffers at run(), so the first
        /// requests do not fault pages in. 0 prefaults nothing.
        void set_prefault_bytes(size_t n) noexcept;
        /// Forward an OpenAI client's body as it arrives, each chunk checked first (default on).
        void set_stream_openai_body(bool on) noexcept;
        void set_stream_body(bool on) noexcept; // any client's; off holds every body (default on)
        void set_stream_body_max(size_t n) noexcept; // past it a body is held; 0 = no cap
        void set_body_timeout_ns(int64_t ns) noexcept; // a streamed body's whole upload; 0 = off
        void set_body_grace_ns(int64_t ns) noexcept;   // before the minimum rate applies
        void set_body_min_rate(uint64_t bytes_per_s) noexcept; // averaged past the grace; 0 = off
        /// Parse storage each worker keeps between bodies (default 4 MiB). Process-wide.
        void set_retain_bytes(size_t n) noexcept;
        /// Optional per-request sink (sink.hpp), non-owning. `capture` names up to
        /// kSinkCaptureMax request headers copied into RequestRecord::captured.
        void set_request_sink(RequestSink* sink, std::vector<std::string> capture);

        /// Does any pooled upstream still hold `needle`? Yes or no only: a
        /// credential-adjacent accessor never returns or logs buffer contents.
        [[nodiscard]] bool pooled_buffer_contains(std::string_view needle) const noexcept;
        /// Loop-thread state, like stats(): valid once that thread has been joined.
        [[nodiscard]] size_t pooled_upstream_count() const noexcept;
        void set_pool_cap_for_test(size_t n) noexcept; // before run()
        void set_stream_body_min_for_test(size_t n) noexcept; // smallest streamed body, 16 KiB
        /// The SNI/verify host recorded for upstream `i`, or empty.
        [[nodiscard]] std::string_view upstream_sni_host(size_t i) const noexcept;
        /// The io_uring provided-buffer count (a power of two), 0 for the default.
        void set_uring_buf_count_for_test(unsigned n) noexcept;
        /// SO_SNDBUF for accepted clients, 0 for the kernel's: a fixed value turns off
        /// the autotuning that can absorb a whole stream on loopback.
        void set_client_sndbuf_for_test(int bytes) noexcept;
        /// SO_SNDBUF for upstream connections, 0 for the kernel's: a venue's writes back up.
        void set_upstream_sndbuf_for_test(int bytes) noexcept;
        /// Epoll only: at most `bytes` per socket write pass, then it behaves as EAGAIN.
        void set_epoll_write_cap_for_test(size_t bytes) noexcept;
        /// Prefault the scratch buffers now and report how much of one is resident.
        size_t prefault_resident_bytes_for_test();
        /// Buffer rotation, callable from tests.
        void retire_wbuf(UpstreamConn* u) noexcept;
        void adopt_warm(UpstreamConn* u) noexcept;
        void note_build(size_t need) noexcept; // counts a build the scratch could not hold
        [[nodiscard]] size_t warm_bufs_for_test() const noexcept;

    private:
        friend struct GatewayCoreTest; // gateway/tests/core_test.cpp
        std::unique_ptr<detail::Engine> _e;
    };
} // namespace llmbridge
