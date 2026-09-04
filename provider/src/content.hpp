// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

#pragma once

// The OpenAI `content` field, carried into a provider body. Shared by every request translator.

#include <string>
#include <string_view>

#include "provider/json.hpp"

namespace llmbridge::provider::detail
{
    // Append the raw (already JSON-escaped) text of a content field: a plain
    // string, or an array of {type:"text",text:...} parts, to `out`, without
    // surrounding quotes. Zero-copy passthrough: the input's escaping is exactly
    // the output's, so nothing is decoded or re-escaped; the bytes are viewed
    // straight out of the request/response buffer.
    // False when the content holds a part this translator cannot carry, which is
    // anything that is not text: an image, audio, a PDF.
    //
    // A content part's `cache_control`, forwarded byte for byte like a tool's
    // schema: it is the caller's field, and re-encoding it could only change it.
    [[nodiscard]] inline bool part_cache_control(const json::Value& part, std::string_view& out)
    {
        const json::Value* cc = part.find("cache_control");
        if (!cc) return true;
        if (!cc->is_object()) return false;
        out = cc->sv;
        return true;
    }

    // Whether any part of this content carries a breakpoint, and refusal if one is
    // malformed. Scanned before emitting anything, because the answer decides
    // which of the two shapes below the content takes.
    [[nodiscard]] inline bool content_cache_state(const json::Value* c, bool& any)
    {
        any = false;
        if (!c || !c->is_array()) return true;
        for (const auto& part : c->arr)
        {
            std::string_view cc;
            if (!part_cache_control(part, cc)) return false;
            if (!cc.empty()) any = true;
        }
        return true;
    }

    [[nodiscard]] inline bool append_text(std::string& out, const json::Value* c)
    {
        if (!c) return true;
        if (c->is_string()) { out += c->sv; return true; }
        if (c->is_array())
            for (const auto& part : c->arr)
            {
                const std::string_view type = part.str_or("type");
                if (type == "text") { out += part.str_or("text"); continue; }
                // A part with no type at all is as unusable as an unknown one:
                // guessing it is text is the same silent edit.
                return false;
            }
        return true;
    }

    // The whole Anthropic `content` value, quotes included, in whichever of the
    // two shapes the input needs.
    [[nodiscard]] inline bool append_content(std::string& out, const json::Value* c)
    {
        bool cached = false;
        if (!content_cache_state(c, cached)) return false;
        if (!cached)
        {
            out += '"';
            if (!append_text(out, c)) return false;
            out += '"';
            return true;
        }
        out += '[';
        bool first = true;
        for (const auto& part : c->arr)
        {
            // Same refusal as the flattening path: a part this translator cannot
            // carry is refused by name, never dropped.
            if (part.str_or("type") != "text") return false;
            if (!first) out += ',';
            first = false;
            out += R"({"type":"text","text":)";
            json::append_raw_string(out, part.str_or("text"));
            std::string_view cc;
            (void)part_cache_control(part, cc); // already validated above
            if (!cc.empty())
            {
                out += ",\"cache_control\":";
                out.append(cc);
            }
            out += '}';
        }
        out += ']';
        return true;
    }
} // namespace llmbridge::provider::detail
