# GoogleTest comes from the root (FetchContent under LLMBRIDGE_BUILD_TESTS).
# One executable per class/concern.

add_executable(gateway_metrics_test metrics_test.cpp)
target_link_libraries(gateway_metrics_test PRIVATE gateway GTest::gtest_main)

add_executable(gateway_stats_test stats_test.cpp)
target_link_libraries(gateway_stats_test PRIVATE gateway GTest::gtest_main)

add_executable(gateway_test gateway_test.cpp)
target_link_libraries(gateway_test PRIVATE gateway GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(gateway_metrics_test)
gtest_discover_tests(gateway_stats_test)
gtest_discover_tests(gateway_test)

# End-to-end TLS tests: only when the TLS transport is compiled in. The mock
# provider in the test uses OpenSSL directly (SSL_set_fd is fine in test code).
if (LLMBRIDGE_TLS)
    add_executable(gateway_tls_test gateway_tls_test.cpp)
    target_link_libraries(gateway_tls_test PRIVATE gateway GTest::gtest_main OpenSSL::SSL OpenSSL::Crypto)
    gtest_discover_tests(gateway_tls_test)
endif ()
