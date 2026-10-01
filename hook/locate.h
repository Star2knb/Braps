// Finding the swap-chain method addresses (recorder plan §4.1, §4.6): kiero2 creates a throwaway
// device on a hidden window and returns its vtable; we read fixed indices and validate each pointer
// before it is hooked. Install thread only: it allocates, creates a window and a device.
#pragma once

#include <cstdint>

namespace rec::hook {

struct SwapChainAddrs {
    void* present = nullptr;   // IDXGISwapChain::Present
    void* present1 = nullptr;  // IDXGISwapChain1::Present1
};

// True if at least Present was found and validated. Logs E1105 (lookup failed) and W1106 (pointer
// outside dxgi.dll). *locate_us is the time the lookup took.
bool locate_dxgi(SwapChainAddrs* out, uint64_t* locate_us);

// True if p lies inside the image of the module with this name (loaded in this process).
bool pointer_in_module(const wchar_t* module_name, const void* p);

}  // namespace rec::hook
