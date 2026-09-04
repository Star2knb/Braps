#pragma once

#include <windows.h>
#include <string>
#include <vector>

// Injects BrapsHook.dll into an already-running target process via the
// standard CreateRemoteThread + LoadLibraryW technique: allocate the DLL
// path string in the target process's memory, then start a remote thread
// whose entry point is LoadLibraryW itself, passing that allocated string
// as its argument. Windows loads the DLL as a side effect of running that
// thread, which triggers our DllMain(DLL_PROCESS_ATTACH) inside the target.
namespace braps {

struct ProcessInfo {
    DWORD pid;
    std::wstring name;
};

// Enumerates running processes with a visible top-level window, since a
// game worth recording always has one — filters out the huge number of
// background/system processes that could never be a capture target.
std::vector<ProcessInfo> ListCandidateProcesses();

// Returns true on successful injection (the DLL's DllMain ran and
// returned). This does NOT guarantee the D3D11 hook itself installed
// successfully — check SharedHeader::hookAttached via the shared channel
// for that, since hook installation happens asynchronously on a thread
// inside the target process after DllMain returns.
//
// When called from a 64-bit Braps.exe against a 32-bit target, this
// transparently delegates to BrapsInjector32.exe (a same-bitness helper,
// since CreateRemoteThread + LoadLibraryW requires matching pointer
// sizes) — the caller doesn't need to handle that case specially, but see
// Is32BitProcess() below if the caller needs to pick which hook DLL
// (BrapsHook.dll vs BrapsHook32.dll) to pass in dllPath.
bool InjectDll(DWORD targetPid, const std::wstring& dllPath, std::wstring& outError);

// True if the given process is 32-bit (running under WOW64 on a 64-bit
// OS, or natively 32-bit). Callers building a 64-bit Braps.exe use this
// to decide whether to pass BrapsHook.dll or BrapsHook32.dll to
// InjectDll(), since the DLL's own bitness must match the target's.
bool Is32BitProcess(DWORD pid);

} // namespace braps
