// What every Present hook (DXGI, OpenGL) does the same way: the statistics in the control block, the cost
// accounting, the choice between APIs when a process presents through more than one, and what happens
// when our code throws.
#pragma once

#include <windows.h>

#include <cstdint>

namespace rec::hook {

// Counts a Present of the measured surface; returns the interval since the previous one in us (0 for the first).
uint32_t note_present(uint64_t now);

// Our own cost of this Present: from `t0` to now, less `held` (the deliberate wait of lock mode).
void note_cost(uint64_t t0, uint64_t held);

// Whether another graphics API than the caller's already owns the measuring: it is the backend and presented
// within the last second. `mine_is_gl`: the caller is the OpenGL hook (otherwise a DXGI one).
bool other_api_active(bool mine_is_gl, uint64_t now);

// An exception in our code disables measuring for good and the game carries on (§13.2).
void note_exception(DWORD code);

// Refresh rate of the monitor the window is on (current display mode); 0 if unknown.
uint32_t window_refresh_hz(HWND window);

}  // namespace rec::hook
