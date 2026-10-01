# Recorder tests. They use the codec's small harness (codec/tests/testfw.h, D-003) and its main.
add_executable(rec_tests
    ${PROJECT_SOURCE_DIR}/codec/tests/test_main.cpp
    test_events.cpp
    test_config.cpp
    test_logging.cpp
)
target_include_directories(rec_tests PRIVATE ${PROJECT_SOURCE_DIR}/codec/tests)
target_link_libraries(rec_tests PRIVATE rec_core)

add_test(NAME rec_tests COMMAND rec_tests)
set_tests_properties(rec_tests PROPERTIES TIMEOUT 300)
