# rec_common: definitions shared by the x64 host and the x86/x64 hook side (event codes, the
# shared-memory protocol). Built for both architectures. Also carries the Windows API settings for
# all recorder targets.
#
# rec_common_headers is the same include path and settings without any compiled code: the hook DLLs
# link the static CRT (they are loaded into other people's processes), so they can't link rec_common,
# which the host builds against the dynamic one.
add_library(rec_common_headers INTERFACE)
target_include_directories(rec_common_headers INTERFACE include)
# Windows 10 1809+ (recorder plan §2); wide-character Win32 API; no min/max macros.
target_compile_definitions(rec_common_headers INTERFACE
    _WIN32_WINNT=0x0A00 WINVER=0x0A00 NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)

add_library(rec_common STATIC src/events.cpp)
target_link_libraries(rec_common PUBLIC rec_common_headers)
