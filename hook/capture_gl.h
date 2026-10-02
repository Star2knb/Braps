// OpenGL frame capture (recorder plan §5.2), run from the SwapBuffers detour on the game's render thread.
//
// Per captured frame: blit the default framebuffer's back buffer into our own framebuffer at the output
// size (the blit scales, letterboxes and flips the rows), read it into a pixel-pack buffer, and fence it.
// The oldest finished buffer is mapped and copied into the shared frame ring as BGRA; the host converts it to
// NV12. The game's GL state is saved and restored around our calls; nothing here waits for the GPU.
#pragma once

#include <windows.h>

#include <cstdint>

namespace rec::hook {

// Called for the measured window on every swap, inside the SEH guard of gl_hook.cpp. Does nothing (one
// atomic load) unless the host asks for a recording. Returns how long (QPC ticks) it deliberately held
// the game to the tick grid (lock mode).
uint64_t capture_gl_on_present(HDC hdc, uint64_t present_qpc, uint32_t frame_time_us);

// Detach: closes the frame ring. No thread is inside a detour any more, and the game's context is not
// current on the calling thread, so the GL objects are abandoned, not deleted.
void capture_gl_shutdown();

}  // namespace rec::hook
