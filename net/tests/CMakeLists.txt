# GoogleTest is made available once at the root (FetchContent) under
# LLMBRIDGE_BUILD_TESTS; modules just consume GTest::gtest_main.

add_executable(net_http_test http_test.cpp)
target_link_libraries(net_http_test PRIVATE net GTest::gtest_main)

add_executable(net_http_stream_test http_stream_test.cpp)
target_link_libraries(net_http_stream_test PRIVATE net GTest::gtest_main)

add_executable(net_socket_util_test socket_util_test.cpp)
target_link_libraries(net_socket_util_test PRIVATE net GTest::gtest_main)

add_executable(net_uring_test uring_test.cpp)
target_link_libraries(net_uring_test PRIVATE net GTest::gtest_main)

add_executable(net_upstream_test upstream_test.cpp)
target_link_libraries(net_upstream_test PRIVATE net GTest::gtest_main)

add_executable(net_secure_test secure_test.cpp)
target_link_libraries(net_secure_test PRIVATE net GTest::gtest_main)


include(GoogleTest)
gtest_discover_tests(net_http_test)
gtest_discover_tests(net_http_stream_test)
gtest_discover_tests(net_socket_util_test)
gtest_discover_tests(net_uring_test)
gtest_discover_tests(net_upstream_test)
gtest_discover_tests(net_secure_test)

# Only when TLS is compiled in; the file is #ifdef'd but there is no point
# building an empty translation unit.
if (LLMBRIDGE_TLS)
    add_executable(net_tls_test tls_test.cpp)
    target_link_libraries(net_tls_test PRIVATE net GTest::gtest_main OpenSSL::SSL OpenSSL::Crypto)
    gtest_discover_tests(net_tls_test)
endif ()

# The logger. Its own target so a private repo can swap the sink without touching
# these tests: they exercise the SEAM (Sink), never the built-in stderr path.
add_executable(net_log_test log_test.cpp)
target_link_libraries(net_log_test PRIVATE net GTest::gtest_main)
gtest_discover_tests(net_log_test)
