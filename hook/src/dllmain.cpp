#include "D3D11Hook.h"

#include <windows.h>

// DllMain runs on injection (DLL_PROCESS_ATTACH) inside the target game's
// process. Per Win32 rules, DllMain must do minimal, loader-lock-safe work
// — so the actual hook installation (which creates a D3D11 device and a
// dummy window) is deferred to a separate thread rather than run inline
// here, since both of those can deadlock if done directly inside DllMain.
namespace {

DWORD WINAPI InstallHookThread(LPVOID) {
    braps::InstallD3D11Hook();
    return 0;
}

}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    switch (reason) {
        case DLL_PROCESS_ATTACH:
            DisableThreadLibraryCalls(hModule);
            CreateThread(nullptr, 0, InstallHookThread, nullptr, 0, nullptr);
            break;
        case DLL_PROCESS_DETACH:
            braps::RemoveD3D11Hook();
            break;
    }
    return TRUE;
}
