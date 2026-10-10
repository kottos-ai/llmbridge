// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

#pragma once

// The OpenAI `messages` array and the request fields every dialect reads the same way.

#include <string_view>

#include "provider/json.hpp"

namespace llmbridge::provider::detail
{
    /// OpenAI's `developer` role is the system prompt under a newer name.
    inline bool is_system_role(std::string_view role) noexcept
    {
        return role == "system" || role == "developer";
    }

    /// One hook call per message, in order. A construct the dialect has no hook for
    /// (tool_calls, tool_result) or an unknown role is refused, never dropped.
    template <class Hooks>
    [[nodiscard]] bool walk_messages(const json::Value& msgs, Hooks& h)
    {
        for (const json::Value& m : msgs.arr)
        {
            const std::string_view role = m.str_or("role");
            const json::Value* content = m.find("content");
            const json::Value* calls = m.find("tool_calls");
            bool ok = false;
            if (is_system_role(role)) ok = h.system(content);
            else if (role == "user") ok = h.turn(false, content);
            else if (role == "assistant" && calls && calls->is_array() && !calls->arr.empty())
            {
                if constexpr (requires { h.tool_calls(content, *calls); })
                    ok = h.tool_calls(content, *calls);
            }
            else if (role == "assistant") ok = h.turn(true, content);
            else if (role == "tool")
            {
                if constexpr (requires { h.tool_result(m, content); }) ok = h.tool_result(m, content);
            }
            if (!ok) return false;
        }
        return true;
    }

    /// Whether the body declares tools, which a dialect without tool support refuses.
    inline bool declares_tools(const json::Value& body)
    {
        const json::Value* t = body.find("tools");
        return t && t->type != json::Value::Type::Null && !(t->is_array() && t->arr.empty());
    }

    /// `max_completion_tokens`, which replaced `max_tokens` in OpenAI's API, else `max_tokens`.
    inline std::string_view max_tokens_of(const json::Value& body, std::string_view def = {})
    {
        const std::string_view n = body.num_or("max_completion_tokens");
        return n.empty() ? body.num_or("max_tokens", def) : n;
    }
} // namespace llmbridge::provider::detail
