#include "rec/inject.h"

#include <psapi.h>

#include "rec/paths.h"

namespace rec {
namespace {

bool is_elevated(HANDLE process) {
    HANDLE token = nullptr;
    if (!OpenProcessToken(process, TOKEN_QUERY, &token)) return false;
    TOKEN_ELEVATION elevation{};
    DWORD size = 0;
    const bool elevated = GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size) && elevation.TokenIsElevated;
    CloseHandle(token);
    return elevated;
}

bool self_elevated() { return is_elevated(GetCurrentProcess()); }

// Quotes one command-line argument the way CommandLineToArgvW expects.
std::wstring quote_argument(const std::wstring& arg) {
    if (!arg.empty() && arg.find_first_of(L" \t\"") == std::wstring::npos) return arg;
    std::wstring r = L"\"";
    size_t backslashes = 0;
    for (wchar_t c : arg) {
        if (c == L'\\') {
            ++backslashes;
        } else if (c == L'"') {
            r.append(backslashes * 2 + 1, L'\\');
            r += L'"';
            backslashes = 0;
        } else {
            r.append(backslashes, L'\\');
            r += c;
            backslashes = 0;
        }
    }
    r.append(backslashes * 2, L'\\');
    r += L'"';
    return r;
}

bool query_identity(HANDLE process, bool* is64, std::string* name, std::filesystem::path* path) {
    wchar_t buf[MAX_PATH * 4];
    DWORD size = DWORD(std::size(buf));
    if (QueryFullProcessImageNameW(process, 0, buf, &size)) {
        *path = buf;
        *name = to_utf8(path->filename().wstring());
    }
    USHORT process_machine = 0, native_machine = 0;
    *is64 = !IsWow64Process2(process, &process_machine, &native_machine) || process_machine == IMAGE_FILE_MACHINE_UNKNOWN;
    return true;
}

}  // namespace

Target& Target::operator=(Target&& o) noexcept {
    if (this != &o) {
        if (process_) CloseHandle(process_);
        pid_ = o.pid_;
        process_ = o.process_;
        is_64bit_ = o.is_64bit_;
        name_ = std::move(o.name_);
        exe_path_ = std::move(o.exe_path_);
        o.process_ = nullptr;
        o.pid_ = 0;
    }
    return *this;
}

Target::~Target() {
    if (process_) CloseHandle(process_);
}

bool Target::open(uint32_t pid, Target* out, std::string* error, bool* elevated_target) {
    if (elevated_target) *elevated_target = false;
    constexpr DWORD kRights = PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION | PROCESS_VM_WRITE |
                              PROCESS_VM_READ | SYNCHRONIZE;
    HANDLE h = OpenProcess(kRights, FALSE, pid);
    if (!h) {
        const DWORD err = GetLastError();
        if (err == ERROR_ACCESS_DENIED) {
            // Elevated target and non-elevated rec is the common reason (E1003); a protected process the other.
            HANDLE probe = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
            const bool elevated = probe && is_elevated(probe);
            if (probe) CloseHandle(probe);
            if (elevated && !self_elevated()) {
                if (elevated_target) *elevated_target = true;
                *error = "E1003: process " + std::to_string(pid) + " runs as administrator and rec does not; run rec as administrator";
                return false;
            }
            *error = "process " + std::to_string(pid) + " can't be opened (access denied; it may be a protected process)";
            return false;
        }
        *error = "process " + std::to_string(pid) + " can't be opened: " + win32_error_text(err);
        return false;
    }
    Target t;
    t.pid_ = pid;
    t.process_ = h;
    query_identity(h, &t.is_64bit_, &t.name_, &t.exe_path_);
    *out = std::move(t);
    return true;
}

Target Target::adopt(HANDLE process, uint32_t pid) {
    Target t;
    t.pid_ = pid;
    t.process_ = process;
    query_identity(process, &t.is_64bit_, &t.name_, &t.exe_path_);
    return t;
}

bool Target::exited() const { return wait_exit(0); }

bool Target::wait_exit(uint32_t ms) const { return process_ && WaitForSingleObject(process_, ms) == WAIT_OBJECT_0; }

bool inject_dll(const Target& target, const std::filesystem::path& dll, std::string* error, uint32_t timeout_ms) {
    const std::wstring path = dll.wstring();
    const SIZE_T bytes = (path.size() + 1) * sizeof(wchar_t);
    HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    // kernel32 is mapped at the same address in every process of a boot session (same bitness).
    auto load_library = kernel32 ? reinterpret_cast<LPTHREAD_START_ROUTINE>(GetProcAddress(kernel32, "LoadLibraryW")) : nullptr;
    if (!load_library) {
        *error = "LoadLibraryW not found";
        return false;
    }
    void* remote = VirtualAllocEx(target.handle(), nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote) {
        *error = "VirtualAllocEx in the target failed: " + win32_error_text(GetLastError());
        return false;
    }
    bool ok = false;
    if (!WriteProcessMemory(target.handle(), remote, path.c_str(), bytes, nullptr)) {
        *error = "WriteProcessMemory failed: " + win32_error_text(GetLastError());
    } else if (HANDLE thread = CreateRemoteThread(target.handle(), nullptr, 0, load_library, remote, 0, nullptr)) {
        const DWORD wait = WaitForSingleObject(thread, timeout_ms);
        DWORD module_low = 0;
        if (wait != WAIT_OBJECT_0) {
            *error = "the target did not finish loading the hook DLL in time";
            // The thread may still complete later; leave the path buffer allocated.
            CloseHandle(thread);
            return false;
        }
        GetExitCodeThread(thread, &module_low);
        CloseHandle(thread);
        if (module_low == 0)
            *error = "LoadLibrary failed inside the target (blocked by its code-integrity policy, or a dependency is missing)";
        else
            ok = true;
    } else {
        *error = "CreateRemoteThread failed: " + win32_error_text(GetLastError());
    }
    VirtualFreeEx(target.handle(), remote, 0, MEM_RELEASE);
    return ok;
}

std::filesystem::path hook_dll_path(bool is_64bit, std::string* error) {
    if (!is_64bit) {
        *error = "32-bit games are supported from recorder milestone M7";
        return {};
    }
    wchar_t buf[MAX_PATH * 4];
    const DWORD n = GetModuleFileNameW(nullptr, buf, DWORD(std::size(buf)));
    std::filesystem::path dll = std::filesystem::path(std::wstring(buf, n)).parent_path() / L"rec_hook64.dll";
    std::error_code ec;
    if (!std::filesystem::exists(dll, ec)) {
        *error = "rec_hook64.dll not found next to rec.exe (" + to_utf8(dll.wstring()) + ")";
        return {};
    }
    return dll;
}

LaunchedProcess::~LaunchedProcess() {
    if (main_thread) CloseHandle(main_thread);
}

bool LaunchedProcess::resume() { return main_thread && ResumeThread(main_thread) != DWORD(-1); }

void LaunchedProcess::terminate() {
    if (target.handle()) TerminateProcess(target.handle(), 1);
}

bool launch_suspended(const std::filesystem::path& exe, const std::vector<std::string>& args, LaunchedProcess* out,
                      std::string* error, bool* elevated_required) {
    if (elevated_required) *elevated_required = false;
    std::error_code ec;
    const std::filesystem::path full = std::filesystem::absolute(exe, ec);
    if (!std::filesystem::exists(full, ec)) {
        *error = "can't find " + to_utf8(full.wstring());
        return false;
    }
    std::wstring command_line = quote_argument(full.wstring());
    for (const std::string& a : args) command_line += L" " + quote_argument(from_utf8(a));

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    const std::wstring cwd = full.parent_path().wstring();
    if (!CreateProcessW(full.c_str(), command_line.data(), nullptr, nullptr, FALSE, CREATE_SUSPENDED, nullptr, cwd.c_str(), &si, &pi)) {
        const DWORD err = GetLastError();
        if (err == ERROR_ELEVATION_REQUIRED) {
            if (elevated_required) *elevated_required = true;
            *error = "E1003: " + to_utf8(full.filename().wstring()) + " needs administrator rights; run rec as administrator";
        } else {
            *error = "can't start " + to_utf8(full.wstring()) + ": " + win32_error_text(err);
        }
        return false;
    }
    out->target = Target::adopt(pi.hProcess, pi.dwProcessId);
    out->main_thread = pi.hThread;
    return true;
}

}  // namespace rec
