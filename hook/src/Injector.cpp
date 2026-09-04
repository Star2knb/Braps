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

namespace {

// Locates BrapsInjector32.exe next to the currently running executable.
// Returns an empty string if it's not present (e.g. the 32-bit helper
// build was never produced/copied alongside Braps.exe).
std::wstring FindInjector32Path() {
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::wstring path(exePath);
    size_t lastSlash = path.find_last_of(L"\\/");
    std::wstring candidate = path.substr(0, lastSlash + 1) + L"BrapsInjector32.exe";
    return (GetFileAttributesW(candidate.c_str()) != INVALID_FILE_ATTRIBUTES) ? candidate : L"";
}

// Cross-bitness injection: CreateRemoteThread + LoadLibraryW only works
// when the calling process matches the target's pointer size, so a
// 64-bit Braps.exe spawns this same-bitness helper and lets IT perform
// the actual injection, rather than attempting it directly.
bool InjectViaHelper32(DWORD targetPid, const std::wstring& dllPath, std::wstring& outError) {
    std::wstring helperPath = FindInjector32Path();
    if (helperPath.empty()) {
        outError = L"Target is a 32-bit process, but BrapsInjector32.exe was not found next to "
                   L"Braps.exe — build the 32-bit helper (see README.md) to support 32-bit games.";
        return false;
    }

    std::wstring cmdLine = L"\"" + helperPath + L"\" " + std::to_wstring(targetPid) + L" \"" + dllPath + L"\"";

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};

    std::wstring mutableCmd = cmdLine;
    if (!CreateProcessW(nullptr, mutableCmd.data(), nullptr, nullptr, FALSE,
                         CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        outError = L"Failed to launch BrapsInjector32.exe (GetLastError=" + std::to_wstring(GetLastError()) + L").";
        return false;
    }

    WaitForSingleObject(pi.hProcess, 10000);
    DWORD exitCode = 1;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    if (exitCode != 0) {
        outError = L"BrapsInjector32.exe reported a failure (exit code " + std::to_wstring(exitCode) + L").";
        return false;
    }
    return true;
}

} // namespace

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
    // bitness boundaries). When we're 64-bit and the target is 32-bit,
    // delegate to the same-bitness BrapsInjector32.exe helper instead of
    // failing outright.
    BOOL targetIsWow64 = FALSE, selfIsWow64 = FALSE;
    IsWow64Process(process, &targetIsWow64);
    IsWow64Process(GetCurrentProcess(), &selfIsWow64);
    if (targetIsWow64 != selfIsWow64) {
        CloseHandle(process);
        if (!selfIsWow64 && targetIsWow64) {
            // We're 64-bit (assuming a 64-bit OS build of Braps.exe), target is 32-bit.
            return InjectViaHelper32(targetPid, dllPath, outError);
        }
        outError = L"Bitness mismatch between Braps.exe and the target process — "
                   L"64-bit targets need the 64-bit Braps.exe build.";
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

bool Is32BitProcess(DWORD pid) {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return false;

    BOOL isWow64 = FALSE;
    IsWow64Process(process, &isWow64);
    CloseHandle(process);

    // IsWow64Process reports TRUE only for a 32-bit process running under
    // WOW64 on a 64-bit OS. On a native 32-bit OS every process would
    // report FALSE here despite being 32-bit — not a concern for us since
    // Braps.exe itself is 64-bit-only, so this function only ever runs on
    // a 64-bit OS where WOW64 is exactly how 32-bit processes show up.
    return isWow64 == TRUE;
}

} // namespace braps
