# rec_inject32.exe: 32-bit injection helper for 32-bit games (recorder plan X2). Built by the
# x86-* presets; a skeleton until recorder milestone M7.
add_executable(rec_inject32 main.cpp)
target_link_libraries(rec_inject32 PRIVATE rec_common)
