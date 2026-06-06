# GoogleTest is made available once at the root (FetchContent) under
# LLMBRIDGE_BUILD_TESTS; modules just consume GTest::gtest_main.

add_executable(net_http_test http_test.cpp)
target_link_libraries(net_http_test PRIVATE net GTest::gtest_main)

add_executable(net_socket_util_test socket_util_test.cpp)
target_link_libraries(net_socket_util_test PRIVATE net GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(net_http_test)
gtest_discover_tests(net_socket_util_test)
