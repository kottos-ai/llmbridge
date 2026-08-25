// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

// Concurrency + fidelity regression: 100 clients x 10 questions through one
// gateway, NON-STREAMING and streamed, on both event-loop backends.
//
// What this proves that the other gateway tests do not:
//
//   1. Response correlation under A shared pool. Every request carries a different
//      question and expects a different answer, and the mock provider answers by
//      Looking the question up instead of replying with a canned body. So if the
//      gateway ever hands client A the bytes meant for client B, the failure mode
//      a framing desync produces, and the reason a shared upstream pool makes
//      framing bugs cross-client; this test says so by name, instead of passing
//      because both clients expected the same string. A canned-response test is
//      structurally blind to that. Question uniqueness is asserted, not assumed.
//
//   2. Text fidelity on real model output. The corpus is recorded Claude answers
//      (scripts/gen_qa_corpus.py, recorded once, replayed here; the test itself
//      never touches the network). Each entry carries a `kind`:
//
//        plain          ordinary prose
//        escape_stress  answers forced to contain JSON-hostile characters
//        json_hostile   answers about JSON escaping, so the text is itself full of
//                       backslashes, quotes and \uXXXX the model wrote literally.
//                       Note the limit of this: asked directly about \u0000 and raw
//                       control characters, the model describes them. 68 answers
//                       carry a literal backslash-u sequence, none carries a raw
//                       control byte. Recorded output cannot cover that byte class,
//                       which is where the control-character defect lived, so it is covered
//                       deterministically by ControlBytesAreRoundTripped... below.
//        tricky_text    RTL, ZWJ emoji, combining marks, astral planes, CJK, Thai
//        long           multi-paragraph answers
//        backend_stress deliberately large answers, see (3)
//
//      The fixture is curated, not merely sampled: 2000 pairs are generated, answers
//      are capped at 6144 bytes (records that hit the cap carry "truncated": true),
//      and the best 1000 are kept by character-class coverage. Curating beats
//      generating 1000 directly because the selector can then guarantee every rare
//      class survives at 100%: tab 18/18, astral 97/97, literal-\u 68/68,
//      backslash 224/224, while the abundant ones (newline, non-ascii) halve
//      harmlessly. A proportional sample does not do this: a naive stride-2 halving
//      of the same 2000 drops `tab` to zero.
//
//      1000 is also exactly kTotal, so pick() strides by 1 and every entry is used
//      by the concurrency runs. At 2000 the stride was 2 and half the corpus was
//      never seen by them at all.
//
//      Reproduce with:
//        gen_qa_corpus.py --out ... -n 2000 --max-answer-bytes 6144 --curate 1000
//
//      Every answer must survive Anthropic-JSON -> parse -> OpenAI-JSON
//      re-serialisation byte for byte. This is how the RFC 8259 control-character
//      defect fixed in 0.9.0 was found: the parser accepted raw control bytes and
//      passed them straight through into a 200 OK that no strict client can read.
//
//   3. Both backends, including where they differ. epoll and io_uring implement
//      this path independently (DESIGN.md, "Naming conventions"), so a fix to one
//      is not a fix to the other and every case here runs twice. The
//      backend_stress answers exist because io_uring reassembles reads across a
//      provided-buffer ring of kUrBufSize = 4096 while epoll grows a single buffer,
//      and on the streaming path io_uring accumulates into `wpending` where epoll
//      pauses reads, so answers past a few KB exercise code that is not shared.
//
//   4. Streaming as well as not. The streamed cases drive real SSE: the provider
//      emits the Anthropic event envelope one HTTP chunk per event, the gateway
//      translates it to OpenAI chunks, and the client reassembles the answer from
//      the deltas. Note the framing changes across the gateway: the upstream leg
//      is chunked, the client leg is close-delimited.
//
// Latency is reported per mode, because the meaningful number differs: a single
// round-trip for non-streaming, and TTFT (what a voice agent feels) plus stream
// completion for streaming. Both assert only a very loose ceiling. A tight
// wall-clock bound on a shared CI runner is a flake generator, and a flaky test is
// one people learn to ignore, the same reasoning that replaced a timing assertion
// in the chunked-decode regression with a deterministic one. Use the printed
// numbers for tracking; the pass/fail signal here is correctness.
//
// The TTFT figures are not a provider-latency claim: the mock replies instantly, so
// they measure the gateway plus 1000 concurrent connection setups, nothing else.

#include "gateway/gateway.hpp"
#include "net/http.hpp"
#include "provider/json.hpp"

#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

using llmbridge::Gateway;
using llmbridge::IoBackend;
using llmbridge::UpstreamDialect;
namespace json = llmbridge::provider::json;
namespace http = llmbridge::net::http;

namespace
{
    constexpr int kClients = 100;
    constexpr int kPerClient = 10;
    constexpr int kTotal = kClients * kPerClient; // 1000

    struct QA
    {
        std::string q;
        std::string a;
        std::string kind; // plain | escape_stress | json_hostile | tricky_text
                          // | long | backend_stress  (see scripts/gen_qa_corpus.py)
    };

    // ---------------------------------------------------------------- corpus
    // One JSON object per line: {"id":N,"topic":..,"q":..,"a":..,"kind":".."}
    // Parsed with the project's own parser on purpose; it is the parser that has
    // to survive these strings in production.
    const std::vector<QA>& corpus()
    {
        static const std::vector<QA> loaded = [] {
            std::vector<QA> v;
            const std::string path = std::string(LLMBRIDGE_TEST_DATA_DIR) + "/qa_corpus.jsonl";
            std::ifstream in(path);
            EXPECT_TRUE(in.good()) << "cannot open corpus: " << path
                                   << " (regenerate with scripts/gen_qa_corpus.py)";
            std::string line;
            while (std::getline(in, line))
            {
                if (line.empty()) continue;
                bool ok = false;
                const json::Value rec = json::parse(line, ok);
                if (!ok || !rec.is_object()) continue;
                const json::Value* q = rec.find("q");
                const json::Value* a = rec.find("a");
                const json::Value* kind = rec.find("kind");
                if (!q || !a || !q->is_string() || !a->is_string()) continue;
                v.push_back({json::unescape_string(q->sv), json::unescape_string(a->sv),
                             kind && kind->is_string() ? std::string(kind->sv) : "plain"});
            }
            return v;
        }();
        return loaded;
    }

    // ---------------------------------------------------------- mock provider
    // Answers by looking up the question in the request. That lookup is the whole
    // point: a canned reply cannot distinguish "right answer" from "some answer".
    class CorpusBackend
    {
    public:
        void start(const std::unordered_map<std::string, std::string>* answers)
        {
            _answers = answers;
            _fd = ::socket(AF_INET, SOCK_STREAM, 0);
            int one = 1;
            ::setsockopt(_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
            sockaddr_in a{};
            a.sin_family = AF_INET;
            a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            a.sin_port = 0;
            ::bind(_fd, reinterpret_cast<sockaddr*>(&a), sizeof a);
            socklen_t len = sizeof a;
            ::getsockname(_fd, reinterpret_cast<sockaddr*>(&a), &len);
            _port = ntohs(a.sin_port);
            // Backlog well above the highest concurrency the sweep offers. A mock
            // backlog below the client count throttles the gateway and produces a
            // plausible wrong number, a documented hazard in this repo
            // (bench/BENCHMARK-CONFIG.md), not a hypothetical one.
            ::listen(_fd, 4096);
            _acc = std::thread([this] { accept_loop(); });
        }

        void stop()
        {
            if (_fd < 0) return;
            ::shutdown(_fd, SHUT_RDWR);
            ::close(_fd);
            _fd = -1;
            if (_acc.joinable()) _acc.join();
            {   // the gateway pools upstream conns, so handler threads sit in read()
                std::lock_guard<std::mutex> lk(_mu);
                for (int fd : _fds) ::shutdown(fd, SHUT_RDWR);
            }
            for (auto& t : _conns)
                if (t.joinable()) t.join();
        }
        ~CorpusBackend() { stop(); }

        uint16_t port() const { return _port; }
        // Fault injection, used only by the negative-control tests below: reply to
        // every question with this text instead of the correct answer. Simulates a
        // pooled-connection desync handing one client another's response.
        void set_wrong_answer(std::string s) { _wrong = std::move(s); }
        // Emit the answer text into the JSON body without escaping it, simulating an
        // escaping regression in the translator.
        void set_skip_escaping(bool b) { _skip_escape = b; }
        int served() const { return _served.load(std::memory_order_relaxed); }
        int unknown_questions() const { return _unknown.load(std::memory_order_relaxed); }
        int malformed() const { return _malformed.load(std::memory_order_relaxed); }

    private:
        void accept_loop()
        {
            for (;;)
            {
                const int fd = ::accept(_fd, nullptr, nullptr);
                if (fd < 0) return;
                int one = 1;
                ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
                {
                    std::lock_guard<std::mutex> lk(_mu);
                    _fds.push_back(fd);
                }
                _conns.emplace_back([this, fd] { serve(fd); });
            }
        }

        // Pull the user question out of the Anthropic request the gateway built.
        static std::string question_of(std::string_view body)
        {
            bool ok = false;
            const json::Value root = json::parse(body, ok);
            if (!ok) return {};
            const json::Value* msgs = root.find("messages");
            if (!msgs || !msgs->is_array() || msgs->arr.empty()) return {};
            const json::Value& last = msgs->arr.back();
            const json::Value* c = last.find("content");
            if (!c) return {};
            if (c->is_string()) return json::unescape_string(c->sv);
            if (c->is_array() && !c->arr.empty()) // [{type:text,text:...}]
            {
                const json::Value* t = c->arr.front().find("text");
                if (t && t->is_string()) return json::unescape_string(t->sv);
            }
            return {};
        }

        // Split on UTF-8 character boundaries: a delta is a JSON string, so cutting
        // mid-sequence would emit invalid UTF-8, which is not the property under test, and
        // rejected by the parser anyway since the RFC 8259 tightening.
        static std::vector<std::string> utf8_chunks(std::string_view sv, size_t target)
        {
            std::vector<std::string> out;
            size_t i = 0;
            while (i < sv.size())
            {
                size_t end = std::min(i + target, sv.size());
                while (end < sv.size() && (static_cast<unsigned char>(sv[end]) & 0xC0) == 0x80) ++end;
                out.emplace_back(sv.substr(i, end - i));
                i = end;
            }
            if (out.empty()) out.emplace_back();
            return out;
        }

        static bool wants_stream(std::string_view body)
        {
            bool ok = false;
            const json::Value v = json::parse(body, ok);
            if (!ok) return false;
            const json::Value* st = v.find("stream");
            return st && st->type == json::Value::Type::Bool && st->boolean;
        }

        // The Anthropic streaming envelope carrying the answer as N text deltas,
        // framed one HTTP chunk per SSE event, what a provider actually does, and
        // what makes the gateway's incremental decode do real work.
        std::string anthropic_sse(std::string_view answer) const
        {
            std::string ev =
                "event: message_start\ndata: {\"type\":\"message_start\",\"message\":"
                "{\"id\":\"msg_corpus\",\"model\":\"claude-x\","
                "\"usage\":{\"input_tokens\":7,\"output_tokens\":0}}}\n\n"
                "event: content_block_start\ndata: {\"type\":\"content_block_start\","
                "\"index\":0,\"content_block\":{\"type\":\"text\",\"text\":\"\"}}\n\n";
            for (const std::string& piece : utf8_chunks(answer, _delta_bytes))
            {
                ev += "event: content_block_delta\ndata: {\"type\":\"content_block_delta\","
                      "\"index\":0,\"delta\":{\"type\":\"text_delta\",\"text\":";
                if (_skip_escape) { ev += '"'; ev += piece; ev += '"'; }
                else json::append_escaped_string(ev, piece);
                ev += "}}\n\n";
            }
            ev += "event: content_block_stop\ndata: {\"type\":\"content_block_stop\","
                  "\"index\":0}\n\n"
                  "event: message_delta\ndata: {\"type\":\"message_delta\","
                  "\"delta\":{\"stop_reason\":\"end_turn\"},"
                  "\"usage\":{\"output_tokens\":11}}\n\n"
                  "event: message_stop\ndata: {\"type\":\"message_stop\"}\n\n";

            std::string out = "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
                              "Cache-Control: no-cache\r\nTransfer-Encoding: chunked\r\n"
                              "Connection: keep-alive\r\n\r\n";
            size_t pos = 0;
            while (pos < ev.size())
            {
                const size_t nl = ev.find("\n\n", pos);
                const size_t end = (nl == std::string::npos) ? ev.size() : nl + 2;
                const std::string piece = ev.substr(pos, end - pos);
                char hdr[32];
                std::snprintf(hdr, sizeof hdr, "%zx\r\n", piece.size());
                out += hdr;
                out += piece;
                out += "\r\n";
                pos = end;
            }
            out += "0\r\n\r\n";
            return out;
        }

        std::string anthropic_reply(std::string_view answer) const
        {
            std::string body =
                R"({"id":"msg_corpus","type":"message","role":"assistant","model":"claude-x",)"
                R"("content":[{"type":"text","text":)";
            if (_skip_escape) { body += '"'; body.append(answer); body += '"'; }
            else json::append_escaped_string(body, answer);
            body += R"(}],"stop_reason":"end_turn",)"
                    R"("usage":{"input_tokens":7,"output_tokens":11}})";
            std::string out = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n";
            out += "Content-Length: " + std::to_string(body.size()) + "\r\n";
            out += "Connection: keep-alive\r\n\r\n";
            out += body;
            return out;
        }

        void serve(int fd)
        {
            std::string buf;
            char tmp[16384];
            for (;;)
            {
                http::Message m;
                while (http::parse_request(buf, m) != http::FrameStatus::Complete)
                {
                    const ssize_t n = ::read(fd, tmp, sizeof tmp);
                    if (n <= 0) return;
                    buf.append(tmp, static_cast<size_t>(n));
                }
                const std::string body = buf.substr(m.header_len, m.body_len);
                buf.erase(0, m.total_len);

                const std::string q = question_of(body);
                std::string answer;
                if (q.empty())
                {
                    _malformed.fetch_add(1, std::memory_order_relaxed);
                    answer = "((MALFORMED REQUEST))";
                }
                else
                {
                    const auto it = _answers->find(q);
                    if (it == _answers->end())
                    {
                        _unknown.fetch_add(1, std::memory_order_relaxed);
                        answer = "((UNKNOWN QUESTION))";
                    }
                    else
                    {
                        answer = it->second;
                    }
                }
                if (!_wrong.empty()) answer = _wrong; // negative control
                const std::string reply =
                    wants_stream(body) ? anthropic_sse(answer) : anthropic_reply(answer);
                size_t off = 0;
                while (off < reply.size())
                {
                    const ssize_t w = ::write(fd, reply.data() + off, reply.size() - off);
                    if (w <= 0) return;
                    off += static_cast<size_t>(w);
                }
                _served.fetch_add(1, std::memory_order_relaxed);
            }
        }

        const std::unordered_map<std::string, std::string>* _answers = nullptr;
        // Written by stop() on one thread, read by the accept loop on another.
        // A plain int here is a real data race, and TSan reported it. Same fix as
        // TlsBackend::_lfd and PlainBackend::_fd in gateway_tls_test.cpp.
        std::atomic<int> _fd{-1};
        uint16_t _port = 0;
        std::thread _acc;
        std::vector<std::thread> _conns;
        std::vector<int> _fds;
        std::mutex _mu;
        std::string _wrong;
        bool _skip_escape = false;
        size_t _delta_bytes = 24; // answer split into deltas of ~this many bytes
        std::atomic<int> _served{0}, _unknown{0}, _malformed{0};
    };

    // ------------------------------------------------------------ test client
    std::string openai_request(std::string_view question, uint16_t port)
    {
        std::string body = R"({"model":"gpt-4o-mini","messages":[{"role":"user","content":)";
        json::append_escaped_string(body, question);
        body += R"(}]})";
        std::string req = "POST /v1/chat/completions HTTP/1.1\r\n";
        req += "Host: 127.0.0.1:" + std::to_string(port) + "\r\n";
        req += "Content-Type: application/json\r\n";
        req += "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n";
        req += body;
        return req;
    }

    std::string openai_stream_request(std::string_view question, uint16_t port)
    {
        std::string body = R"({"model":"gpt-4o-mini","stream":true,"messages":[{"role":"user","content":)";
        json::append_escaped_string(body, question);
        body += R"(}]})";
        std::string req = "POST /v1/chat/completions HTTP/1.1\r\n";
        req += "Host: 127.0.0.1:" + std::to_string(port) + "\r\n";
        req += "Content-Type: application/json\r\n";
        req += "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n";
        req += body;
        return req;
    }

    // What a streaming client actually observes.
    struct StreamOutcome
    {
        bool ok = false;
        bool saw_done = false;
        std::string content;   // reassembled from every delta
        double ttft_ms = 0;    // to the first content delta: the number voice agents feel
        double total_ms = 0;   // to [DONE]
        int deltas = 0;
    };

    // delta.content out of one OpenAI stream chunk; empty if this chunk carries none
    // (the role chunk, the finish chunk and the usage chunk all legitimately do not).
    std::string delta_of(std::string_view data)
    {
        bool ok = false;
        const json::Value root = json::parse(data, ok);
        if (!ok) return {};
        const json::Value* ch = root.find("choices");
        if (!ch || !ch->is_array() || ch->arr.empty()) return {};
        const json::Value* d = ch->arr.front().find("delta");
        if (!d) return {};
        const json::Value* c = d->find("content");
        if (!c || !c->is_string()) return {};
        return json::unescape_string(c->sv);
    }

    // choices[0].message.content, unescaped. Empty on any structural surprise.
    std::string answer_of(std::string_view http_response)
    {
        const size_t hdr = http_response.find("\r\n\r\n");
        if (hdr == std::string_view::npos) return {};
        bool ok = false;
        const json::Value root = json::parse(http_response.substr(hdr + 4), ok);
        if (!ok) return {};
        const json::Value* ch = root.find("choices");
        if (!ch || !ch->is_array() || ch->arr.empty()) return {};
        const json::Value* msg = ch->arr.front().find("message");
        if (!msg) return {};
        const json::Value* content = msg->find("content");
        if (!content || !content->is_string()) return {};
        return json::unescape_string(content->sv);
    }

    struct Client
    {
        int fd = -1;
        std::string buf;

        bool connect(uint16_t port)
        {
            fd = ::socket(AF_INET, SOCK_STREAM, 0);
            if (fd < 0) return false;
            int one = 1;
            ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
            sockaddr_in a{};
            a.sin_family = AF_INET;
            a.sin_port = htons(port);
            a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            return ::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) == 0;
        }
        bool send_all(const std::string& s)
        {
            size_t off = 0;
            while (off < s.size())
            {
                const ssize_t w = ::write(fd, s.data() + off, s.size() - off);
                if (w <= 0) return false;
                off += static_cast<size_t>(w);
            }
            return true;
        }
        // Read exactly one framed response off the keep-alive connection.
        std::string recv_one()
        {
            char tmp[16384];
            for (;;)
            {
                http::ResponseDecoder dec;
                const http::ParsedResponse r = http::parse_response(buf, dec);
                if (r.status == http::FrameStatus::Error) return {};
                if (r.status == http::FrameStatus::Complete)
                {
                    std::string out = buf.substr(0, r.total_len);
                    buf.erase(0, r.total_len);
                    return out;
                }
                const ssize_t n = ::read(fd, tmp, sizeof tmp);
                if (n <= 0) return {};
                buf.append(tmp, static_cast<size_t>(n));
            }
        }
        // Drive one streaming request end to end, timestamping the first content
        // delta.
        //
        // Note the framing: the gateway's client-facing SSE is CLOSE-DELIMITED
        // ("Connection: close", no Content-Length and no Transfer-Encoding). The
        // upstream leg is chunked, the client leg is not. An earlier version of this
        // helper ran a ChunkDecoder over the client bytes and every single stream
        // failed identically, which is the signature of a framing assumption rather
        // than a bug in the thing under test.
        StreamOutcome stream_once(const std::string& request)
        {
            StreamOutcome r;
            const auto t0 = std::chrono::steady_clock::now();
            if (!send_all(request)) return r;

            http::ResponseHead head{};
            bool have_head = false;
            size_t cursor = 0; // index into buf of the next unparsed SSE byte
            char tmp[16384];

            for (;;)
            {
                if (!have_head &&
                    http::parse_response_head(buf, head) == http::FrameStatus::Complete)
                {
                    if (head.status != 200) return r;
                    have_head = true;
                    cursor = head.header_len;
                }
                if (have_head)
                {
                    size_t nl;
                    while ((nl = buf.find("\n\n", cursor)) != std::string::npos)
                    {
                        const std::string ev = buf.substr(cursor, nl - cursor);
                        cursor = nl + 2;
                        const size_t dp = ev.find("data:");
                        if (dp == std::string::npos) continue;
                        std::string data = ev.substr(dp + 5);
                        while (!data.empty() && data.front() == ' ') data.erase(0, 1);
                        while (!data.empty() && (data.back() == '\n' || data.back() == '\r'))
                            data.pop_back();
                        if (data == "[DONE]")
                        {
                            r.saw_done = true;
                            r.ok = true;
                            r.total_ms = std::chrono::duration<double, std::milli>(
                                             std::chrono::steady_clock::now() - t0).count();
                            return r;
                        }
                        const std::string piece = delta_of(data);
                        if (!piece.empty())
                        {
                            if (r.deltas == 0)
                                r.ttft_ms = std::chrono::duration<double, std::milli>(
                                                std::chrono::steady_clock::now() - t0).count();
                            ++r.deltas;
                            r.content += piece;
                        }
                    }
                }
                const ssize_t n = ::read(fd, tmp, sizeof tmp);
                if (n <= 0) break; // close-delimited: EOF ends the stream
                buf.append(tmp, static_cast<size_t>(n));
            }
            r.total_ms = std::chrono::duration<double, std::milli>(
                             std::chrono::steady_clock::now() - t0).count();
            return r;
        }

        void close()
        {
            if (fd >= 0) { ::close(fd); fd = -1; }
        }
        ~Client() { close(); }
    };

    // Percentiles from an unsorted sample. Sorting in place is fine; callers are
    // done with the ordering by the time they report.
    struct Lat
    {
        std::vector<double> v;
        void add(double ms) { v.push_back(ms); }
        double pct(double p)
        {
            if (v.empty()) return 0;
            std::sort(v.begin(), v.end());
            return v[static_cast<size_t>(p * (v.size() - 1))];
        }
        double max() { return v.empty() ? 0 : *std::max_element(v.begin(), v.end()); }
        size_t size() const { return v.size(); }
    };

    // The corpus is larger than one concurrency run (2000 pairs, 1000 requests), so
    // walk it with a stride instead of taking the first kTotal; otherwise the run
    // would only ever see the plain-prose half and never the long, json_hostile,
    // tricky_text or backend_stress entries. Distinct i -> distinct index -> distinct
    // question, which is what keeps mis-correlation detectable.
    const QA& pick(int i)
    {
        const size_t n = corpus().size();
        const size_t stride = n / static_cast<size_t>(kTotal);
        return corpus()[(static_cast<size_t>(i) * (stride ? stride : 1)) % n];
    }

    // The gateway's own added-latency histogram tops out at ~2.62 ms, and
    // Histogram::percentile() returns the running max once the target lands in the
    // overflow region, so a saturated histogram silently reports a number that
    // looks like a percentile and is not one. That exact artifact has produced a
    // wrong result in this project before, so refuse to print one.
    //
    // It saturates here for io_uring under concurrency because the uring stamps
    // bracket a submitted send and its completion, so the interval includes time
    // the SQE spent queued; the epoll path stamps around an inline write() and
    // measures only compute. The two are therefore not comparable under load.
    // Compare them in the sequential control instead, where both read ~45 us.
    std::string added_latency(const llmbridge::Histogram& h)
    {
        const uint64_t n = h.total(), of = h.overflow_count();
        char buf[160];
        if (n == 0) return "n/a";
        if (of * 20 > n) // >5% overflow: percentiles are not trustworthy
        {
            std::snprintf(buf, sizeof buf, "unresolvable (%llu/%llu over %.2f ms cap)",
                          (unsigned long long)of, (unsigned long long)n,
                          h.max_tracked_ns() / 1e6);
            return buf;
        }
        std::snprintf(buf, sizeof buf, "p50 %.0f us  p99 %.0f us",
                      h.percentile(0.50) / 1000.0, h.percentile(0.99) / 1000.0);
        return buf;
    }

    struct Failure
    {
        int client = 0, index = 0;
        std::string question, expected, got;
    };

    // --------------------------------------------------------------- fixture
    class CorpusIT : public ::testing::TestWithParam<IoBackend>
    {
    protected:
        void SetUp() override
        {
            ASSERT_GE(corpus().size(), static_cast<size_t>(kTotal))
                << "corpus has " << corpus().size() << " pairs, need " << kTotal;
            for (const QA& qa : corpus()) _answers[qa.q] = qa.a;
            // Distinct questions are what make mis-correlation detectable at all:
            // if two clients asked the same thing, swapping their replies would go
            // unnoticed. Assert the property instead of assume the generator held it.
            ASSERT_EQ(_answers.size(), corpus().size())
                << "corpus contains duplicate questions; the correlation check would be blind";
        }
        void TearDown() override
        {
            if (_gw) _gw->request_stop();
            if (_gt.joinable()) _gt.join();
            _backend.stop();
        }

        void start()
        {
            _backend.start(&_answers);
            _gw = std::make_unique<Gateway>(0, "127.0.0.1", _backend.port(), /*warmup*/ 0,
                                            UpstreamDialect::Anthropic, GetParam(),
                                            Gateway::kDefaultUpstreamIdleNs,
                                            llmbridge::TlsConfig{}, /*timing_headers*/ false);
            _port = _gw->bound_port();
            _gt = std::thread([this] { _gw->run(); });
        }

        CorpusBackend _backend;
        std::unordered_map<std::string, std::string> _answers;
        std::unique_ptr<Gateway> _gw;
        std::thread _gt;
        uint16_t _port = 0;
    };
} // namespace

// Every client gets a disjoint slice of the corpus, so all 1000 questions are asked
// exactly once and every expected answer is unique to one in-flight request.
TEST_P(CorpusIT, ThousandQuestionsAcrossHundredClients)
{
    start();

    std::vector<std::vector<double>> lat(kClients);
    std::vector<std::vector<Failure>> fails(kClients);
    std::atomic<int> connect_fail{0}, empty_resp{0}, non_200{0};

    const auto t0 = std::chrono::steady_clock::now();
    std::vector<std::thread> threads;
    threads.reserve(kClients);
    for (int c = 0; c < kClients; ++c)
    {
        threads.emplace_back([&, c] {
            Client cl;
            if (!cl.connect(_port)) { connect_fail.fetch_add(1); return; }
            lat[c].reserve(kPerClient);
            for (int k = 0; k < kPerClient; ++k)
            {
                const QA& qa = pick(c * kPerClient + k);
                const auto s = std::chrono::steady_clock::now();
                if (!cl.send_all(openai_request(qa.q, _port))) { empty_resp.fetch_add(1); return; }
                const std::string resp = cl.recv_one();
                const auto e = std::chrono::steady_clock::now();
                if (resp.empty()) { empty_resp.fetch_add(1); return; }
                lat[c].push_back(std::chrono::duration<double, std::milli>(e - s).count());
                if (resp.find("200 OK") == std::string::npos)
                {
                    non_200.fetch_add(1);
                    fails[c].push_back({c, k, qa.q, qa.a, resp.substr(0, 64)});
                    continue;
                }
                const std::string got = answer_of(resp);
                if (got != qa.a) fails[c].push_back({c, k, qa.q, qa.a, got});
            }
            cl.close();
        });
    }
    for (auto& t : threads) t.join();
    const double wall = std::chrono::duration<double, std::milli>(
                            std::chrono::steady_clock::now() - t0).count();

    // ---- correctness (the real signal) ----------------------------------
    EXPECT_EQ(connect_fail.load(), 0);
    EXPECT_EQ(empty_resp.load(), 0);
    EXPECT_EQ(non_200.load(), 0);
    EXPECT_EQ(_backend.malformed(), 0) << "the gateway sent the provider an unparseable body";
    EXPECT_EQ(_backend.unknown_questions(), 0)
        << "the provider received a question that is not in the corpus; the gateway "
           "corrupted or swapped a request body";

    std::vector<Failure> all;
    for (auto& f : fails) all.insert(all.end(), f.begin(), f.end());
    EXPECT_TRUE(all.empty()) << all.size() << " of " << kTotal << " answers were wrong";

    // Name the failure mode explicitly: a wrong answer that is another question's
    // answer is cross-client response mixing, which is far more serious than
    // corruption and has a different cause (pooled-connection desync).
    int swapped = 0;
    for (const Failure& f : all)
    {
        for (size_t i = 0; i < corpus().size() && !swapped; ++i)
            if (corpus()[i].a == f.got && corpus()[i].q != f.question) ++swapped;
    }
    EXPECT_EQ(swapped, 0) << "RESPONSE MIXING: a client received another client's answer";

    for (size_t i = 0; i < all.size() && i < 3; ++i)
        ADD_FAILURE() << "client " << all[i].client << " req " << all[i].index
                      << "\n  Q:        " << all[i].question.substr(0, 90)
                      << "\n  expected: " << all[i].expected.substr(0, 120)
                      << "\n  got:      " << all[i].got.substr(0, 120);

    std::vector<double> flat;
    for (auto& v : lat) flat.insert(flat.end(), v.begin(), v.end());
    ASSERT_EQ(flat.size(), static_cast<size_t>(kTotal));
    std::sort(flat.begin(), flat.end());
    const auto pct = [&](double p) { return flat[static_cast<size_t>(p * (flat.size() - 1))]; };

    // Two very different numbers, printed together on purpose.
    //
    // The client-observed figure is dominated by queueing, not by the gateway: one
    // single-threaded loop serves 100 concurrent clients, so by Little's law the
    // wait is concurrency/throughput regardless of how fast the gateway is. The
    // harness makes it worse. ~201 threads (100 clients + up to 100 mock-provider
    // threads + the loop) on a 12-core box.
    //
    // The gateway's own added-latency histogram is the number that reflects this
    // codebase. If the two ever converge, the loop really has become the
    // bottleneck; while they differ by orders of magnitude, the client figure is
    // measuring the test harness.
    // stats() is owned by the loop thread, so join before reading it. The join used
    // to happen twenty lines further down, after this diagnostic print, which is a
    // data race TSan reports. Nothing below needs a live gateway.
    _gw->request_stop();
    if (_gt.joinable()) _gt.join();

    const llmbridge::Stats& st = _gw->stats();
    std::printf("[ %-5s ] %d reqs / %d clients in %.0f ms = %.0f req/s | "
                "client-observed p50 %.3f  p99 %.3f  max %.3f ms | gateway added %s\n",
                GetParam() == IoBackend::Epoll ? "epoll" : "uring",
                kTotal, kClients, wall, kTotal / (wall / 1000.0),
                pct(0.50), pct(0.99), flat.back(), added_latency(st.overhead).c_str());

    // ---- latency: loose ceiling only, on purpose (see the file header) ---
    // Deliberately ~50x the observed value. This catches a collapse (the gateway
    // stalling or serialising), never a regression; the printed numbers are for
    // that. An earlier 250 ms bound sat within 20% of the measured p99 and flaked
    // under `ctest -j4`, which is how a load test becomes noise people ignore.
    EXPECT_LT(pct(0.99), 2000.0) << "p99 " << pct(0.99) << " ms, past the collapse threshold";

    EXPECT_EQ(_gw->stats().requests, static_cast<uint64_t>(kTotal));
    EXPECT_EQ(_gw->stats().errors, 0u);
    EXPECT_EQ(_backend.served(), kTotal);
    // Keep-alive must hold: 1000 requests over far fewer upstream connections.
    EXPECT_LT(_gw->stats().upstream_conns_opened, static_cast<uint64_t>(kTotal))
        << "no upstream connection reuse: the pool is not working";
}

// The subset most likely to break escaping, run on its own so a failure points
// straight at the character class instead of at "one of 1000 answers".
TEST_P(CorpusIT, TextStressAnswersRoundTripByteForByte)
{
    start();
    // Every kind except plain prose is here on purpose: escape_stress forces
    // JSON-hostile characters, json_hostile answers are *about* escaping (so they
    // are full of backslashes and \uXXXX the model wrote literally), tricky_text
    // is Unicode edge cases.
    std::vector<const QA*> hard;
    for (size_t i = 0; i < corpus().size(); ++i)
    {
        const std::string& k = corpus()[i].kind;
        if (k == "escape_stress" || k == "json_hostile" || k == "tricky_text")
            hard.push_back(&corpus()[i]);
    }
    ASSERT_FALSE(hard.empty()) << "corpus has no text-stress entries; regenerate it";

    Client cl;
    ASSERT_TRUE(cl.connect(_port));
    int checked = 0;
    for (const QA* qa : hard)
    {
        ASSERT_TRUE(cl.send_all(openai_request(qa->q, _port)));
        const std::string resp = cl.recv_one();
        ASSERT_FALSE(resp.empty()) << "no response for: " << qa->q.substr(0, 60);
        EXPECT_EQ(answer_of(resp), qa->a)
            << "escaping regression on: " << qa->q.substr(0, 60);
        ++checked;
    }
    cl.close();
    std::printf("[ %-5s ] TEXT-STRESS %d answers (escape/json_hostile/tricky) round-tripped byte for byte\n",
                GetParam() == IoBackend::Epoll ? "epoll" : "uring", checked);
}

// The same 1000-request shape, streamed. Latency is reported as TTFT, the number
// a voice agent actually feels, alongside stream completion time, instead of the
// single round-trip figure the non-streaming case prints.
TEST_P(CorpusIT, StreamedThousandQuestionsAcrossHundredClients)
{
    start();

    std::vector<std::vector<Failure>> fails(kClients);
    std::vector<Lat> ttft(kClients), total(kClients);
    std::vector<long long> deltas(kClients, 0);
    std::atomic<int> broken{0}, no_done{0};

    const auto t0 = std::chrono::steady_clock::now();
    std::vector<std::thread> threads;
    threads.reserve(kClients);
    for (int c = 0; c < kClients; ++c)
    {
        threads.emplace_back([&, c] {
            for (int k = 0; k < kPerClient; ++k)
            {
                const QA& qa = pick(c * kPerClient + k);
                // A streamed response is close-delimited, so each one gets its own
                // connection, which is also what an SSE client does in practice.
                // One retry on connect: 1000 streams means 1000 fresh connections,
                // and ephemeral-port pressure under a loaded machine is a documented
                // host artifact here (bench/BENCHMARK-CONFIG.md), not a gateway bug.
                Client cl;
                if (!cl.connect(_port))
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
                    if (!cl.connect(_port)) { broken.fetch_add(1); return; }
                }
                const StreamOutcome r = cl.stream_once(openai_stream_request(qa.q, _port));
                if (!r.ok) { broken.fetch_add(1); cl.close(); continue; }
                if (!r.saw_done) no_done.fetch_add(1);
                ttft[c].add(r.ttft_ms);
                total[c].add(r.total_ms);
                deltas[c] += r.deltas;
                if (r.content != qa.a)
                    fails[c].push_back({c, k, qa.q, qa.a, r.content});
                cl.close();
            }
        });
    }
    for (auto& t : threads) t.join();
    const double wall = std::chrono::duration<double, std::milli>(
                            std::chrono::steady_clock::now() - t0).count();

    EXPECT_EQ(broken.load(), 0) << "streams that never completed";
    EXPECT_EQ(no_done.load(), 0) << "streams with no terminal [DONE]";
    EXPECT_EQ(_backend.unknown_questions(), 0);
    EXPECT_EQ(_backend.malformed(), 0);

    std::vector<Failure> all;
    for (auto& f : fails) all.insert(all.end(), f.begin(), f.end());
    EXPECT_TRUE(all.empty()) << all.size() << " of " << kTotal
                             << " streamed answers did not reassemble to the expected text";
    for (size_t i = 0; i < all.size() && i < 3; ++i)
        ADD_FAILURE() << "client " << all[i].client << " req " << all[i].index
                      << "\n  expected(" << all[i].expected.size() << "B): "
                      << all[i].expected.substr(0, 110)
                      << "\n  got     (" << all[i].got.size() << "B): "
                      << all[i].got.substr(0, 110);

    Lat tt, tot;
    long long ndelta = 0;
    for (int c = 0; c < kClients; ++c)
    {
        for (double v : ttft[c].v) tt.add(v);
        for (double v : total[c].v) tot.add(v);
        ndelta += deltas[c];
    }
    ASSERT_EQ(tt.size(), static_cast<size_t>(kTotal));
    std::printf("[ %-5s ] STREAMING  %d streams / %d clients in %.0f ms = %.0f streams/s | "
                "TTFT p50 %.3f  p99 %.3f  max %.3f ms | complete p50 %.3f  p99 %.3f ms | "
                "%lld deltas (%.1f/stream)\n",
                GetParam() == IoBackend::Epoll ? "epoll" : "uring",
                kTotal, kClients, wall, kTotal / (wall / 1000.0),
                tt.pct(0.50), tt.pct(0.99), tt.max(), tot.pct(0.50), tot.pct(0.99),
                ndelta, static_cast<double>(ndelta) / kTotal);

    EXPECT_GT(ndelta, static_cast<long long>(kTotal))
        << "answers arrived in one delta each: the mock is not really streaming";
    // See the non-streaming note: collapse threshold only, far above the observed
    // value, because this test runs 1000 concurrent streams and its wall-clock is
    // sensitive to whatever else the machine is doing.
    EXPECT_LT(tt.pct(0.99), 2000.0) << "TTFT p99 " << tt.pct(0.99) << " ms, past the collapse threshold";

    _gw->request_stop();
    if (_gt.joinable()) _gt.join();
    EXPECT_EQ(_gw->stats().errors, 0u);
}

// Large answers are where the two backends genuinely differ: io_uring reassembles
// across a provided-buffer ring (kUrBufSize = 4096) while epoll grows one buffer, and
// on the streaming path io_uring accumulates in `wpending` where epoll pauses reads.
// Anything past a few KB therefore exercises code that is not shared.
TEST_P(CorpusIT, LargeAnswersCrossBufferBoundariesOnBothPaths)
{
    start();
    std::vector<const QA*> big;
    for (size_t i = 0; i < corpus().size(); ++i)
        if (corpus()[i].kind == "backend_stress" || corpus()[i].kind == "long")
            big.push_back(&corpus()[i]);
    ASSERT_FALSE(big.empty()) << "corpus has no large answers; regenerate it";
    if (big.size() > 60) big.resize(60);

    size_t largest = 0, over_4k = 0;
    Lat plain, stream_ttft;
    for (const QA* qa : big)
    {
        largest = std::max(largest, qa->a.size());
        if (qa->a.size() > 4096) ++over_4k;

        Client a;
        ASSERT_TRUE(a.connect(_port));
        const auto s0 = std::chrono::steady_clock::now();
        ASSERT_TRUE(a.send_all(openai_request(qa->q, _port)));
        const std::string resp = a.recv_one();
        plain.add(std::chrono::duration<double, std::milli>(
                      std::chrono::steady_clock::now() - s0).count());
        ASSERT_FALSE(resp.empty()) << "no non-streaming response for a " << qa->a.size()
                                   << "B answer";
        EXPECT_EQ(answer_of(resp), qa->a) << "non-streaming truncation/corruption at "
                                          << qa->a.size() << " bytes";
        a.close();

        Client b;
        ASSERT_TRUE(b.connect(_port));
        const StreamOutcome r = b.stream_once(openai_stream_request(qa->q, _port));
        EXPECT_TRUE(r.ok) << "stream did not complete for a " << qa->a.size() << "B answer";
        EXPECT_EQ(r.content, qa->a) << "streamed truncation/corruption at " << qa->a.size()
                                    << " bytes (" << r.deltas << " deltas)";
        stream_ttft.add(r.ttft_ms);
        b.close();
    }
    std::printf("[ %-5s ] LARGE      %zu answers, largest %zu B, %zu over the 4 KiB uring "
                "buffer | non-stream p50 %.3f ms | stream TTFT p50 %.3f ms\n",
                GetParam() == IoBackend::Epoll ? "epoll" : "uring", big.size(), largest,
                over_4k, plain.pct(0.50), stream_ttft.pct(0.50));
    EXPECT_GT(over_4k, 0u) << "no answer exceeds one io_uring provided buffer; this test "
                              "is not reaching the multi-buffer path it exists for";
}

// Control for the two load tests above: identical code path, one client, no
// concurrency. Anything the loaded runs report beyond what this reports is
// queueing, not gateway cost, so this is the number to reason about when the
// concurrent figures look alarming.
TEST_P(CorpusIT, SequentialBaselineSeparatesGatewayCostFromQueueing)
{
    start();
    constexpr int kN = 300;
    Lat wall;
    Client cl;
    ASSERT_TRUE(cl.connect(_port));
    for (int i = 0; i < kN; ++i)
    {
        const QA& qa = pick(i);
        const auto t = std::chrono::steady_clock::now();
        ASSERT_TRUE(cl.send_all(openai_request(qa.q, _port)));
        const std::string resp = cl.recv_one();
        wall.add(std::chrono::duration<double, std::milli>(
                     std::chrono::steady_clock::now() - t).count());
        ASSERT_FALSE(resp.empty());
        EXPECT_EQ(answer_of(resp), qa.a);
    }
    cl.close();

    _gw->request_stop();
    if (_gt.joinable()) _gt.join();
    const llmbridge::Stats& st = _gw->stats();
    std::printf("[ %-5s ] SEQUENTIAL %d reqs, 1 client | client-observed p50 %.3f  p99 %.3f ms | "
                "gateway added %s\n",
                GetParam() == IoBackend::Epoll ? "epoll" : "uring", kN,
                wall.pct(0.50), wall.pct(0.99),
                added_latency(st.overhead).c_str());
    EXPECT_EQ(st.errors, 0u);
}

// Where does the queueing start? Same workload, same code, concurrency swept.
//
// This exists to answer a specific question honestly: the 100-client runs report
// client-observed latency in the 10 ms range, and it is fair to ask whether the
// gateway is slow. It is not; that figure is Little's law applied to one
// single-threaded loop, and this sweep shows it arriving right on schedule.
//
// It also shows the cost of turning the concurrency down: fewer clients means fewer
// pooled upstream connections, which is exactly the thing the correlation test needs
// in order to be able to observe a cross-client desync at all. So the sweep informs
// the trade-off instead of settling it.
TEST_P(CorpusIT, ConcurrencySweepSeparatesQueueingFromGatewayCost)
{
    _backend.start(&_answers);
    std::printf("[ %-5s ] SWEEP  (one single-threaded loop; 12 cores)\n",
                GetParam() == IoBackend::Epoll ? "epoll" : "uring");
    std::printf("           %8s %10s %12s %14s %10s   %s\n",
                "clients", "req/s", "p50 ms", "p99 ms", "little's", "gateway added");

    // Capped at 100: the gateway's own per-request cost is flat across the whole
    // range (measured 45 us at 1 client, 79 us at 500), so the higher levels add
    // ~1000 threads and no new information. 100 is also what the correlation
    // tests above use, which keeps the two comparable.
    for (const int nclients : {1, 5, 10, 25, 50, 100})
    {
        const int per = std::max(4, 400 / nclients);
        auto gw = std::make_unique<Gateway>(0, "127.0.0.1", _backend.port(), 0,
                                            UpstreamDialect::Anthropic, GetParam(),
                                            Gateway::kDefaultUpstreamIdleNs,
                                            llmbridge::TlsConfig{}, false);
        const uint16_t port = gw->bound_port();
        std::thread th([&gw] { gw->run(); });

        std::vector<std::vector<double>> lat(nclients);
        std::atomic<int> bad{0};
        const auto t0 = std::chrono::steady_clock::now();
        std::vector<std::thread> ts;
        for (int c = 0; c < nclients; ++c)
        {
            ts.emplace_back([&, c] {
                Client cl;
                if (!cl.connect(port)) { bad.fetch_add(1); return; }
                for (int k = 0; k < per; ++k)
                {
                    const QA& qa = pick(c * per + k);
                    const auto s0 = std::chrono::steady_clock::now();
                    if (!cl.send_all(openai_request(qa.q, port))) { bad.fetch_add(1); return; }
                    const std::string resp = cl.recv_one();
                    if (resp.empty()) { bad.fetch_add(1); return; }
                    lat[c].push_back(std::chrono::duration<double, std::milli>(
                                         std::chrono::steady_clock::now() - s0).count());
                    if (answer_of(resp) != qa.a) bad.fetch_add(1);
                }
                cl.close();
            });
        }
        for (auto& t : ts) t.join();
        const double wall = std::chrono::duration<double, std::milli>(
                                std::chrono::steady_clock::now() - t0).count();
        gw->request_stop();
        if (th.joinable()) th.join();

        EXPECT_EQ(bad.load(), 0) << "errors at concurrency " << nclients;
        Lat all;
        for (auto& v : lat) for (double x : v) all.add(x);
        const double rps = (nclients * per) / (wall / 1000.0);
        // Little's law prediction: in-flight / throughput. If this tracks the measured
        // p50, the latency is the queue and not the gateway.
        const double little = nclients / rps * 1000.0;
        std::printf("           %8d %10.0f %12.3f %14.3f %10.3f   %s\n",
                    nclients, rps, all.pct(0.50), all.pct(0.99), little,
                    added_latency(gw->stats().overhead).c_str());
    }
}

// Raw control bytes, which the corpus cannot reach.
//
// The json_hostile entries ask Claude directly about \u0000, raw control
// characters, form feed and backspace. It answers by *describing* them: 68 corpus
// answers contain a literal backslash-u sequence as text, and zero contain a raw
// control byte. So that category exercises backslash and \u-literal handling: real
// and worth having, but not the byte class where the control-character defect lived.
//
// Recorded model output cannot cover this, so cover it deterministically. Both
// directions matter and they have different correct answers:
//
//   escaped by the provider  -> must round-trip byte for byte, and our own body
//                               must contain no raw control byte either
//   raw from the provider    -> must be refused (RFC 8259 s7), never forwarded into
//                               a 200 that a strict client then chokes on
TEST_P(CorpusIT, ControlBytesAreRoundTrippedWhenEscapedAndRefusedWhenRaw)
{
    std::string payload = "before";
    for (char c = 0x01; c < 0x20; ++c) payload += c;   // every control byte
    payload += '\0';                                     // and a NUL
    payload += "after";

    {   // --- provider escapes them properly: byte-exact round trip ---
        _backend.set_wrong_answer(payload);
        start();
        Client cl;
        ASSERT_TRUE(cl.connect(_port));
        ASSERT_TRUE(cl.send_all(openai_request(corpus()[0].q, _port)));
        const std::string resp = cl.recv_one();
        ASSERT_FALSE(resp.empty());
        EXPECT_NE(resp.find("200 OK"), std::string::npos);

        // The body we hand the client must itself be legal JSON: no raw control byte
        // after the header block. This is exactly the 0.9.0 defect: a 200
        // whose body Python's json.loads rejects.
        const size_t hdr = resp.find("\r\n\r\n");
        ASSERT_NE(hdr, std::string::npos);
        for (size_t i = hdr + 4; i < resp.size(); ++i)
            ASSERT_GE(static_cast<unsigned char>(resp[i]), 0x20)
                << "raw control byte at body offset " << (i - hdr - 4);

        EXPECT_EQ(answer_of(resp), payload) << "escaped control bytes did not survive";
        cl.close();
        TearDown();
    }
    {   // --- provider sends them raw: must fail closed ---
        _gw.reset();
        _backend.set_wrong_answer(payload);
        _backend.set_skip_escaping(true);
        start();
        Client cl;
        ASSERT_TRUE(cl.connect(_port));
        ASSERT_TRUE(cl.send_all(openai_request(corpus()[0].q, _port)));
        const std::string resp = cl.recv_one();
        if (!resp.empty())
        {
            EXPECT_EQ(resp.find("200 OK"), std::string::npos)
                << "a provider body with raw control bytes was laundered into a 200";
            const size_t hdr = resp.find("\r\n\r\n");
            if (hdr != std::string::npos)
            {
                    for (size_t i = hdr + 4; i < resp.size(); ++i)
                        EXPECT_GE(static_cast<unsigned char>(resp[i]), 0x20)
                            << "raw control byte leaked into our error envelope";
            }
        }
        cl.close();
    }
    std::printf("[ %-5s ] CONTROL BYTES 0x01-0x1F + NUL: escaped round-trips, raw refused\n",
                GetParam() == IoBackend::Epoll ? "epoll" : "uring");
}

// ---------------------------------------------------------------- negative controls
//
// The two tests above pass. That is only meaningful if they would fail when the
// property they check is violated. A test that has never failed is decoration,
// the same reasoning that makes every security fix here ship with a test that
// fails without it. These two break the property on purpose and assert the
// harness notices. If either of these ever passes silently, the tests above are
// no longer checking anything.

TEST_P(CorpusIT, NegativeControl_DetectsResponseMixing)
{
    // The provider answers every question with question 0's answer, exactly what a
    // client would observe if a pooled-connection desync handed it another client's
    // response body.
    _backend.set_wrong_answer(corpus()[0].a);
    start();

    Client cl;
    ASSERT_TRUE(cl.connect(_port));
    int detected = 0, checked = 0;
    for (int i = 1; i <= 20; ++i) // skip 0: its answer is the injected one
    {
        const QA& qa = corpus()[i];
        ASSERT_TRUE(cl.send_all(openai_request(qa.q, _port)));
        const std::string resp = cl.recv_one();
        ASSERT_FALSE(resp.empty());
        ++checked;
        const std::string got = answer_of(resp);
        if (got != qa.a) ++detected;
        // and it is specifically another question's answer, not corruption
        EXPECT_EQ(got, corpus()[0].a);
    }
    cl.close();
    EXPECT_EQ(detected, checked)
        << "the correlation check did NOT notice deliberately mixed responses; "
           "ThousandQuestionsAcrossHundredClients is not actually verifying correlation";
    std::printf("[ %-5s ] negative control: %d/%d mixed responses detected\n",
                GetParam() == IoBackend::Epoll ? "epoll" : "uring", detected, checked);
}

TEST_P(CorpusIT, NegativeControl_DetectsEscapingRegression)
{
    // The provider stops escaping the answer text. For answers containing a quote,
    // newline or backslash the body is now invalid JSON, so the gateway must reject
    // it (non-200) instead of forward something malformed; for the rest the text
    // arrives altered. Either way the client must not see the correct answer.
    _backend.set_skip_escaping(true);
    start();

    // Reconnect per iteration: rejecting a malformed upstream body closes the
    // client connection, so a single keep-alive connection would sample only one
    // case and report a misleadingly thin "1/1 detected".
    int checked = 0, caught = 0;
    for (size_t i = 0; i < corpus().size() && checked < 20; ++i)
    {
        const QA& qa = corpus()[i];
        // Only answers that actually need escaping can demonstrate the regression.
        if (qa.a.find('"') == std::string::npos && qa.a.find('\n') == std::string::npos &&
            qa.a.find('\\') == std::string::npos)
            continue;
        Client cl;
        ASSERT_TRUE(cl.connect(_port));
        ASSERT_TRUE(cl.send_all(openai_request(qa.q, _port)));
        const std::string resp = cl.recv_one();
        ++checked;
        // A dropped connection is a legitimate way to refuse a malformed body.
        if (resp.empty()) { ++caught; continue; }
        const bool ok_status = resp.find("200 OK") != std::string::npos;
        if (!ok_status || answer_of(resp) != qa.a) ++caught;
    }
    ASSERT_GT(checked, 0) << "no answers in the corpus require escaping; regenerate it";
    EXPECT_EQ(caught, checked)
        << "an unescaped provider body was accepted AND round-tripped as correct; "
           "TextStressAnswersRoundTripByteForByte is not actually verifying escaping";
    std::printf("[ %-5s ] negative control: %d/%d escaping regressions detected\n",
                GetParam() == IoBackend::Epoll ? "epoll" : "uring", caught, checked);
}

INSTANTIATE_TEST_SUITE_P(Backends, CorpusIT,
                         ::testing::Values(IoBackend::Epoll, IoBackend::Uring),
                         [](const testing::TestParamInfo<IoBackend>& i) {
                             return i.param == IoBackend::Epoll ? "epoll" : "uring";
                         });
