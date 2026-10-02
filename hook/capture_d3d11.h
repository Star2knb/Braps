// D3D11 frame capture (recorder plan §5.1), run from the Present detour on the game's render thread.
//
// Per captured frame: copy the back buffer, scale + letterbox + convert it to NV12 planes with two
// small draw calls (hook/shaders/capture.hlsl) inside our own device-context state, copy the planes
// into a ring of staging textures, and read the oldest finished one back into the shared frame ring
// (never waiting for the GPU). One frame per tick of the recording clock.
#pragma once

#include <dxgi.h>

#include <cstdint>

namespace rec::hook {

// Called for the measured swap chain on every Present, inside the SEH guard of dxgi_hook.cpp. Does
// nothing (one atomic load) unless the host asks for a recording. `frame_time_us` is the interval
// since the previous Present. Returns how long (QPC ticks) it deliberately held the game to the tick
// grid (lock mode), which is not counted as the hook's cost.
uint64_t capture_on_present(IDXGISwapChain* swap_chain, uint64_t present_qpc, uint32_t frame_time_us);

// Called when an exception was caught around capture_on_present: lets go of anything it held.
void capture_on_exception();

// Detach: closes the frame ring and drops the GPU objects. No thread is inside a detour any more.
void capture_shutdown();

}  // namespace rec::hook
