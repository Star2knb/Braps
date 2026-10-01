// rec_inject32.exe: injects rec_hook32.dll into 32-bit games on behalf of the x64 host
// (recorder plan X2). Skeleton: the x86 build works; injection arrives in recorder milestone M7.
#include <cstdio>
#include <cstring>

#include "rec/events.h"

static_assert(sizeof(void*) == 4, "rec_inject32 is the 32-bit helper; build it with an x86-* preset");

int main(int argc, char** argv) {
    if (argc > 1 && std::strcmp(argv[1], "--version") == 0) {
        std::printf("rec_inject32 0.1.0 (x86, %zu event codes)\n", rec::kEventCount);
        return 0;
    }
    std::fprintf(stderr, "rec_inject32: injection into 32-bit games arrives in recorder milestone M7.\n");
    return 2;
}
