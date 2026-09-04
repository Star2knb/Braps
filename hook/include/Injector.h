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
bool InjectDll(DWORD targetPid, const std::wstring& dllPath, std::wstring& outError);

} // namespace braps
