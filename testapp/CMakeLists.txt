# rec_testapp: the synthetic game used to test the recorder (plan §14.1, X10). D3D11 or OpenGL (--gl).
# d3d11.dll, dxgi.dll and opengl32.dll are delay-loaded, so they are not in the process when it starts: that is
# what `rec launch` has to cope with in a real game that loads its graphics API late (plan §4.1, I1103), and it
# keeps a Direct3D run free of OpenGL and an OpenGL run free of Direct3D.
add_executable(rec_testapp main.cpp)
target_link_libraries(rec_testapp PRIVATE rec_common_headers d3d11 dxgi opengl32 gdi32 user32 delayimp)
target_link_options(rec_testapp PRIVATE /DELAYLOAD:d3d11.dll /DELAYLOAD:dxgi.dll /DELAYLOAD:opengl32.dll)
set_target_properties(rec_testapp PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
