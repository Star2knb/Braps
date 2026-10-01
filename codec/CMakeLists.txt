# RCV1 codec (RCV1_Codec_Plan.md). Core has no dependencies beyond the C++ standard library.

set(RCV_SOURCES
    src/format.cpp
    src/huffman.cpp
    src/huffman_avx2.cpp
    src/cpu.cpp
    src/predict_scalar.cpp
    src/predict_sse41.cpp
    src/predict_avx2.cpp
    src/crc32c.cpp
    src/crc32c_sse42.cpp
    src/threadpool.cpp
    src/skipmap.cpp
    src/colour.cpp
    src/nearlossless.cpp
    src/encoder.cpp
    src/decoder.cpp
)

# Per-file ISA flags (§3.2/§3.3): everything else stays at the x64 baseline. MSVC accepts
# SSE4.x intrinsics without a flag; clang-cl needs -msse4.1 / -msse4.2. Both take /arch:AVX2.
set_source_files_properties(src/predict_avx2.cpp src/huffman_avx2.cpp PROPERTIES COMPILE_OPTIONS "/arch:AVX2")
if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    set_source_files_properties(src/predict_sse41.cpp PROPERTIES COMPILE_OPTIONS "-msse4.1")
    set_source_files_properties(src/crc32c_sse42.cpp PROPERTIES COMPILE_OPTIONS "-msse4.2")
endif()

add_library(rcv STATIC ${RCV_SOURCES})
target_include_directories(rcv PUBLIC include PRIVATE src)

add_library(rcv_shared SHARED ${RCV_SOURCES})
target_include_directories(rcv_shared PUBLIC include PRIVATE src)
target_compile_definitions(rcv_shared PRIVATE RCV_SHARED_BUILD INTERFACE RCV_SHARED)

add_executable(rcv_cli tools/rcv_cli.cpp)
target_link_libraries(rcv_cli PRIVATE rcv)

# Benchmark harness (§11.5). Uses the internal stage profiler, so it sees the private headers.
add_executable(rcv_bench bench/bench_main.cpp)
target_include_directories(rcv_bench PRIVATE src)
target_link_libraries(rcv_bench PRIVATE rcv PowrProf)

add_executable(rcv_tests
    tests/test_main.cpp
    tests/test_bitio.cpp
    tests/test_huffman.cpp
    tests/test_predict.cpp
    tests/test_roundtrip.cpp
    tests/test_errors.cpp
    tests/test_windows_h.cpp
    tests/test_simd.cpp
    tests/test_threads.cpp
    tests/test_pframes.cpp
    tests/test_gbr.cpp
    tests/test_near.cpp
    tests/test_validation.cpp
    tests/test_bounds.cpp
    tests/test_alloc.cpp
)
# Tests exercise internal kernels too, so they see the private headers.
target_include_directories(rcv_tests PRIVATE src)
target_link_libraries(rcv_tests PRIVATE rcv)

add_test(NAME rcv_tests COMMAND rcv_tests)
set_tests_properties(rcv_tests PROPERTIES TIMEOUT 600)  # a threading deadlock fails instead of hanging

# Seed corpus for the fuzzer (any compiler): rcv_fuzz_seeds <dir>
add_executable(rcv_fuzz_seeds fuzz/make_seeds.cpp)
target_link_libraries(rcv_fuzz_seeds PRIVATE rcv)

# Decoder fuzzer, libFuzzer + ASan + UBSan (§11.4, A12). clang-cl only: the clang-debug preset.
# The codec sources are compiled again with coverage and sanitizer instrumentation.
if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
    add_executable(rcv_fuzz fuzz/fuzz_decoder.cpp ${RCV_SOURCES})
    target_include_directories(rcv_fuzz PRIVATE include src)
    # The libFuzzer shipped with Visual Studio is built for the static release CRT (/MT), and ASan
    # needs a release CRT anyway. Optimised, so an hour of fuzzing tries more inputs.
    set_property(TARGET rcv_fuzz PROPERTY MSVC_RUNTIME_LIBRARY MultiThreaded)
    target_compile_options(rcv_fuzz PRIVATE -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=all /O2)
    # The prebuilt libFuzzer has no STL container annotations; the objects must match (the codec
    # keeps its data in raw buffers, which ASan checks as usual).
    target_compile_definitions(rcv_fuzz PRIVATE _DISABLE_STRING_ANNOTATION _DISABLE_VECTOR_ANNOTATION)
    execute_process(COMMAND ${CMAKE_CXX_COMPILER} /clang:-print-resource-dir
                    OUTPUT_VARIABLE RCV_CLANG_RESOURCE_DIR OUTPUT_STRIP_TRAILING_WHITESPACE)
    set(RCV_CLANG_RT "${RCV_CLANG_RESOURCE_DIR}/lib/windows")
    # CMake links with lld-link directly, so name the runtimes the clang-cl driver would add.
    target_link_options(rcv_fuzz PRIVATE
        "-wholearchive:${RCV_CLANG_RT}/clang_rt.fuzzer-x86_64.lib"
        "${RCV_CLANG_RT}/clang_rt.asan_dynamic-x86_64.lib"
        "-wholearchive:${RCV_CLANG_RT}/clang_rt.asan_static_runtime_thunk-x86_64.lib")
    add_custom_command(TARGET rcv_fuzz POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${RCV_CLANG_RT}/clang_rt.asan_dynamic-x86_64.dll"
                $<TARGET_FILE_DIR:rcv_fuzz>)
endif()
