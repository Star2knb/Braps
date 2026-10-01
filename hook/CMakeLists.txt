# rec_hook64.dll / rec_hook32.dll (recorder plan §3.1, §18): loaded into the game by rec.exe.
# Static CRT (it lives in other people's processes) and no dependency on rec_core or spdlog:
# the hook logs through a shared-memory ring (hlog.cpp), the host formats it.
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(REC_HOOK_NAME rec_hook64)
else()
    set(REC_HOOK_NAME rec_hook32)
endif()

add_library(${REC_HOOK_NAME} SHARED
    hook_main.cpp
    hlog.cpp
    locate.cpp
    dxgi_hook.cpp
)
target_link_libraries(${REC_HOOK_NAME} PRIVATE rec_common_headers minhook kiero psapi user32)
target_include_directories(${REC_HOOK_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
set_target_properties(${REC_HOOK_NAME} PROPERTIES
    MSVC_RUNTIME_LIBRARY "${REC_HOOK_CRT}"
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
