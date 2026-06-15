// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

// debug_translate — a tiny, synchronous harness for stepping through the dialect
// translator in a debugger (CLion, gdb, lldb). Unlike the live gateway, there is
// NO event loop, NO sockets, NO async I/O — each translate call runs inline on a
// hardcoded payload, so you can set a breakpoint on the call and "Step Into"
// (CLion: F7) to walk provider/src/translate.cpp line by line.
//
// Build & debug in CLion:
//   1. Open the repo root as a CMake project.
//   2. Pick the "Debug" CMake profile (Settings ▸ Build ▸ CMake) so it compiles
//      -O0 -g — optimized builds make stepping jumpy.
//   3. Select the `debug_translate` run/debug configuration.
//   4. Put a breakpoint on a `provider::...` call below (or inside translate.cpp)
//      and Debug (Shift+F9). Step Into to enter the translator.
//
// Pick a direction with argv[1]: anthropic | gemini | cohere | all (default all).
//   ./debug_translate anthropic

#include <iostream>
#include <string>
#include <string_view>

#include "provider/translate.hpp"

namespace
{
    void banner(std::string_view title)
    {
        std::cout << "\n==== " << title << " ====\n";
    }

    void show(std::string_view label, std::string_view body)
    {
        std::cout << "-- " << label << " --\n" << body << "\n";
    }

    // A representative OpenAI chat-completion request: system message + a
    // multi-turn user/assistant exchange + sampling params. Enough to exercise
    // system extraction, message restructuring, and parameter renaming. Edit it
    // freely — it's just a string. Set `model` to the TARGET provider's model
    // name; llmbridge passes `model` through verbatim (it translates the format,
    // not the model identity).
    std::string openai_request(std::string_view model)
    {
        return std::string(R"({
  "model": ")") + std::string(model) + R"(",
  "messages": [
    { "role": "system", "content": "You are a terse developer." },
    { "role": "user", "content": "What is 2+2?" },
    { "role": "assistant", "content": "4" },
    { "role": "user", "content": "Now reply with exactly: pong" }
  ],
  "max_tokens": 64,
  "temperature": 0.2,
  "top_p": 0.9
})";
    }

    // ── OpenAI ⇄ Anthropic ──────────────────────────────────────────────────
    void debug_anthropic()
    {
        banner("OpenAI -> Anthropic -> OpenAI");

        const std::string oai = openai_request("claude-3-5-sonnet-20241022");
        show("1. OpenAI request (input)", oai);

        // ⬇ BREAKPOINT here, then Step Into to walk the request translator.
        const std::string anthropic_req =
            llmbridge::provider::openai_to_anthropic_request(oai);
        show("2. translated Anthropic request", anthropic_req);

        // A canned Anthropic Messages response (what a real Claude endpoint returns).
        const std::string anthropic_resp = R"({
  "id": "msg_debug01",
  "type": "message",
  "role": "assistant",
  "model": "claude-3-5-sonnet-20241022",
  "content": [ { "type": "text", "text": "pong" } ],
  "stop_reason": "end_turn",
  "usage": { "input_tokens": 25, "output_tokens": 1 }
})";
        show("3. Anthropic response (input)", anthropic_resp);

        // ⬇ BREAKPOINT here, then Step Into to walk the response translator.
        const std::string oai_resp =
            llmbridge::provider::anthropic_to_openai_response(anthropic_resp);
        show("4. translated OpenAI response", oai_resp);
    }

    // ── OpenAI ⇄ Gemini ─────────────────────────────────────────────────────
    void debug_gemini()
    {
        banner("OpenAI -> Gemini -> OpenAI");

        const std::string oai = openai_request("gemini-1.5-pro");
        show("1. OpenAI request (input)", oai);

        const std::string gemini_req =
            llmbridge::provider::openai_to_gemini_request(oai);
        show("2. translated Gemini request", gemini_req);

        const std::string gemini_resp = R"({
  "candidates": [ {
    "content": { "role": "model", "parts": [ { "text": "pong" } ] },
    "finishReason": "STOP", "index": 0
  } ],
  "usageMetadata": { "promptTokenCount": 25, "candidatesTokenCount": 1, "totalTokenCount": 26 }
})";
        show("3. Gemini response (input)", gemini_resp);

        const std::string oai_resp =
            llmbridge::provider::gemini_to_openai_response(gemini_resp);
        show("4. translated OpenAI response", oai_resp);
    }

    // ── OpenAI ⇄ Cohere ─────────────────────────────────────────────────────
    void debug_cohere()
    {
        banner("OpenAI -> Cohere -> OpenAI");

        const std::string oai = openai_request("command-r-plus");
        show("1. OpenAI request (input)", oai);

        const std::string cohere_req =
            llmbridge::provider::openai_to_cohere_request(oai);
        show("2. translated Cohere request", cohere_req);

        const std::string cohere_resp = R"({
  "id": "debug01",
  "finish_reason": "COMPLETE",
  "message": { "role": "assistant", "content": [ { "type": "text", "text": "pong" } ] },
  "usage": { "tokens": { "input_tokens": 25, "output_tokens": 1 } }
})";
        show("3. Cohere response (input)", cohere_resp);

        const std::string oai_resp =
            llmbridge::provider::cohere_to_openai_response(cohere_resp);
        show("4. translated OpenAI response", oai_resp);
    }
} // namespace

int main(int argc, char** argv)
{
    const std::string which = (argc > 1) ? argv[1] : "all";

    if (which == "anthropic" || which == "all") debug_anthropic();
    if (which == "gemini"    || which == "all") debug_gemini();
    if (which == "cohere"    || which == "all") debug_cohere();

    if (which != "anthropic" && which != "gemini" && which != "cohere" && which != "all")
    {
        std::cerr << "usage: debug_translate [anthropic|gemini|cohere|all]\n";
        return 2;
    }
    return 0;
}
