cmake_minimum_required(VERSION 3.25)
project(rec LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "The codec is x64 only (codec plan §3.2). Use an x64 developer environment.")
endif()

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

add_subdirectory(codec)
