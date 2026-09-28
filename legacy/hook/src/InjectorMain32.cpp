#include "Injector.h"

#include <iostream>
#include <cstdlib>

// Standalone 32-bit helper: CreateRemoteThread + LoadLibraryW only works
// when the calling process matches the target's bitness (both must read/
// write the target's address space and resolve LoadLibraryW's address
// consistently) — a 64-bit Braps.exe cannot inject into a 32-bit game
// directly. This tiny x86 executable exists solely so Braps.exe can spawn
// a same-bitness injector when the target turns out to be 32-bit,
// reusing the exact same Injector.cpp logic (just compiled for x86 here
// instead of x64).
//
// Usage: BrapsInjector32.exe <pid> <dllPath>
// Exit code 0 on success; prints the error and returns 1 on failure.
int wmain(int argc, wchar_t** argv) {
    if (argc != 3) {
        std::wcerr << L"Usage: BrapsInjector32.exe <pid> <dllPath>\n";
        return 1;
    }

    DWORD pid = static_cast<DWORD>(_wtoi(argv[1]));
    std::wstring dllPath = argv[2];

    std::wstring error;
    if (!braps::InjectDll(pid, dllPath, error)) {
        std::wcerr << L"Injection failed: " << error << L"\n";
        return 1;
    }

    std::wcout << L"Injection succeeded.\n";
    return 0;
}
