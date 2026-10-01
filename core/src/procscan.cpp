#include "rec/procscan.h"

#include <windows.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cctype>

#include "rec/paths.h"
#include "rec/protocol.h"

namespace rec {
namespace {

std::string lower(std::string_view s) {
    std::string r(s);
    for (char& c : r) c = char(std::tolower(static_cast<unsigned char>(c)));
    return r;
}

bool is_64bit_process(uint32_t pid) {
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return true;
    USHORT process_machine = 0, native_machine = 0;
    bool is64 = true;
    if (IsWow64Process2(h, &process_machine, &native_machine)) is64 = (process_machine == IMAGE_FILE_MACHINE_UNKNOWN);
    CloseHandle(h);
    return is64;
}

struct ModuleScan {
    uint32_t apis = 0;
    bool hook_loaded = false;
    bool ok = false;
};

ModuleScan scan_modules(uint32_t pid) {
    ModuleScan scan;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snap == INVALID_HANDLE_VALUE) return scan;
    scan.ok = true;
    MODULEENTRY32W m{};
    m.dwSize = sizeof(m);
    for (BOOL ok = Module32FirstW(snap, &m); ok; ok = Module32NextW(snap, &m)) {
        const std::string name = to_utf8(m.szModule);
        scan.apis |= classify_graphics_module(name);
        if (is_hook_module(name)) scan.hook_loaded = true;
    }
    CloseHandle(snap);
    return scan;
}

std::vector<PROCESSENTRY32W> all_processes() {
    std::vector<PROCESSENTRY32W> list;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return list;
    PROCESSENTRY32W p{};
    p.dwSize = sizeof(p);
    for (BOOL ok = Process32FirstW(snap, &p); ok; ok = Process32NextW(snap, &p)) list.push_back(p);
    CloseHandle(snap);
    return list;
}

constexpr uint32_t kRealApis = proto::kApiD3D9 | proto::kApiD3D10 | proto::kApiD3D11 | proto::kApiD3D12 |
                               proto::kApiOpenGL | proto::kApiVulkan;

}  // namespace

uint32_t classify_graphics_module(std::string_view base_name) {
    const std::string n = lower(base_name);
    if (n == "d3d9.dll") return proto::kApiD3D9;
    if (n == "d3d10.dll" || n == "d3d10_1.dll" || n == "d3d10core.dll") return proto::kApiD3D10;
    if (n == "d3d11.dll") return proto::kApiD3D11;
    if (n == "d3d12.dll") return proto::kApiD3D12;
    if (n == "opengl32.dll") return proto::kApiOpenGL;
    if (n == "vulkan-1.dll") return proto::kApiVulkan;
    if (n == "dxgi.dll") return proto::kApiDXGI;
    return 0;
}

std::string api_names(uint32_t apis) {
    static const struct {
        uint32_t bit;
        const char* name;
    } kNames[] = {{proto::kApiD3D9, "D3D9"},     {proto::kApiD3D10, "D3D10"},   {proto::kApiD3D11, "D3D11"},
                  {proto::kApiD3D12, "D3D12"},   {proto::kApiOpenGL, "OpenGL"}, {proto::kApiVulkan, "Vulkan"},
                  {proto::kApiDXGI, "DXGI"}};
    std::string r;
    for (const auto& n : kNames)
        if (apis & n.bit) r += (r.empty() ? "" : " ") + std::string(n.name);
    return r.empty() ? "-" : r;
}

bool is_hook_module(std::string_view base_name) {
    const std::string n = lower(base_name);
    return n == "rec_hook64.dll" || n == "rec_hook32.dll";
}

std::vector<ProcessInfo> list_graphics_processes() {
    std::vector<ProcessInfo> result;
    const uint32_t self = GetCurrentProcessId();
    for (const PROCESSENTRY32W& p : all_processes()) {
        if (p.th32ProcessID == 0 || p.th32ProcessID == 4 || p.th32ProcessID == self) continue;
        const ModuleScan scan = scan_modules(p.th32ProcessID);
        if (!scan.ok || !(scan.apis & kRealApis)) continue;
        ProcessInfo info;
        info.pid = p.th32ProcessID;
        info.name = to_utf8(p.szExeFile);
        info.is_64bit = is_64bit_process(info.pid);
        info.apis = scan.apis;
        info.hook_loaded = scan.hook_loaded;
        result.push_back(std::move(info));
    }
    std::sort(result.begin(), result.end(), [](const ProcessInfo& a, const ProcessInfo& b) {
        const std::string x = lower(a.name), y = lower(b.name);
        return x != y ? x < y : a.pid < b.pid;
    });
    return result;
}

bool process_info(uint32_t pid, ProcessInfo* out) {
    for (const PROCESSENTRY32W& p : all_processes()) {
        if (p.th32ProcessID != pid) continue;
        ProcessInfo info;
        info.pid = pid;
        info.name = to_utf8(p.szExeFile);
        info.is_64bit = is_64bit_process(pid);
        const ModuleScan scan = scan_modules(pid);
        info.apis = scan.apis;
        info.hook_loaded = scan.hook_loaded;
        *out = std::move(info);
        return true;
    }
    return false;
}

std::vector<uint32_t> find_processes_by_name(std::string_view name) {
    std::vector<uint32_t> pids;
    const std::string wanted = lower(name);
    for (const PROCESSENTRY32W& p : all_processes())
        if (lower(to_utf8(p.szExeFile)) == wanted) pids.push_back(p.th32ProcessID);
    return pids;
}

std::vector<uint32_t> find_hooked_processes() {
    std::vector<uint32_t> pids;
    const uint32_t self = GetCurrentProcessId();
    for (const PROCESSENTRY32W& p : all_processes()) {
        if (p.th32ProcessID == 0 || p.th32ProcessID == 4 || p.th32ProcessID == self) continue;
        if (scan_modules(p.th32ProcessID).hook_loaded) pids.push_back(p.th32ProcessID);
    }
    return pids;
}

}  // namespace rec
