# Vendored libraries (see README.md here for versions). Their headers are SYSTEM includes, so our
# /W4 applies to our code only; spdlog's own sources compile without our warning level.

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

# MinHook and kiero2 are linked into the hook DLLs only. Those DLLs live inside other people's
# processes, so they use the static CRT (REC_HOOK_CRT, set in the root CMakeLists) and every
# library they link must match.

# MinHook: x64 uses the 64-bit disassembler, x86 the 32-bit one.
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(_hde minhook/src/hde/hde64.c)
else()
    set(_hde minhook/src/hde/hde32.c)
endif()
add_library(minhook STATIC minhook/src/buffer.c minhook/src/hook.c minhook/src/trampoline.c ${_hde})
target_include_directories(minhook SYSTEM PUBLIC minhook/include)
set_target_properties(minhook PROPERTIES MSVC_RUNTIME_LIBRARY "${REC_HOOK_CRT}")

# kiero2 (recorder plan §4.6.1): only the D3D11 backend for now (D3D9 and D3D12 come with M8 and
# M10). The enum values of its Implementation_* constants depend on header include order, so the
# slot of each backend source below must match hook/kiero_wrap.hpp (D3D9, D3D11, D3D12, in that
# order, counting only the enabled ones). hook/kiero_config.h is force-included to route kiero2's
# debug output and assertions into the hook's log ring.
add_library(kiero STATIC kiero2/kiero_intern.cpp kiero2/kiero_d3d11.cpp)
set_source_files_properties(kiero2/kiero_d3d11.cpp PROPERTIES COMPILE_DEFINITIONS "KIERO_IMPL_FIRST_SLOT=0")
target_include_directories(kiero SYSTEM PUBLIC kiero2)
target_include_directories(kiero PRIVATE ${PROJECT_SOURCE_DIR}/hook)
target_compile_options(kiero PRIVATE /FIkiero_config.h)
target_compile_definitions(kiero PUBLIC KIERO_BUILD_D3D11=1)
target_link_libraries(kiero PUBLIC rec_common_headers user32)
set_target_properties(kiero PROPERTIES MSVC_RUNTIME_LIBRARY "${REC_HOOK_CRT}")
