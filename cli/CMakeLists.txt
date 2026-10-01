# rec.exe (recorder plan §12). Lands in <build>/bin next to rec_hook64.dll, which it injects.
add_executable(rec main.cpp hook_commands.cpp)
target_link_libraries(rec PRIVATE rec_core cli11)
set_target_properties(rec PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
add_dependencies(rec rec_hook64)
