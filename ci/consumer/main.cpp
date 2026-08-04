// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

// Smallest useful consumer: include each installed public header, call across the
// public API, and check a real translation round-trips. Compiling is most of the
// value (it proves the headers stand alone and the namespaces are what we
// document), but asserting the output catches an install that ships stale headers
// against a fresh library.

#include "provider/json.hpp"
#include "provider/sse.hpp"
#include "provider/translate.hpp"

#include <cstdio>
#include <string>

int main()
{
    // 1. The translator, named exactly as the README documents it.
    const std::string openai =
        R"({"model":"gpt-4o-mini","messages":[{"role":"user","content":"hi"}]})";
    const std::string anthropic = llmbridge::provider::openai_to_anthropic_request(openai);
    if (anthropic.find("\"messages\"") == std::string::npos)
    {
        std::fprintf(stderr, "consumer: request translation produced no messages\n");
        return 1;
    }

    // 2. The JSON parser, at its post-0.9.0 namespace.
    bool ok = false;
    const llmbridge::provider::json::Value v =
        llmbridge::provider::json::parse(anthropic, ok);
    if (!ok || !v.is_object())
    {
        std::fprintf(stderr, "consumer: installed parser rejected our own output\n");
        return 1;
    }

    // 3. The strictness fix shipped in 0.9.0 must be present in the INSTALLED header,
    //    not just in-tree: a raw control byte in a string is refused.
    bool bad_ok = true;
    (void)llmbridge::provider::json::parse("{\"t\":\"a\nb\"}", bad_ok);
    if (bad_ok)
    {
        std::fprintf(stderr, "consumer: installed parser accepted a raw control byte\n");
        return 1;
    }

    // 4. The SSE translator is constructible from an installed header.
    llmbridge::provider::AnthropicToOpenAiSse sse;
    std::string out;
    (void)sse.feed("event: message_stop\ndata: {\"type\":\"message_stop\"}\n\n", out);

    std::printf("consumer: installed llmbridge OK (translate + json + sse)\n");
    return 0;
}
