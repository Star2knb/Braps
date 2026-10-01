#include "rec/doctor.h"

#include <windows.h>
#include <dxgi.h>
#include <intrin.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <filesystem>

#include "rcv/rcv.h"
#include "rec/options.h"
#include "rec/paths.h"

namespace rec {
namespace {

std::string fmt(const char* f, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, f);
    std::vsnprintf(buf, sizeof(buf), f, ap);
    va_end(ap);
    return buf;
}

CheckResult pass(const char* name, std::string detail) { return {name, CheckStatus::Pass, std::move(detail), {}}; }
CheckResult warn(const char* name, std::string detail, std::string fix) {
    return {name, CheckStatus::Warn, std::move(detail), std::move(fix)};
}
CheckResult fail(const char* name, std::string detail, std::string fix) {
    return {name, CheckStatus::Fail, std::move(detail), std::move(fix)};
}

DWORD windows_build() {
    using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
    OSVERSIONINFOW v{};
    v.dwOSVersionInfoSize = sizeof(v);
    if (HMODULE ntdll = GetModuleHandleW(L"ntdll.dll"))
        if (auto fn = reinterpret_cast<RtlGetVersionFn>(GetProcAddress(ntdll, "RtlGetVersion"))) fn(&v);
    return v.dwBuildNumber;
}

CheckResult check_windows() {
    const DWORD build = windows_build();
    const char* name = build >= 22000 ? "Windows 11" : "Windows 10";
    if (build < 17763)
        return fail("Windows", fmt("build %lu", build), "rec needs Windows 10 version 1809 (build 17763) or later");
    return pass("Windows", fmt("%s, build %lu", name, build));
}

CheckResult check_game_audio() {
    const DWORD build = windows_build();
    if (build < 20348)
        return warn("Game audio", fmt("build %lu has no per-process audio capture", build),
                    "game audio falls back to all system sounds; per-game audio needs Windows 11");
    return pass("Game audio", "per-process audio capture available");
}

std::string cpu_brand() {
    int r[4];
    __cpuid(r, int(0x80000000));
    if (unsigned(r[0]) < 0x80000004u) return "unknown CPU";
    char brand[49] = {};
    for (int i = 0; i < 3; ++i) {
        __cpuid(r, int(0x80000002 + i));
        std::memcpy(brand + 16 * i, r, 16);
    }
    std::string s(brand);
    const size_t a = s.find_first_not_of(' ');
    return a == std::string::npos ? s : s.substr(a);
}

CheckResult check_cpu() {
    const rcv_isa isa = rcv_cpu_isa();
    const std::string brand = cpu_brand();
    if (isa == RCV_ISA_AVX2) return pass("CPU", brand + ", AVX2");
    if (isa == RCV_ISA_SSE41)
        return warn("CPU", brand + ", SSE4.1 (no AVX2)", "encoding works but is slower; prefer 720p or lower");
    return warn("CPU", brand + ", no SSE4.1", "encoding works but is much slower; prefer low resolutions");
}

CheckResult check_cores() {
    const DWORD n = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    if (n >= 4) return pass("CPU threads", fmt("%lu logical CPUs (encoder uses 2)", n));
    return warn("CPU threads", fmt("%lu logical CPUs", n), "the encoder will use 1 thread; record at 720p or lower");
}

CheckResult check_ram() {
    MEMORYSTATUSEX m{};
    m.dwLength = sizeof(m);
    GlobalMemoryStatusEx(&m);
    const double gb = double(m.ullTotalPhys) / (1024.0 * 1024 * 1024);
    if (gb >= 4) return pass("Memory", fmt("%.1f GB", gb));
    return warn("Memory", fmt("%.1f GB", gb), "close other programs; use a smaller record.queue_mb");
}

std::vector<CheckResult> check_gpus() {
    std::vector<CheckResult> out;
    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory)))) {
        out.push_back(fail("GPU", "DXGI unavailable", "update the graphics driver"));
        return out;
    }
    IDXGIAdapter1* a = nullptr;
    for (UINT i = 0; factory->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 d{};
        a->GetDesc1(&d);
        if (!(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
            LARGE_INTEGER v{};
            std::string driver = "driver version unknown";
            if (SUCCEEDED(a->CheckInterfaceSupport(__uuidof(IDXGIDevice), &v)))
                driver = fmt("driver %u.%u.%u.%u", HIWORD(v.HighPart), LOWORD(v.HighPart), HIWORD(v.LowPart),
                             LOWORD(v.LowPart));
            // The same adapter can be listed more than once (e.g. with a virtual display driver).
            const std::string line = to_utf8(d.Description) + ", " + driver;
            bool seen = false;
            for (const CheckResult& r : out) seen = seen || r.detail == line;
            if (!seen) out.push_back(pass("GPU", line));
        }
        a->Release();
    }
    factory->Release();
    if (out.empty()) out.push_back(fail("GPU", "no hardware adapter", "install or update the graphics driver"));
    return out;
}

CheckResult check_hotkey(const Config& cfg) {
    Hotkey k;
    if (!parse_hotkey(cfg.hotkeys.toggle, &k))
        return fail("Hotkey", "hotkeys.toggle = \"" + cfg.hotkeys.toggle + "\" is not a key",
                    "rec config set hotkeys.toggle F9");
    const std::string name = hotkey_name(k);
    constexpr int kId = 0xBF00;
    if (RegisterHotKey(nullptr, kId, k.modifiers | MOD_NOREPEAT, k.vk)) {
        UnregisterHotKey(nullptr, kId);
        return pass("Hotkey", name + " is free");
    }
    const DWORD err = GetLastError();
    return warn("Hotkey", name + " is taken by another program (" + win32_error_text(err) + ")",
                "rec will use a keyboard hook instead, or pick another key: rec config set hotkeys.toggle F10");
}

// The output folder, or the nearest folder above it that exists.
std::filesystem::path existing_ancestor(std::filesystem::path p) {
    std::error_code ec;
    while (!p.empty() && !std::filesystem::is_directory(p, ec)) {
        const std::filesystem::path parent = p.parent_path();
        if (parent == p) return {};
        p = parent;
    }
    return p;
}

CheckResult check_output_dir(const std::filesystem::path& dir) {
    const std::filesystem::path base = existing_ancestor(dir);
    const std::string shown = to_utf8(dir.wstring());
    if (base.empty()) return fail("Output folder", shown + ": drive not found", "rec config set record.out_dir <folder>");
    const std::wstring probe = (base / (L".rec_doctor_" + std::to_wstring(GetCurrentProcessId()) + L".tmp")).wstring();
    HANDLE h = CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                           FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return fail("Output folder", shown + ": not writable (" + win32_error_text(GetLastError()) + ")",
                    "choose another folder: rec config set record.out_dir <folder>");
    DWORD written = 0;
    const char byte = 0;
    const BOOL ok = WriteFile(h, &byte, 1, &written, nullptr);
    CloseHandle(h);  // deletes the probe
    if (!ok) return fail("Output folder", shown + ": write failed", "rec config set record.out_dir <folder>");
    std::error_code ec;
    return pass("Output folder", shown + (std::filesystem::is_directory(dir, ec) ? " (writable)" : " (will be created)"));
}

std::vector<CheckResult> check_volume(const Config& cfg, const std::filesystem::path& dir) {
    std::vector<CheckResult> out;
    const std::filesystem::path base = existing_ancestor(dir);
    wchar_t root[MAX_PATH] = {};
    if (base.empty() || !GetVolumePathNameW(base.c_str(), root, MAX_PATH)) return out;

    wchar_t fs[MAX_PATH] = {};
    GetVolumeInformationW(root, nullptr, 0, nullptr, nullptr, nullptr, fs, MAX_PATH);
    const std::string fs_name = to_utf8(fs);
    const UINT type = GetDriveTypeW(root);
    const std::string where = to_utf8(root) + " " + fs_name;
    if (type == DRIVE_REMOTE)
        out.push_back(warn("Filesystem", where + " (network drive)", "record to a local drive; network writes stall"));
    else if (fs_name == "FAT32")
        out.push_back(warn("Filesystem", where, "files are split every 3.9 GB; NTFS or exFAT avoids this"));
    else
        out.push_back(pass("Filesystem", where));

    ULARGE_INTEGER free_bytes{};
    if (GetDiskFreeSpaceExW(root, &free_bytes, nullptr, nullptr)) {
        const double gb = double(free_bytes.QuadPart) / (1024.0 * 1024 * 1024);
        OutputSize size;
        parse_size(cfg.record.size, &size);
        if (size.native) {  // estimate with the desktop size
            DEVMODEW dm{};
            dm.dmSize = sizeof(dm);
            EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &dm);
            size.width = int(dm.dmPelsWidth);
            size.height = int(dm.dmPelsHeight);
        }
        const RateEstimate r = estimate_write_rate(size.width, size.height, cfg.record.fps, cfg.record.format == "rgb");
        const double minutes = r.typical_mbps > 0 ? gb * 1024 / r.typical_mbps / 60 : 0;
        const std::string d = fmt("%.0f GB free (about %.0f min at %dx%d@%d)", gb, minutes, size.width, size.height,
                                  cfg.record.fps);
        if (gb < cfg.log.critical_space_gb)
            out.push_back(fail("Free space", d, "free up space or choose another drive (record.out_dir)"));
        else if (gb < cfg.log.low_space_gb)
            out.push_back(warn("Free space", d, "free up space or choose another drive (record.out_dir)"));
        else
            out.push_back(pass("Free space", d));

        // No disk benchmark until `rec bench-disk` exists (recorder M5): state what the drive must sustain.
        out.push_back(warn("Disk speed",
                           fmt("not measured; %dx%d@%d lossless needs ~%.0f MB/s, up to ~%.0f MB/s in detailed scenes",
                               size.width, size.height, cfg.record.fps, r.typical_mbps, r.detailed_mbps),
                           "measure it with rec bench-disk (arrives in recorder milestone M5)"));
    }
    return out;
}

CheckResult check_power() {
    SYSTEM_POWER_STATUS p{};
    if (!GetSystemPowerStatus(&p)) return warn("Power", "unknown", "");
    const bool battery_saver = p.SystemStatusFlag == 1;
    if (p.ACLineStatus == 0)
        return warn("Power", fmt("on battery (%d%%)%s", int(p.BatteryLifePercent), battery_saver ? ", battery saver on" : ""),
                    "plug in: on battery the CPU runs slower and recordings may drop to near-lossless");
    if (battery_saver) return warn("Power", "AC, battery saver on", "turn off battery saver while recording");
    return pass("Power", "AC power");
}

CheckResult check_hw_encoder() {
    const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    std::string names;
    UINT32 count = 0;
    if (SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE))) {
        MFT_REGISTER_TYPE_INFO out_type{MFMediaType_Video, MFVideoFormat_H264};
        IMFActivate** acts = nullptr;
        if (SUCCEEDED(MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER, nullptr,
                                &out_type, &acts, &count))) {
            for (UINT32 i = 0; i < count; ++i) {
                WCHAR* name = nullptr;
                UINT32 len = 0;
                if (SUCCEEDED(acts[i]->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &name, &len))) {
                    const std::string n = to_utf8(name);
                    if (names.find(n) == std::string::npos) names += (names.empty() ? "" : ", ") + n;  // one per GPU
                    CoTaskMemFree(name);
                }
                acts[i]->Release();
            }
            CoTaskMemFree(acts);
        }
        MFShutdown();
    }
    if (SUCCEEDED(co)) CoUninitialize();
    if (count == 0)
        return warn("HW encoder", "no hardware H.264 encoder found",
                    "--encoder hw will be unavailable; update the graphics driver");
    return pass("HW encoder", names.empty() ? fmt("%u found", count) : names);
}

CheckResult check_ffmpeg() {
    wchar_t path[MAX_PATH] = {};
    if (SearchPathW(nullptr, L"ffmpeg.exe", nullptr, MAX_PATH, path, nullptr))
        return pass("FFmpeg", to_utf8(path));
    return warn("FFmpeg", "not on PATH", "install FFmpeg and add it to PATH to use rec convert");
}

}  // namespace

RateEstimate estimate_write_rate(int width, int height, int fps, bool rgb) {
    // Whole-file ratios measured by the codec at 60 fps: Minecraft 5.7 (YUV) / 7.4 (RGB) with skip and
    // DUPs; Warframe 1.7 real-frame (YUV). Detailed RGB is assumed no better than YUV.
    const double raw_mb = double(width) * height * (rgb ? 3.0 : 1.5) * fps / (1024.0 * 1024);
    return {raw_mb / (rgb ? 7.4 : 5.7), raw_mb / 1.7};
}

std::vector<CheckResult> run_doctor(const Config& cfg) {
    std::vector<CheckResult> r;
    r.push_back(check_windows());
    r.push_back(check_cpu());
    r.push_back(check_cores());
    r.push_back(check_ram());
    for (CheckResult& g : check_gpus()) r.push_back(std::move(g));
    r.push_back(check_hotkey(cfg));
    const std::filesystem::path dir = expand_env(cfg.record.out_dir);
    r.push_back(check_output_dir(dir));
    for (CheckResult& v : check_volume(cfg, dir)) r.push_back(std::move(v));
    r.push_back(check_power());
    r.push_back(check_game_audio());
    r.push_back(check_hw_encoder());
    r.push_back(check_ffmpeg());
    return r;
}

}  // namespace rec
