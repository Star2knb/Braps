#include "Injector.h"

#include <tlhelp32.h>
#include <psapi.h>

namespace braps {

namespace {

struct EnumContext {
    DWORD pid;
    bool hasVisibleWindow;
};

BOOL CALLBACK EnumWindowsProc(HWND hwnd, LPARAM lParam) {
    auto* ctx = reinterpret_cast<EnumContext*>(lParam);
    DWORD windowPid = 0;
    GetWindowThreadProcessId(hwnd, &windowPid);
    if (windowPid == ctx->pid && IsWindowVisible(hwnd) && GetWindowTextLengthW(hwnd) > 0) {
        ctx->hasVisibleWindow = true;
        return FALSE; // stop enumeration, found one
    }
    return TRUE;
}

bool HasVisibleWindow(DWORD pid) {
    EnumContext ctx{pid, false};
    EnumWindows(EnumWindowsProc, reinterpret_cast<LPARAM>(&ctx));
    return ctx.hasVisibleWindow;
}

}

std::vector<ProcessInfo> ListCandidateProcesses() {
    std::vector<ProcessInfo> result;

    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return result;

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);

    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (entry.th32ProcessID == 0) continue;
            if (HasVisibleWindow(entry.th32ProcessID)) {
                result.push_back({entry.th32ProcessID, entry.szExeFile});
            }
        } while (Process32NextW(snapshot, &entry));
    }

    CloseHandle(snapshot);
    return result;
}

bool InjectDll(DWORD targetPid, const std::wstring& dllPath, std::wstring& outError) {
    HANDLE process = OpenProcess(
        PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION |
        PROCESS_VM_WRITE | PROCESS_VM_READ,
        FALSE, targetPid);
    if (!process) {
        outError = L"OpenProcess failed (insufficient privileges, or process is protected — "
                   L"anti-cheat-protected games will refuse this).";
        return false;
    }

    // Bitness check: a 64-bit Braps.exe cannot inject into a 32-bit game
    // process or vice versa — CreateRemoteThread with LoadLibraryW's
    // address only works when both processes share the same pointer size
    // and the same LoadLibraryW address (true for same-bitness processes
    // since kernel32 loads at the same address system-wide, but not across
    // bitness boundaries).
    BOOL targetIsWow64 = FALSE, selfIsWow64 = FALSE;
    IsWow64Process(process, &targetIsWow64);
    IsWow64Process(GetCurrentProcess(), &selfIsWow64);
    if (targetIsWow64 != selfIsWow64) {
        outError = L"Bitness mismatch between Braps.exe and the target process — "
                   L"build/run the matching 32-bit or 64-bit variant.";
        CloseHandle(process);
        return false;
    }

    size_t pathBytes = (dllPath.size() + 1) * sizeof(wchar_t);
    LPVOID remotePath = VirtualAllocEx(process, nullptr, pathBytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remotePath) {
        outError = L"VirtualAllocEx failed in target process.";
        CloseHandle(process);
        return false;
    }

    if (!WriteProcessMemory(process, remotePath, dllPath.c_str(), pathBytes, nullptr)) {
        outError = L"WriteProcessMemory failed.";
        VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
        CloseHandle(process);
        return false;
    }

    HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    auto loadLibraryAddr = reinterpret_cast<LPTHREAD_START_ROUTINE>(
        GetProcAddress(kernel32, "LoadLibraryW"));

    HANDLE remoteThread = CreateRemoteThread(
        process, nullptr, 0, loadLibraryAddr, remotePath, 0, nullptr);
    if (!remoteThread) {
        outError = L"CreateRemoteThread failed.";
        VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
        CloseHandle(process);
        return false;
    }

    WaitForSingleObject(remoteThread, 5000);

    DWORD exitCode = 0;
    GetExitCodeThread(remoteThread, &exitCode); // HMODULE of loaded DLL, or 0 on failure

    CloseHandle(remoteThread);
    VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
    CloseHandle(process);

    if (exitCode == 0) {
        outError = L"LoadLibraryW returned NULL in target process — DLL failed to load "
                   L"(missing dependency, or blocked by anti-cheat/anti-malware).";
        return false;
    }
    return true;
}

} // namespace braps
