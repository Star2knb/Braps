// MinHook detours on IDXGISwapChain::Present and Present1 (recorder plan §4.4). In milestone M1 they
// only measure: the frame interval and our own cost per call, written to the control block.
#pragma once

#include <cstdint>

namespace rec::hook {

// Finds the addresses (kiero2) and installs the hooks. Install thread only. False if nothing was hooked.
bool install_dxgi_hooks();

// Stops measuring and removes the hooks. The caller then waits for g.inflight to reach zero.
void remove_dxgi_hooks();

}  // namespace rec::hook
