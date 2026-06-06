#pragma once

// Gateway — the whole Kottos proxy in one self-contained class. A single-
// threaded, non-blocking epoll event loop: accept clients, frame requests,
// (optionally) translate the provider dialect, forward over a keep-alive
// upstream pool, read the response, translate back, write to the client. No
// framework, no external dependencies — just net (sockets + HTTP framing) and
// provider (dialect translation).
//
// Per-request added latency (the headline metric) is four wall-clock stamps:
//   added = (upstream_request_sent - client_request_received)
//         + (client_response_sent  - upstream_response_received)
// i.e. request-path + response-path work, with the upstream wait excluded.

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "gateway/metrics.hpp"
#include "net/http.hpp"
#include "provider/translate.hpp"

namespace kottos
{
    // Dialect translation: None = byte-forward (OpenAI-compatible upstreams);
    // the rest translate OpenAI<->provider on the way out and back.
    enum class TranslateMode
    {
        None,
        Anthropic,
        Gemini,
        Cohere,
    };

    // Per-fd connection state (client or upstream).
    struct Connection
    {
        int fd = -1;
        bool is_client = true;
        bool write_armed = false;     // EPOLLOUT currently registered
        bool connected = false;       // upstream-only: non-blocking connect done
        bool request_pending = false; // client-only: full request buffered, awaiting forward
        bool doomed = false;          // closed this epoll batch; deleted after the batch

        uint64_t id = 0; // client conns: stable id; upstream conns: 0

        std::string rbuf;
        std::string wbuf;
        size_t woff = 0;

        Connection* peer = nullptr; // linked counterpart for the in-flight request
        http::Message msg{};

        // Latency stamps (ns), held on the CLIENT conn for the active request.
        int64_t ts_req_recvd = 0;
        int64_t ts_up_sent = 0;
        int64_t ts_up_recvd = 0;
    };

    struct Stats
    {
        Histogram overhead;  // total proxy-added latency per request
        Histogram req_path;  // client-recv -> upstream-sent
        Histogram resp_path; // upstream-recv -> client-sent
        uint64_t requests = 0;
        uint64_t errors = 0;
        uint64_t upstream_conns_opened = 0;
    };

    class Gateway
    {
    public:
        Gateway(uint16_t listen_port, std::string upstream_ip, uint16_t upstream_port,
                int64_t warmup_ns = 0, TranslateMode translate = TranslateMode::None);
        ~Gateway();

        Gateway(const Gateway&) = delete;
        Gateway& operator=(const Gateway&) = delete;

        // Run the event loop until request_stop() is called. Returns 0.
        int run();

        // Async-signal-safe-ish: flips a flag the loop observes within one poll tick.
        void request_stop() noexcept { _stop = true; }

        const Stats& stats() const noexcept { return _stats; }

        // Actual bound listen port (resolves ephemeral when 0 was requested).
        uint16_t bound_port() const noexcept;

    private:
        void ep_add_read(Connection* c) noexcept;
        void ep_arm_write(Connection* c) noexcept;
        void ep_disarm_write(Connection* c) noexcept;

        void on_accept() noexcept;
        void on_client_readable(Connection* c) noexcept;
        void on_client_writable(Connection* c) noexcept;
        void on_upstream_writable(Connection* c) noexcept;
        void on_upstream_readable(Connection* c) noexcept;

        // Forward the client's buffered (and optionally translated) request to an
        // upstream connection. Called inline once a full request is framed.
        void forward(Connection* client) noexcept;
        // Write the buffered response to the client; close out accounting.
        void respond(Connection* client) noexcept;
        void finish_client_response(Connection* c) noexcept;

        Connection* acquire_upstream() noexcept;
        void release_upstream(Connection* u) noexcept;
        void close_client(Connection* c) noexcept;
        void close_upstream(Connection* u) noexcept;
        void abort_pair(Connection* client) noexcept;

        bool drain_read(Connection* c) noexcept;
        bool pump_write(Connection* c, bool* done) noexcept;
        Connection* client_by_id(uint64_t id) noexcept;

        uint16_t _listen_port;
        std::string _upstream_ip;
        uint16_t _upstream_port;
        int64_t _warmup_ns;
        TranslateMode _translate;

        int _epfd = -1;
        int _listen_fd = -1;
        Connection* _listen_conn = nullptr;

        std::unordered_map<uint64_t, Connection*> _clients;
        std::vector<Connection*> _idle_upstreams;
        uint64_t _next_client_id = 1;
        std::vector<Connection*> _doomed; // closed mid-batch, freed after the batch

        int64_t _t_start = 0;
        Stats _stats;
        volatile bool _stop = false;
    };
} // namespace kottos
