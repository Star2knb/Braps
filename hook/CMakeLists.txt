# rec_hook64.dll / rec_hook32.dll (recorder plan §3.1, §18): loaded into the game by rec.exe.
# Static CRT (it lives in other people's processes) and no dependency on rec_core or spdlog:
# the hook logs through a shared-memory ring (hlog.cpp), the host formats it.
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(REC_HOOK_NAME rec_hook64)
else()
    set(REC_HOOK_NAME rec_hook32)
endif()

# Capture shaders: compiled offline by fxc (Windows SDK) into byte-code headers (plan §18).
file(GLOB _sdk_fxc "$ENV{ProgramFiles\(x86\)}/Windows Kits/10/bin/*/x64/fxc.exe")
list(SORT _sdk_fxc)
list(REVERSE _sdk_fxc)  # newest SDK first
find_program(REC_FXC NAMES fxc HINTS ${_sdk_fxc})
if(NOT REC_FXC)
    message(FATAL_ERROR "fxc.exe (HLSL compiler, part of the Windows SDK) not found")
endif()
set(REC_SHADER_DIR ${CMAKE_CURRENT_BINARY_DIR}/shaders)
file(MAKE_DIRECTORY ${REC_SHADER_DIR})
set(REC_HLSL ${CMAKE_CURRENT_SOURCE_DIR}/shaders/capture.hlsl)
set(REC_SHADER_HEADERS "")
foreach(entry IN ITEMS "vs_main|vs_4_0|g_capture_vs" "ps_nv12|ps_4_0|g_capture_ps_nv12")
    string(REPLACE "|" ";" parts "${entry}")
    list(GET parts 0 e)
    list(GET parts 1 profile)
    list(GET parts 2 var)
    set(out ${REC_SHADER_DIR}/${e}.h)
    add_custom_command(OUTPUT ${out}
        COMMAND ${REC_FXC} /nologo /O3 /T ${profile} /E ${e} /Vn ${var} /Fh ${out} ${REC_HLSL}
        DEPENDS ${REC_HLSL}
        COMMENT "fxc ${e} (${profile})"
        VERBATIM)
    list(APPEND REC_SHADER_HEADERS ${out})
endforeach()

add_library(${REC_HOOK_NAME} SHARED
    hook_main.cpp
    hlog.cpp
    locate.cpp
    dxgi_hook.cpp
    capture_d3d11.cpp
    ${REC_SHADER_HEADERS}
)
target_link_libraries(${REC_HOOK_NAME} PRIVATE rec_common_headers minhook kiero psapi user32)
target_include_directories(${REC_HOOK_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${REC_SHADER_DIR})
set_target_properties(${REC_HOOK_NAME} PROPERTIES
    MSVC_RUNTIME_LIBRARY "${REC_HOOK_CRT}"
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
