cmake_minimum_required(VERSION 3.25)
project(rec LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    add_compile_options(/W4 /permissive- /utf-8 /Zc:__cplusplus)
    if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
        add_compile_options(/Zc:preprocessor)
    endif()
    # Release: /O2 /Oi + whole-program optimisation (/GL + /LTCG), codec plan §3.2.
    add_compile_options($<$<CONFIG:Release>:/Oi>)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
endif()
enable_testing()

# The hook DLLs live inside other people's processes: static CRT, and so does everything they link.
set(REC_HOOK_CRT "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_subdirectory(third_party)
add_subdirectory(common)

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    # x64: codec, host (rec_core + rec.exe), the 64-bit hook DLL, the test app, tests.
    # rec.exe, rec_hook64.dll and rec_testapp.exe all land in <build>/bin: the host looks for the
    # hook DLL next to itself.
    add_subdirectory(codec)
    add_subdirectory(core)
    add_subdirectory(hook)
    add_subdirectory(cli)
    add_subdirectory(testapp)
    add_subdirectory(tests)
else()
    # x86 (x86-* presets): only the 32-bit parts (hook DLL, injector helper), which talk to the x64 host through `common`
    # (recorder plan §2, X2). The codec is x64 only (codec plan §3.2).
    add_subdirectory(hook)
    add_subdirectory(inject32)
endif()
