# libFuzzer targets for the two hand-rolled parsers — llmbridge's highest-risk
# surface (untrusted bytes → json.hpp / http.hpp). Clang-only: libFuzzer ships
# with Clang and needs -fsanitize=fuzzer. Enabled via -DLLMBRIDGE_BUILD_FUZZERS=ON.
#
#   cmake -B build-fuzz -DLLMBRIDGE_BUILD_FUZZERS=ON -DCMAKE_CXX_COMPILER=clang++
#   cmake --build build-fuzz
#   ./build-fuzz/fuzz/fuzz_json -max_total_time=120
#   ./build-fuzz/fuzz/fuzz_http -max_total_time=120

if (NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(WARNING "fuzz: libFuzzer needs Clang; skipping (compiler is ${CMAKE_CXX_COMPILER_ID}). "
                    "Configure with -DCMAKE_CXX_COMPILER=clang++ to build fuzzers.")
    return()
endif ()

# json.hpp and http.hpp are header-only; link the module libs purely for their
# PUBLIC include dirs. Sanitize with fuzzer + ASan + UBSan for the sharpest signal.
foreach (tgt json http)
    add_executable(fuzz_${tgt} fuzz_${tgt}.cpp)
    target_compile_options(fuzz_${tgt} PRIVATE -g -O1 -fsanitize=fuzzer,address,undefined)
    target_link_options(fuzz_${tgt} PRIVATE -fsanitize=fuzzer,address,undefined)
endforeach ()

target_link_libraries(fuzz_json PRIVATE provider) # provider/include (json.hpp)
target_link_libraries(fuzz_http PRIVATE net)      # net/include (http.hpp)
