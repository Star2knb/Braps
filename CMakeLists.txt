cmake_minimum_required(VERSION 3.20)
project(Braps LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

add_executable(Braps
    src/main.cpp
    src/Recorder.cpp
    src/CaptureDXGI.cpp
    src/CaptureGDI.cpp
    src/Encoder.cpp
    src/Hotkeys.cpp
    src/Overlay.cpp
    hook/src/Injector.cpp
    hook/src/CaptureHook.cpp
)

target_include_directories(Braps PRIVATE include hook/include)

target_link_libraries(Braps PRIVATE
    d3d11
    dxgi
    User32
    Gdi32
    Shcore
    winmm
)

target_compile_definitions(Braps PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)

# BrapsHook.dll: injected into a target game's process to hook
# IDXGISwapChain::Present directly (Fraps-style capture), for
# single-player/non-anticheat games where this yields much lower capture
# overhead than the DXGI Desktop Duplication path Braps.exe uses on its own.
add_library(BrapsHook SHARED
    hook/src/dllmain.cpp
    hook/src/D3D11Hook.cpp
)

target_include_directories(BrapsHook PRIVATE include hook/include)

target_link_libraries(BrapsHook PRIVATE
    d3d11
    dxgi
    User32
)

target_compile_definitions(BrapsHook PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)

# So Braps.exe can find BrapsHook.dll next to itself at runtime without the
# user needing to know or pass its path manually.
add_dependencies(Braps BrapsHook)
add_custom_command(TARGET Braps POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:BrapsHook>" "$<TARGET_FILE_DIR:Braps>/BrapsHook.dll"
)

# Bundle the FFmpeg binary next to the built executable so the app can
# spawn it for deferred MP4 encoding without requiring it on PATH.
set(FFMPEG_BIN "${CMAKE_SOURCE_DIR}/third_party/ffmpeg/bin/ffmpeg.exe")
if(EXISTS "${FFMPEG_BIN}")
    add_custom_command(TARGET Braps POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${FFMPEG_BIN}" "$<TARGET_FILE_DIR:Braps>/ffmpeg.exe"
    )
else()
    message(WARNING "ffmpeg.exe not found at ${FFMPEG_BIN} - extract the FFmpeg build there before running Braps. See README.md.")
endif()
