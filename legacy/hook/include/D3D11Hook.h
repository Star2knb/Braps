#pragma once

// Installs a vtable hook on IDXGISwapChain::Present inside the current
// (target game) process, capturing each presented backbuffer and pushing
// it to the shared frame channel for Braps.exe to consume. This is the
// Fraps-style approach: grab the frame directly from the game's own D3D
// pipeline before Windows ever composites it, avoiding the desktop
// round-trip (and its GPU readback cost) that DXGI Desktop Duplication
// pays on every frame.
//
// Despite the name (kept for header-file continuity), this hook covers
// both DXGI SwapChain-based APIs that share the exact same
// IDXGISwapChain::Present vtable slot: Direct3D 11 and Direct3D 10. Which
// one a given game actually uses is detected per-swapchain at runtime
// inside the Present hook itself (see PresentHook.cpp) — the vtable patch
// point is identical either way, only the backbuffer readback path
// differs (ID3D11Device+DeviceContext vs. ID3D10Device, which has no
// separate immediate context object).
//
// Must be called from DllMain (DLL_PROCESS_ATTACH) after injection.
namespace braps {

bool InstallD3D11Hook();
void RemoveD3D11Hook();

} // namespace braps
