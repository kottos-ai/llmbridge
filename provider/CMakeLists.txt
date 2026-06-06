# provider — dialect translation (OpenAI <-> provider-native), the C++ analog of
# LiteLLM's transform_request/transform_response. Pure JSON/string work, no deps
# (hand-rolled JSON in include/provider/json.hpp). The gateway links this when
# translation is enabled.

add_library(provider STATIC
        src/translate.cpp
)
target_include_directories(provider PUBLIC include)
target_compile_features(provider PUBLIC cxx_std_20)

if (KOTTOS_BUILD_TESTS)
    add_subdirectory(tests)
endif ()
