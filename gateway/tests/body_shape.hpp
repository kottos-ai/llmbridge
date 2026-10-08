// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include <cstddef>
#include <string>

namespace llmbridge::test
{
    /// A coding agent's request body of about `target` bytes: a system prompt with
    /// escaped quotes, tool schemas whose properties are named `stream` and `model`,
    /// then messages. `keys_last` puts model, stream and stream_options after the
    /// messages, the order the OpenAI Python SDK sends, so every reader of them has
    /// to cross the whole body.
    inline std::string agent_body(size_t target, bool keys_last)
    {
        const std::string keys =
            R"("model":"gpt-6","max_tokens":4096,"stream":true,"stream_options":{"include_usage":true})";
        std::string s = keys_last ? "{" : "{" + keys + ",";
        s += R"("system":")";
        const std::string chunk =
            "You are a coding agent. Read the files and edit them carefully; "
            "\\\"quote\\\" and \\n newline. ";
        while (s.size() < target / 3) s += chunk;
        s += R"(","tools":[)";
        for (int i = 0; s.size() < target * 2 / 3; ++i)
            s += (i ? "," : "") + std::string(R"({"name":"tool)") + std::to_string(i) +
                 R"(","description":"Does a thing with a path and some options",)"
                 R"("input_schema":{"type":"object","properties":{"path":{"type":"string"},)"
                 R"("stream":{"type":"boolean"},"model":{"type":"string"}},"required":["path"]}})";
        s += R"(],"messages":[)";
        for (int i = 0; s.size() < target; ++i)
            s += (i ? "," : "") + std::string(R"({"role":"user","content":")") + chunk + chunk +
                 R"("})";
        s += keys_last ? "]," + keys + "}" : "]}";
        return s;
    }
} // namespace llmbridge::test
