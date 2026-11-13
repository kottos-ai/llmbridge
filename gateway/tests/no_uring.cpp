// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

// Runs a command with one io_uring syscall refused with `EPERM` by a seccomp filter, as
// Docker's default profile refuses all three, so a test sees what the gateway does where
// io_uring is blocked. Exits 77 without running it where io_uring is unavailable anyway,
// so the test is skipped instead of passing for another reason.
//
//   no_uring setup|register command [args...]

#include <linux/filter.h>
#include <linux/io_uring.h>
#include <linux/seccomp.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstring>

int main(int argc, char** argv)
{
    const bool setup = argc >= 3 && std::strcmp(argv[1], "setup") == 0;
    if (argc < 3 || (!setup && std::strcmp(argv[1], "register") != 0))
    {
        std::fprintf(stderr, "usage: no_uring setup|register COMMAND [ARGS...]\n");
        return 2;
    }
    io_uring_params p{};
    const long fd = ::syscall(__NR_io_uring_setup, 4, &p);
    if (fd < 0)
    {
        std::printf("no_uring: io_uring is unavailable here even without a filter (%s)\n",
                    std::strerror(errno));
        return 77;
    }
    ::close(static_cast<int>(fd));

    // The native syscall number only: a test helper, not a sandbox.
    const unsigned nr = setup ? __NR_io_uring_setup : __NR_io_uring_register;
    sock_filter prog[] = {
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, nr)),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, nr, 0, 1),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
    };
    sock_fprog fprog{static_cast<unsigned short>(sizeof prog / sizeof prog[0]), prog};
    if (::prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0 ||
        ::prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &fprog) != 0)
    {
        std::perror("no_uring: seccomp");
        return 2;
    }
    ::execvp(argv[2], argv + 2);
    std::perror("no_uring: exec");
    return 127;
}
