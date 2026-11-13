# Copyright 2026 Kottos AI, Inc.
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# The release image, ghcr.io/kottos-ai/llmbridge, published by .github/workflows/image.yml.
# A Release build with TLS, so it can reach https:// providers and terminate inbound TLS,
# on the Ubuntu 24.04 it is developed and tested on. A plain http:// upstream never
# touches OpenSSL.
#
# It serves on io_uring or not at all. Docker's default seccomp profile blocks io_uring,
# and an image cannot lift that: only `docker run --security-opt` can. So the entrypoint
# passes --io uring, which refuses to start and names the option instead of serving the
# slower epoll backend (README, "Running in a container"). `--io epoll` after the image
# name serves on epoll on purpose: the last --io wins.
FROM ubuntu:24.04 AS build
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential cmake libssl-dev ca-certificates \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY . .
RUN cmake -S /src -B /out -DCMAKE_BUILD_TYPE=Release -DLLMBRIDGE_TLS=ON \
          -DLLMBRIDGE_BUILD_TESTS=OFF -DLLMBRIDGE_BUILD_TOOLS=OFF \
    && cmake --build /out -j"$(nproc)" --target llmbridge

FROM ubuntu:24.04
RUN apt-get update && apt-get install -y --no-install-recommends libssl3t64 ca-certificates \
    && rm -rf /var/lib/apt/lists/*
COPY --from=build /out/bin/llmbridge /usr/local/bin/llmbridge
# Set, or the base image's "24.04" is what anything reading the label reports as our version.
ARG VERSION=unreleased
LABEL org.opencontainers.image.source="https://github.com/kottos-ai/llmbridge" \
      org.opencontainers.image.version="${VERSION}" \
      org.opencontainers.image.licenses="Apache-2.0" \
      org.opencontainers.image.description="OpenAI and Anthropic API translator and gateway, in C++20"
# The image's own unprivileged user. Nothing is written at runtime.
USER ubuntu
EXPOSE 8088
ENTRYPOINT ["/usr/local/bin/llmbridge", "--io", "uring"]
