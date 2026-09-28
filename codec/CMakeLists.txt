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
    src/threadpool.cpp
    src/skipmap.cpp
    src/colour.cpp
    src/nearlossless.cpp
    src/encoder.cpp
    src/decoder.cpp
)

# Per-file ISA flags (§3.2/§3.3): everything else stays at the x64 baseline. MSVC accepts
# SSE4.1 intrinsics without a flag; clang-cl needs -msse4.1. Both take /arch:AVX2.
set_source_files_properties(src/predict_avx2.cpp src/huffman_avx2.cpp PROPERTIES COMPILE_OPTIONS "/arch:AVX2")
if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    set_source_files_properties(src/predict_sse41.cpp PROPERTIES COMPILE_OPTIONS "-msse4.1")
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
)
# Tests exercise internal kernels too, so they see the private headers.
target_include_directories(rcv_tests PRIVATE src)
target_link_libraries(rcv_tests PRIVATE rcv)

add_test(NAME rcv_tests COMMAND rcv_tests)
set_tests_properties(rcv_tests PROPERTIES TIMEOUT 600)  # a threading deadlock fails instead of hanging
