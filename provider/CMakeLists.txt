# provider: dialect translation (OpenAI <-> provider-native), the C++ analog of
# LiteLLM's transform_request/transform_response. Pure JSON/string work, no deps
# (hand-rolled JSON in include/provider/json.hpp). The gateway links this when
# translation is enabled.

add_library(provider STATIC
        src/translate.cpp
        src/sse.cpp
)
add_library(llmbridge::provider ALIAS provider) # namespaced alias, matches find_package()
target_include_directories(provider PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_compile_features(provider PUBLIC cxx_std_20)

# Install the embeddable translator library + its public headers. Top-level builds
# only: a consumer that add_subdirectory()s llmbridge doesn't want our install().
if (PROJECT_IS_TOP_LEVEL)
    install(TARGETS provider EXPORT llmbridgeTargets
            ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
    install(DIRECTORY include/provider DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
endif ()

if (LLMBRIDGE_BUILD_TESTS)
    add_subdirectory(tests)
endif ()
