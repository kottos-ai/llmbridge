// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

// libFuzzer target for the `--config` parser (app/options.cpp). Any input may be
// refused, never crash or throw, and an accepted one leaves every number inside its
// row's bounds and every word among its row's choices.

#include "options.hpp"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    using namespace llmbridge::app;
    Settings s;
    std::string err;
    if (!parse_config(std::string_view(reinterpret_cast<const char*>(data), size), s, err))
    {
        assert(!err.empty());
        return 0;
    }
    for (const Option& o : options())
    {
        if (o.kind == Kind::Int)
        {
            const int v = s.*std::get<int Settings::*>(o.field);
            assert(v >= o.lo && v <= o.hi);
        }
        if (o.kind == Kind::Number)
        {
            const double v = s.*std::get<double Settings::*>(o.field);
            assert(v >= o.lo && v <= o.hi);
        }
        if (o.kind == Kind::Choice)
        {
            const std::string& v = s.*std::get<std::string Settings::*>(o.field);
            bool known = false;
            for (std::string_view c : o.choices) known = known || c == v;
            assert(known);
        }
    }
    for (const Venue& v : s.more_upstreams) assert(!v.url.empty() && !v.dialect.empty());
    return 0;
}
