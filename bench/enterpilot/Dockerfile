# Copyright 2026 Kottos AI, Inc.
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# llmbridge for the ENTERPILOT gateway benchmark. Release build, io_uring default,
# no TLS: every gateway in that harness talks plain HTTP to a mock on the same
# Docker network, so a TLS build would measure OpenSSL and nothing else.
FROM ubuntu:24.04 AS build
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential cmake git ca-certificates \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY . .
# Out of tree, at an absolute path: the host's own build/ can arrive in the context
# (the classic builder ignores a Dockerfile.dockerignore) and CMake then refuses the
# stale cache because it names a different source directory.
RUN cmake -S /src -B /out -DCMAKE_BUILD_TYPE=Release -DLLMBRIDGE_TLS=OFF \
          -DLLMBRIDGE_BUILD_TESTS=OFF -DLLMBRIDGE_BUILD_TOOLS=OFF \
    && cmake --build /out -j"$(nproc)" --target llmbridge

FROM ubuntu:24.04
RUN apt-get update && apt-get install -y --no-install-recommends ca-certificates \
    && rm -rf /var/lib/apt/lists/*
COPY --from=build /out/bin/llmbridge /usr/local/bin/llmbridge
EXPOSE 8080
ENTRYPOINT ["/usr/local/bin/llmbridge"]
