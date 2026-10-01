# rec_testapp: the synthetic game used to test the recorder (plan §14.1, X10). D3D11 for now.
# d3d11.dll and dxgi.dll are delay-loaded, so they are not in the process when it starts: that is what
# `rec launch` has to cope with in a real game that loads Direct3D late (plan §4.1, I1103).
add_executable(rec_testapp main.cpp)
target_link_libraries(rec_testapp PRIVATE rec_common_headers d3d11 dxgi user32 delayimp)
target_link_options(rec_testapp PRIVATE /DELAYLOAD:d3d11.dll /DELAYLOAD:dxgi.dll)
set_target_properties(rec_testapp PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
