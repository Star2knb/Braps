# rec.exe (recorder plan §12)
add_executable(rec main.cpp)
target_link_libraries(rec PRIVATE rec_core cli11)
