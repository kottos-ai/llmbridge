# bench — benchmark tooling: the open-loop load generator and the instant C++
# backend for the saturation sweep. loadgen uses the vendored Histogram + clock
# from the gateway module; fastbackend only needs net (sockets + framing).
# Python mock + orchestration scripts live alongside (not built by CMake).

add_executable(loadgen loadgen.cpp)
target_link_libraries(loadgen PRIVATE net gateway)

add_executable(fastbackend fastbackend.cpp)
target_link_libraries(fastbackend PRIVATE net)
