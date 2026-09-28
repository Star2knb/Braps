# RCV1 codec (RCV1_Codec_Plan.md). Core has no dependencies beyond the C++ standard library.

set(RCV_SOURCES
    src/format.cpp
    src/huffman.cpp
    src/predict_scalar.cpp
    src/crc32c.cpp
    src/encoder.cpp
    src/decoder.cpp
)

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
)
# Tests exercise internal kernels too, so they see the private headers.
target_include_directories(rcv_tests PRIVATE src)
target_link_libraries(rcv_tests PRIVATE rcv)

add_test(NAME rcv_tests COMMAND rcv_tests)
