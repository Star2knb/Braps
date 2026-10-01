# rec_core: the recorder's logic as a library (recorder plan X12); rec.exe is a thin front-end.
add_library(rec_core STATIC
    src/paths.cpp
    src/options.cpp
    src/config.cpp
    src/logging.cpp
    src/doctor.cpp
    src/anticheat.cpp
    src/procscan.cpp
    src/inject.cpp
    src/hooklink.cpp
    src/watch.cpp
)
target_include_directories(rec_core PUBLIC include)
target_link_libraries(rec_core
    PUBLIC rec_common spdlog
    PRIVATE tomlplusplus rcv dxgi mfplat mfuuid ole32 shell32 psapi)
