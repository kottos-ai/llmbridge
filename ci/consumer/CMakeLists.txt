# Copyright 2026 Kottos AI, Inc.
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0

# A downstream project, built against an INSTALLED llmbridge exactly as the README
# tells a user to. Nothing else in CI exercises the install tree: the test suite
# builds in-tree, so a broken export, a header that does not stand alone, or a
# renamed public namespace all pass everything and only break for the consumer.
#
# v0.9.0 moved `llmbridge::json` to `llmbridge::provider::json` inside an installed
# header. That was deliberate — but nothing verified it, so a downstream user would
# have been the one to discover it.
cmake_minimum_required(VERSION 3.20)
project(llmbridge_consumer CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(llmbridge CONFIG REQUIRED)

add_executable(consumer main.cpp)
target_link_libraries(consumer PRIVATE llmbridge::provider)
