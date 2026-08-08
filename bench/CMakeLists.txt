# bench: benchmark tooling. The open-loop load generator and the instant C++
# backend for the saturation sweep. loadgen uses the vendored Histogram + clock
# from the gateway module; fastbackend only needs net (sockets + framing).
# Python mock + orchestration scripts live alongside (not built by CMake).

add_executable(loadgen loadgen.cpp)
target_link_libraries(loadgen PRIVATE net gateway)

add_executable(fastbackend fastbackend.cpp)
target_link_libraries(fastbackend PRIVATE net)

# streamgen/faststream: the Phase-B (SSE) pair. streamgen reuses the vendored
# Histogram+clock; faststream is standalone (raw epoll, no llmbridge headers) so the
# provider under measurement shares nothing with the code being measured.
add_executable(streamgen streamgen.cpp)
target_link_libraries(streamgen PRIVATE net gateway)

add_executable(faststream faststream.cpp)

# nullrelay: the zero-work proxy, the control that separates "cost of the hop" from
# "cost of the gateway's work". See the comment at the top of nullrelay.cpp.
add_executable(nullrelay nullrelay.cpp)
