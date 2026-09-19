# GoogleTest comes from the root (FetchContent under LLMBRIDGE_BUILD_TESTS).
# One executable per class/concern.

add_executable(gateway_metrics_test metrics_test.cpp)
target_link_libraries(gateway_metrics_test PRIVATE gateway GTest::gtest_main)

add_executable(gateway_stats_test stats_test.cpp)
target_link_libraries(gateway_stats_test PRIVATE gateway GTest::gtest_main)

add_executable(gateway_test gateway_test.cpp)
target_link_libraries(gateway_test PRIVATE gateway GTest::gtest_main)

add_executable(gateway_dialect_test dialect_test.cpp)
target_link_libraries(gateway_dialect_test PRIVATE gateway GTest::gtest_main)

# 100 clients x 10 real question/answer pairs, on both event-loop backends.
# The corpus is a recorded fixture (scripts/gen_qa_corpus.py); the test is
# hermetic. LLMBRIDGE_TEST_DATA_DIR points at it so the test does not depend on
# the working directory ctest happens to run from.
add_executable(gateway_rebuild_test rebuild_test.cpp)
target_link_libraries(gateway_rebuild_test PRIVATE gateway GTest::gtest_main)
# The rebuild is a private helper under src/, reached here and nowhere else.
target_include_directories(gateway_rebuild_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
add_executable(gateway_corpus_test gateway_corpus_test.cpp)
target_link_libraries(gateway_corpus_test PRIVATE gateway GTest::gtest_main)
target_compile_definitions(gateway_corpus_test
    PRIVATE LLMBRIDGE_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")

# auth_headers_for is declared in request.hpp but the header lives under src/,
# same reason gateway_rebuild_test needs the include path below.
add_executable(gateway_request_test request_test.cpp)
target_link_libraries(gateway_request_test PRIVATE gateway GTest::gtest_main)
target_include_directories(gateway_request_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)

# note_served_tier is a public inline in stream.hpp, but stream.hpp lives
# under src/, same reason as gateway_rebuild_test.
add_executable(gateway_served_tier_test served_tier_test.cpp)
target_link_libraries(gateway_served_tier_test PRIVATE gateway GTest::gtest_main)
target_include_directories(gateway_served_tier_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)

# The perf half. Built by default, but deliberately never registered with
# gtest_discover_tests below: ctest must not run it, so shared-runner jitter
# cannot fail a PR on a timing number. Run ./bin/gateway_request_perf_test by
# hand or from a nightly job.
add_executable(gateway_request_perf_test request_perf_test.cpp)
target_link_libraries(gateway_request_perf_test PRIVATE gateway)
target_include_directories(gateway_request_perf_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)

include(GoogleTest)
gtest_discover_tests(gateway_metrics_test)
gtest_discover_tests(gateway_stats_test)
gtest_discover_tests(gateway_test)
gtest_discover_tests(gateway_dialect_test)
# 1000 requests per case; generous timeout so a slow CI runner does not flake it.
# RUN_SERIAL: each case drives 1000 requests across 100 threads. Run concurrently
# with the rest of the suite under `ctest -j`, it both flakes and reports
# meaningless latencies; the numbers it prints are only worth reading when it
# has the machine to itself.
gtest_discover_tests(gateway_rebuild_test)
gtest_discover_tests(gateway_request_test)
gtest_discover_tests(gateway_served_tier_test)
gtest_discover_tests(gateway_corpus_test PROPERTIES TIMEOUT 600 RUN_SERIAL TRUE)

# End-to-end TLS tests: only when the TLS transport is compiled in. The mock
# provider in the test uses OpenSSL directly (SSL_set_fd is fine in test code).
if (LLMBRIDGE_TLS)
    add_executable(gateway_tls_test gateway_tls_test.cpp)
    target_link_libraries(gateway_tls_test PRIVATE gateway GTest::gtest_main OpenSSL::SSL OpenSSL::Crypto)
    gtest_discover_tests(gateway_tls_test)
endif ()
