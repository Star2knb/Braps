# Vendored libraries (see README.md here for versions). Their headers are SYSTEM includes, so our
# /W4 applies to our code only; spdlog's own sources compile without our warning level.
# MinHook and kiero2 are added with the hook DLLs (recorder M1).

# Keep the language options from the root, drop /W4 for third-party sources.
get_directory_property(_rec_opts COMPILE_OPTIONS)
list(REMOVE_ITEM _rec_opts /W4)
set_directory_properties(PROPERTIES COMPILE_OPTIONS "${_rec_opts}")

# CLI11 (header-only)
add_library(cli11 INTERFACE)
target_include_directories(cli11 SYSTEM INTERFACE CLI11/include)

# toml++ (single header)
add_library(tomlplusplus INTERFACE)
target_include_directories(tomlplusplus SYSTEM INTERFACE tomlplusplus)

# spdlog, compiled library mode with the bundled {fmt}. Wide-character file names, so log paths
# under a non-ASCII user profile work.
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    add_library(spdlog STATIC
        spdlog/src/spdlog.cpp
        spdlog/src/stdout_sinks.cpp
        spdlog/src/color_sinks.cpp
        spdlog/src/file_sinks.cpp
        spdlog/src/async.cpp
        spdlog/src/cfg.cpp
        spdlog/src/bundled_fmtlib_format.cpp)
    target_include_directories(spdlog SYSTEM PUBLIC spdlog/include)
    target_compile_definitions(spdlog PUBLIC SPDLOG_COMPILED_LIB SPDLOG_WCHAR_FILENAMES)
endif()
