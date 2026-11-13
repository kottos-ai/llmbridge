// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

// IoBackend::Uring where io_uring is blocked: run() returns 1 at once and never serves
// epoll, while Auto still does. ctest runs this only under no_uring; run bare, it skips.

#include "gateway/gateway.hpp"
#include "net/uring.hpp"

#include <gtest/gtest.h>

#include <sys/syscall.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <thread>

namespace
{
    using llmbridge::Gateway;
    using llmbridge::IoBackend;

    // run()'s code if it returned within 3 s, else -1: it was serving when stopped.
    int run_briefly(IoBackend io)
    {
        Gateway gw(0, "127.0.0.1", 9, 0, llmbridge::UpstreamDialect::OpenAI, io);
        std::atomic<int> rc{-1};
        std::atomic<bool> done{false};
        std::thread t([&] {
            rc = gw.run();
            done = true;
        });
        for (int i = 0; i < 300 && !done; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        const bool serving = !done;
        gw.request_stop();
        t.join();
        return serving ? -1 : rc.load();
    }

    // no_uring register lets setup through and refuses every register with `EPERM`.
    bool register_blocked()
    {
        return ::syscall(__NR_io_uring_register, -1, 0, nullptr, 0) < 0 && errno == EPERM;
    }

    TEST(UringStrict, SetupBlockedUringRefuses)
    {
        if (llmbridge::net::uring::available()) GTEST_SKIP() << "io_uring is usable; run under no_uring setup";
        EXPECT_EQ(run_briefly(IoBackend::Uring), 1);
    }

    TEST(UringStrict, SetupBlockedAutoServesEpoll)
    {
        if (llmbridge::net::uring::available()) GTEST_SKIP() << "io_uring is usable; run under no_uring setup";
        EXPECT_EQ(run_briefly(IoBackend::Auto), -1);
    }

    // Setup works and the provided-buffer ring cannot be registered: the second place
    // the backend can fail, inside run().
    TEST(UringStrict, RegisterBlockedUringRefuses)
    {
        if (!register_blocked()) GTEST_SKIP() << "io_uring_register is allowed; run under no_uring register";
        EXPECT_EQ(run_briefly(IoBackend::Uring), 1);
    }

    TEST(UringStrict, RegisterBlockedAutoServesEpoll)
    {
        if (!register_blocked()) GTEST_SKIP() << "io_uring_register is allowed; run under no_uring register";
        EXPECT_EQ(run_briefly(IoBackend::Auto), -1);
    }
} // namespace
