#pragma once

// Installs a vtable hook on IDXGISwapChain::Present inside the current
// (target game) process, capturing each presented backbuffer and pushing
// it to the shared frame channel for Braps.exe to consume. This is the
// Fraps-style approach: grab the frame directly from the game's own D3D11
// pipeline before Windows ever composites it, avoiding the desktop
// round-trip (and its GPU readback cost) that DXGI Desktop Duplication
// pays on every frame.
//
// Must be called from DllMain (DLL_PROCESS_ATTACH) after injection.
namespace braps {

bool InstallD3D11Hook();
void RemoveD3D11Hook();

} // namespace braps
