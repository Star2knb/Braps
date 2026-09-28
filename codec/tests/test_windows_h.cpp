// The capture DLL and host include <windows.h> before rcv.h. This must keep compiling:
// windows.h defines macros such as near, far, min and max that can break a public header.
#include <windows.h>

#include "rcv/rcv.h"
#include "testfw.h"

TEST_CASE("api: rcv.h compiles after <windows.h>") {
    rcv_encode_params p{};
    p.near_level = 1;
    p.force_keyframe = 0;
    CHECK(p.near_level == 1);
}
