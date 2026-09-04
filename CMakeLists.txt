cmake_minimum_required(VERSION 3.20)
project(Braps LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# CreateRemoteThread + LoadLibraryW injection only works when the injecting
# process matches the target's bitness — a 64-bit process cannot inject
# into a 32-bit target this way (or vice versa). So a 32-bit target needs
# BOTH a 32-bit hook DLL AND a 32-bit process to perform the injection.
# Rather than build a full 32-bit Braps.exe, this same CMakeLists.txt,
# configured for Win32 (`cmake -B build32 -A Win32`), builds ONLY
# BrapsHook32.dll + the tiny BrapsInjector32.exe helper that the main
# (x64) Braps.exe spawns when it detects a 32-bit target. See README.md
# for the two-build setup.
if(CMAKE_SIZEOF_VOID_P EQUAL 4)
    set(BRAPS_BUILDING_32BIT_HELPERS TRUE)
else()
    set(BRAPS_BUILDING_32BIT_HELPERS FALSE)
endif()

if(NOT BRAPS_BUILDING_32BIT_HELPERS)
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
        windowscodecs
        ole32
    )

    target_compile_definitions(Braps PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
endif()

# BrapsHook.dll / BrapsHook32.dll: injected into a target game's process to
# hook IDXGISwapChain::Present directly (Fraps-style capture), for
# single-player/non-anticheat games where this yields much lower capture
# overhead than the DXGI Desktop Duplication path Braps.exe uses on its own.
set(HOOK_TARGET_NAME BrapsHook)
if(BRAPS_BUILDING_32BIT_HELPERS)
    set(HOOK_TARGET_NAME BrapsHook32)
endif()

add_library(${HOOK_TARGET_NAME} SHARED
    hook/src/dllmain.cpp
    hook/src/D3D11Hook.cpp
)

target_include_directories(${HOOK_TARGET_NAME} PRIVATE include hook/include)

target_link_libraries(${HOOK_TARGET_NAME} PRIVATE
    d3d11
    dxgi
    User32
)

target_compile_definitions(${HOOK_TARGET_NAME} PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)

if(BRAPS_BUILDING_32BIT_HELPERS)
    # BrapsInjector32.exe: tiny x86 helper the main (x64) Braps.exe spawns
    # to perform CreateRemoteThread-based injection into 32-bit targets,
    # since that technique requires matching bitness with the target.
    # Reuses Injector.cpp's InjectDll() unchanged, just compiled for x86.
    add_executable(BrapsInjector32
        hook/src/InjectorMain32.cpp
        hook/src/Injector.cpp
    )
    target_include_directories(BrapsInjector32 PRIVATE include hook/include)
    target_compile_definitions(BrapsInjector32 PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
else()
    # So Braps.exe can find BrapsHook.dll next to itself at runtime without
    # the user needing to know or pass its path manually.
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
endif()
