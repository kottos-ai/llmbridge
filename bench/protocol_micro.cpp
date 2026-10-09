// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

// Per-call cost of the protocol work on the request path, outside the event loop:
// a realistic provider response head, a translated response, one translated SSE
// event, and a large agent request. The bench mocks use a 4-line head and tiny
// bodies, which hide these costs; this is the baseline the refactor is held to.
// Not run by ctest (timings jitter on shared runners). Run it on a quiet machine
// from a Release build and compare percentiles against a previous run.

#include "net/http.hpp"
#include "provider/sse.hpp"
#include "provider/translate.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

namespace
{
    namespace http = llmbridge::net::http;
    namespace provider = llmbridge::provider;

    // Batches amortise the ~16 ns clock read; enough batches keep the tail visible.
    template <class F>
    void measure(const char* name, int batch, int samples, F&& call)
    {
        for (int i = 0; i < batch * 100; ++i) call();
        std::vector<double> ns(static_cast<size_t>(samples));
        for (auto& v : ns)
        {
            const auto t0 = std::chrono::steady_clock::now();
            for (int j = 0; j < batch; ++j) call();
            const auto t1 = std::chrono::steady_clock::now();
            v = std::chrono::duration<double, std::nano>(t1 - t0).count() / batch;
        }
        std::sort(ns.begin(), ns.end());
        const auto at = [&](double p) { return ns[static_cast<size_t>(p / 100 * (ns.size() - 1))]; };
        std::printf("%-38s p50 %10.1f ns   p95 %10.1f ns   p99 %10.1f ns\n", name, at(50), at(95), at(99));
    }

    // Shaped like a real Anthropic response head: 26 lines, ~1 KB, rate-limit block.
    std::string anthropic_head()
    {
        std::string h = "HTTP/1.1 200 OK\r\nDate: Thu, 09 Oct 2026 12:00:00 GMT\r\n"
                        "Content-Type: application/json\r\nContent-Length: 412\r\n"
                        "Connection: keep-alive\r\n";
        for (const char* k : {"requests", "input-tokens", "output-tokens", "tokens"})
            for (const char* f : {"limit", "remaining", "reset"})
                h += std::string("anthropic-ratelimit-") + k + "-" + f +
                     (f[0] == 'r' && f[2] == 's' ? ": 2026-10-09T12:00:30Z\r\n" : ": 400000\r\n");
        h += "request-id: req_011CTESTREQUESTIDENTIFIER0001\r\n"
             "anthropic-organization-id: 00000000-0000-0000-0000-000000000000\r\n"
             "via: 1.1 google\r\ncf-cache-status: DYNAMIC\r\n"
             "X-Robots-Tag: none\r\nServer: cloudflare\r\n"
             "CF-RAY: 8c0123456789abcd-IAD\r\n\r\n";
        return h;
    }

    constexpr std::string_view kAnthropicReply =
        R"({"id":"msg_011CTESTMESSAGEID","type":"message","role":"assistant",)"
        R"("model":"claude-opus-5-5","content":[{"type":"text","text":"The capital of France )"
        R"(is Paris. It has been the capital since the 10th century and is the country's )"
        R"(largest city."}],"stop_reason":"end_turn","stop_sequence":null,)"
        R"("usage":{"input_tokens":25,"cache_creation_input_tokens":0,)"
        R"("cache_read_input_tokens":0,"output_tokens":31}})";

    constexpr std::string_view kSseStart =
        "event: message_start\ndata: {\"type\":\"message_start\",\"message\":{\"id\":\"msg_1\","
        "\"model\":\"claude-opus-5-5\",\"usage\":{\"input_tokens\":25,\"output_tokens\":1}}}\n\n"
        "event: content_block_start\ndata: {\"type\":\"content_block_start\",\"index\":0,"
        "\"content_block\":{\"type\":\"text\",\"text\":\"\"}}\n\n";
    constexpr std::string_view kSseDelta =
        "event: content_block_delta\ndata: {\"type\":\"content_block_delta\",\"index\":0,"
        "\"delta\":{\"type\":\"text_delta\",\"text\":\" capital\"}}\n\n";

    // An agent turn: system prompt, tools, and a long history, about 130 KB.
    std::string agent_request()
    {
        std::string b = R"({"model":"claude-opus-5-5","max_tokens":4096,"messages":[)"
                        R"({"role":"system","content":"You are a coding agent."})";
        const std::string text(1000, 'a');
        for (int i = 0; b.size() < 130'000; ++i)
            b += std::string(R"(,{"role":")") + (i % 2 ? "assistant" : "user") +
                 R"(","content":")" + text + R"("})";
        b += "]}";
        return b;
    }
} // namespace

int main()
{
    const std::string head = anthropic_head();
    measure("parse_response_head (~1 KB head)", 64, 100'000, [&] {
        http::ResponseHead h;
        if (http::parse_response_head(head, h) != http::FrameStatus::Complete) std::abort();
    });

    measure("anthropic_to_openai_response", 16, 50'000, [&] {
        if (provider::anthropic_to_openai_response(kAnthropicReply).empty()) std::abort();
    });

    provider::AnthropicToOpenAiSse sse(1'760'000'000);
    std::string out;
    if (!sse.feed(kSseStart, out)) std::abort();
    measure("AnthropicToOpenAiSse::feed (1 event)", 64, 100'000, [&] {
        out.clear();
        if (!sse.feed(kSseDelta, out)) std::abort();
    });

    const std::string agent = agent_request();
    std::string translated;
    measure("openai_to_anthropic_request (130 KB)", 1, 2'000, [&] {
        if (!provider::openai_to_anthropic_request(agent, translated)) std::abort();
    });
    std::printf("(agent body %zu bytes; head %zu bytes)\n", agent.size(), head.size());
    return 0;
}
