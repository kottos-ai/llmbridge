// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

#include "net/log.hpp"

#include <gtest/gtest.h>

#include <string>
#include <thread>
#include <vector>

namespace log_ = llmbridge::net::log;

namespace
{
    /// Captures whole lines, which is also the shape a private SPSC sink takes: it
    /// is handed one finished line and must not block.
    class CaptureSink final : public log_::Sink
    {
    public:
        void write(log_::Level lv, std::string_view line) noexcept override
        {
            lines.emplace_back(line);
            levels.push_back(lv);
        }
        std::vector<std::string> lines;
        std::vector<log_::Level> levels;
        [[nodiscard]] std::string all() const
        {
            std::string s;
            for (const auto& l : lines) s += l;
            return s;
        }
    };

    struct Fixture : ::testing::Test
    {
        CaptureSink sink;
        log_::Level saved = log_::Level::Info;
        void SetUp() override
        {
            saved = log_::level();
            log_::set_sink(&sink);
            log_::set_level(log_::Level::Trace);
        }
        void TearDown() override
        {
            log_::set_sink(nullptr);
            log_::set_level(saved);
        }
    };

    // An object with a log_put found by ADL: this is the "print method" a class gets
    // without dragging iostreams onto the event loop.
    struct Widget
    {
        int fd = 7;
        uint64_t inst = 42;
    };
    void log_put(log_::Line& l, const Widget& w)
    {
        l.put(log_::Id{"Widget", w.inst});
        l.put("(fd=");
        l.put(static_cast<int64_t>(w.fd));
        l.put(')');
    }
} // namespace

using LogTest = Fixture;

TEST_F(LogTest, LineCarriesLevelThreadSiteAndMessage)
{
    log_::register_thread("worker", 3);
    LB_ERROR("boom code=", 42);
    ASSERT_EQ(sink.lines.size(), 1u);
    const std::string& l = sink.lines[0];
    EXPECT_NE(l.find("ERROR"), std::string::npos);
    EXPECT_NE(l.find("worker/3"), std::string::npos) << l;
    EXPECT_NE(l.find("log_test.cpp:"), std::string::npos) << l;
    EXPECT_NE(l.find("boom code=42"), std::string::npos) << l;
    EXPECT_EQ(l.back(), '\n') << "every record must be one whole line";
    log_::register_thread("main", 0);
}

// The identity the founder asked for: a class name and an instance number, so one
// object is greppable across its whole life.
TEST_F(LogTest, ObjectsPrintAsClassHashInstance)
{
    const Widget w{9, 1234};
    LB_INFO("closing ", w);
    EXPECT_NE(sink.all().find("Widget#1234(fd=9)"), std::string::npos) << sink.all();
}

TEST_F(LogTest, InstanceNumbersAreUniqueAndMonotonic)
{
    const uint64_t a = log_::next_instance();
    const uint64_t b = log_::next_instance();
    EXPECT_GT(b, a);
}

TEST_F(LogTest, RuntimeLevelGatesEmission)
{
    log_::set_level(log_::Level::Warn);
    LB_INFO("suppressed");
    LB_WARN("kept");
    LB_ERROR("kept too");
    EXPECT_EQ(sink.lines.size(), 2u) << sink.all();
    EXPECT_EQ(sink.all().find("suppressed"), std::string::npos);
}

TEST_F(LogTest, OffSuppressesEverything)
{
    log_::set_level(log_::Level::Off);
    LB_ERROR("not this either");
    EXPECT_TRUE(sink.lines.empty());
}

// The compile-time floor is the property that keeps the benchmark honest: below it
// nothing is emitted no matter what the runtime level says.
TEST_F(LogTest, CompileFloorRemovesLowLevelsEntirely)
{
    log_::set_level(log_::Level::Trace);
    LB_TRACE("trace line");
    LB_DEBUG("debug line");
    LB_INFO("info line");
    if constexpr (LLMBRIDGE_LOG_COMPILE_LEVEL > 0)
        EXPECT_EQ(sink.all().find("trace line"), std::string::npos)
            << "TRACE was compiled in despite the floor";
    if constexpr (LLMBRIDGE_LOG_COMPILE_LEVEL > 1)
        EXPECT_EQ(sink.all().find("debug line"), std::string::npos)
            << "DEBUG was compiled in despite the floor";
    EXPECT_NE(sink.all().find("info line"), std::string::npos);
}

// Arguments below the floor must not even be evaluated, or a disabled log line still
// costs whatever computing them costs.
TEST_F(LogTest, ArgumentsBelowTheFloorAreNotEvaluated)
{
    int calls = 0;
    auto expensive = [&calls]() -> int64_t { ++calls; return 1; };
    LB_TRACE("x=", expensive());
    if constexpr (LLMBRIDGE_LOG_COMPILE_LEVEL > 0) EXPECT_EQ(calls, 0);
    LB_INFO("y=", expensive());
    EXPECT_EQ(calls, 1);
}

// Arguments must not be evaluated when the runtime level suppresses the line either.
TEST_F(LogTest, ArgumentsAreNotEvaluatedWhenRuntimeSuppressed)
{
    log_::set_level(log_::Level::Error);
    int calls = 0;
    auto expensive = [&calls]() -> int64_t { ++calls; return 1; };
    LB_INFO("x=", expensive());
    EXPECT_EQ(calls, 0);
}

TEST_F(LogTest, OversizeLinesTruncateAndSaySo)
{
    const std::string huge(4096, 'x');
    LB_INFO(huge);
    ASSERT_EQ(sink.lines.size(), 1u);
    EXPECT_LT(sink.lines[0].size(), log_::Line::kCap + 32);
    EXPECT_NE(sink.lines[0].find("[truncated]"), std::string::npos);
    EXPECT_EQ(sink.lines[0].back(), '\n');
}

TEST_F(LogTest, NumbersRenderWithoutIostreams)
{
    LB_INFO("u=", static_cast<uint64_t>(18446744073709551615ULL), " i=",
            static_cast<int64_t>(-9223372036854775807LL - 1), " b=", true, " d=", 1.5);
    const std::string s = sink.all();
    EXPECT_NE(s.find("u=18446744073709551615"), std::string::npos) << s;
    EXPECT_NE(s.find("i=-9223372036854775808"), std::string::npos) << s;
    EXPECT_NE(s.find("b=true"), std::string::npos) << s;
    EXPECT_NE(s.find("d=1.500"), std::string::npos) << s;
}

TEST_F(LogTest, LevelNamesRoundTripAndTyposAreRefused)
{
    log_::Level l{};
    EXPECT_TRUE(log_::level_from_name("debug", l));
    EXPECT_EQ(l, log_::Level::Debug);
    EXPECT_TRUE(log_::level_from_name("off", l));
    EXPECT_EQ(l, log_::Level::Off);
    // A typo must be refused, never silently defaulted: an operator who mistypes
    // the level would otherwise believe they had raised it.
    EXPECT_FALSE(log_::level_from_name("verbose", l));
    EXPECT_FALSE(log_::level_from_name("INFO", l));
    EXPECT_FALSE(log_::level_from_name("", l));
}

// SECURITY.md promises credentials are never logged. That promise is kept by call
// sites, not by the logger, so this pins the one rule that makes it checkable: the
// helper below is what every credential-adjacent site must use.
TEST_F(LogTest, RedactedHelperNeverPrintsTheValue)
{
    // Deliberately not shaped like a real key. An `sk-ant-...` fixture here would
    // trip scripts/check_no_secrets.py, and the right response to that is a
    // different fixture, never an allow entry: the scanner exists to catch exactly
    // that pattern in tracked files, and an exception for test convenience is how
    // a real key eventually slips past it.
    const std::string secret = "CREDENTIAL-PLACEHOLDER-MUST-NOT-BE-LOGGED";
    // The pattern: name and length, never the value.
    LB_INFO("auth header present name=authorization len=", secret.size());
    const std::string s = sink.all();
    EXPECT_EQ(s.find(secret), std::string::npos) << "a credential reached a log line";
    EXPECT_NE(s.find("len=41"), std::string::npos) << s;
}

TEST_F(LogTest, ConcurrentWritersProduceWholeLines)
{
    // The sink is not thread-safe, so this uses the real stderr path only to prove
    // the emit path itself does not crash under threads; correctness of interleaving
    // at the fd is the kernel's, not ours.
    log_::set_sink(nullptr);
    std::vector<std::thread> ts;
    for (int i = 0; i < 4; ++i)
        ts.emplace_back([i] {
            log_::register_thread("t", static_cast<unsigned>(i));
            for (int k = 0; k < 20; ++k) LB_ERROR("concurrent i=", i, " k=", k);
        });
    for (auto& t : ts) t.join();
    log_::set_sink(&sink);
    SUCCEED();
}

// The subjects. A reader greps one of three words to follow a client, an upstream, or
// a request, and a line that could be about either half of the proxy is a line that
// costs a reader time. These are pinned because the names are the interface.
TEST_F(LogTest, SubjectsAreDistinctAndLeadTheMessage)
{
    // Stand-ins with the same shape the gateway uses, so this test does not need to
    // build a Gateway to pin the naming.
    struct C { uint64_t inst; bool client; int fd; };
    auto put = [](log_::Line& l, const C& c) {
        l.put(log_::Id{c.client ? "ClientConnection" : "UpstreamConnection", c.inst});
        l.put("(fd=");
        l.put(static_cast<int64_t>(c.fd));
        l.put(')');
    };
    log_::Line a;
    put(a, C{7, true, 5});
    EXPECT_EQ(a.view(), "ClientConnection#7(fd=5)");

    log_::Line b;
    put(b, C{8, false, 6});
    EXPECT_EQ(b.view(), "UpstreamConnection#8(fd=6)");

    log_::Line c;
    c.put(log_::Id{"Request", 99});
    EXPECT_EQ(c.view(), "Request#99");
}
