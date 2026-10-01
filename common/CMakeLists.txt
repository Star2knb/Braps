# rec_common: definitions shared by the x64 host and the x86/x64 hook side (event codes; later the
# shared-memory protocol). Built for both architectures. Also carries the Windows API settings for
# all recorder targets.
add_library(rec_common STATIC src/events.cpp)
target_include_directories(rec_common PUBLIC include)
# Windows 10 1809+ (recorder plan §2); wide-character Win32 API; no min/max macros.
target_compile_definitions(rec_common PUBLIC
    _WIN32_WINNT=0x0A00 WINVER=0x0A00 NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
