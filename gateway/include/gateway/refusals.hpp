// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

#pragma once

namespace llmbridge
{
    /// What a refused request is told, in one place. The tests read these, so the
    /// wording lives here and editing it cannot break a test that is still right.
    namespace refuse
    {
        inline constexpr const char* kImage =
            "request translate: image content is not supported";
        inline constexpr const char* kAudio =
            "request translate: audio content is not supported";
        inline constexpr const char* kFile =
            "request translate: file content is not supported";
        inline constexpr const char* kPart =
            "request translate: unsupported content part; only \"text\" parts are carried";
        inline constexpr const char* kToolArgs =
            "request translate: tool_calls[].function.arguments must be a JSON object "
            "and nothing else";
        inline constexpr const char* kCredential =
            "a credential header holds bytes that cannot be forwarded "
            "(control characters are refused)";
        inline constexpr const char* kBedrockCredential =
            "a Bedrock venue is signed with the caller's AWS access key pair, sent as "
            "Bearer ACCESS_KEY_ID:SECRET or ACCESS_KEY_ID:SECRET:SESSION_TOKEN; the bearer "
            "sent is not one (a Bedrock API key is not accepted here)";
        inline constexpr const char* kNotJson = "request translate: body is not valid JSON";
        inline constexpr const char* kRepeatedKey =
            "request translate: an object in the body repeats a key";
        inline constexpr const char* kNotObject =
            "request translate: body is not a JSON object";
        inline constexpr const char* kNoModel = "request translate: no \"model\" field";
        inline constexpr const char* kNoMessages = "request translate: no \"messages\" field";
        inline constexpr const char* kShape = "request translate: unsupported request shape";
    } // namespace refuse
} // namespace llmbridge
