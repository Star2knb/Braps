#include "rec/anticheat.h"

#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cctype>

#include "rec/paths.h"

namespace rec {
namespace {

std::string lower_stem(std::string_view s) {
    std::string r;
    r.reserve(s.size());
    for (char c : s) r += char(std::tolower(static_cast<unsigned char>(c)));
    // Drop a short extension (.exe, .dll, .sys), but not a dot inside a longer name part.
    const size_t dot = r.rfind('.');
    if (dot != std::string::npos && r.size() - dot <= 5) r.resize(dot);
    return r;
}

// `entry` occurs in `name` at its start or right after a non-alphanumeric character. "BEService_x64"
// and "game_EasyAntiCheat" match; "WindscribeService" does not contain "BEService" as a word.
bool contains_word_start(const std::string& name, const std::string& entry) {
    for (size_t at = name.find(entry); at != std::string::npos; at = name.find(entry, at + 1))
        if (at == 0 || !std::isalnum(static_cast<unsigned char>(name[at - 1]))) return true;
    return false;
}

bool matches(std::string_view name, const std::vector<std::string>& list) {
    const std::string n = lower_stem(name);
    if (n.empty()) return false;
    for (const std::string& entry : list) {
        const std::string e = lower_stem(entry);
        if (e.empty()) continue;
        if (n == e) return true;
        if (e.size() >= 6 && contains_word_start(n, e)) return true;
    }
    return false;
}

std::vector<std::string> merged(const std::vector<std::string>& user) {
    std::vector<std::string> all = builtin_anticheat_names();
    all.insert(all.end(), user.begin(), user.end());
    return all;
}

void add(std::vector<AnticheatFinding>* out, std::string name, const char* where) {
    for (const AnticheatFinding& f : *out)
        if (f.name == name && f.where == where) return;
    out->push_back({std::move(name), where});
}

void scan_modules(uint32_t pid, const std::vector<std::string>& list, std::vector<AnticheatFinding>* out) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snap == INVALID_HANDLE_VALUE) return;
    MODULEENTRY32W m{};
    m.dwSize = sizeof(m);
    for (BOOL ok = Module32FirstW(snap, &m); ok; ok = Module32NextW(snap, &m)) {
        const std::string name = to_utf8(m.szModule);
        if (matches(name, list)) add(out, name, "loaded in the game");
    }
    CloseHandle(snap);
}

void scan_processes(const std::vector<std::string>& list, std::vector<AnticheatFinding>* out) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    PROCESSENTRY32W p{};
    p.dwSize = sizeof(p);
    for (BOOL ok = Process32FirstW(snap, &p); ok; ok = Process32NextW(snap, &p)) {
        const std::string name = to_utf8(p.szExeFile);
        if (matches(name, list)) add(out, name, "running process");
    }
    CloseHandle(snap);
}

void scan_drivers(const std::vector<std::string>& list, std::vector<AnticheatFinding>* out) {
    LPVOID bases[2048];
    DWORD needed = 0;
    if (!EnumDeviceDrivers(bases, sizeof(bases), &needed)) return;
    const size_t count = std::min<size_t>(needed, sizeof(bases)) / sizeof(LPVOID);
    for (size_t i = 0; i < count; ++i) {
        wchar_t name[MAX_PATH];
        if (!GetDeviceDriverBaseNameW(bases[i], name, MAX_PATH)) continue;
        const std::string n = to_utf8(name);
        if (matches(n, list)) add(out, n, "kernel driver");
    }
}

void scan_folder(const std::filesystem::path& dir, const std::vector<std::string>& list, std::vector<AnticheatFinding>* out) {
    std::error_code ec;
    if (dir.empty() || !std::filesystem::is_directory(dir, ec)) return;
    constexpr int kMaxDepth = 2;
    constexpr int kMaxEntries = 20000;
    int seen = 0;
    std::filesystem::recursive_directory_iterator it(dir, std::filesystem::directory_options::skip_permission_denied, ec), end;
    for (; !ec && it != end && seen < kMaxEntries; it.increment(ec), ++seen) {
        if (it.depth() >= kMaxDepth) it.disable_recursion_pending();
        const std::string name = to_utf8(it->path().filename().wstring());
        if (matches(name, list)) add(out, name, "game folder");
    }
}

}  // namespace

const std::vector<std::string>& builtin_anticheat_names() {
    static const std::vector<std::string> names = {
        "EasyAntiCheat",  // Epic Easy Anti-Cheat (EasyAntiCheat.exe, EasyAntiCheat_EOS.dll, ...)
        "BattlEye",       "BEClient",    "BEService", "BEDaisy",  // BattlEye
        "vgc",            "vgk",         "Vanguard",              // Riot Vanguard
        "EAAntiCheat",                                            // EA anti-cheat
        "XIGNCODE",       "GameGuard",   "nProtect",              // XIGNCODE3, GameGuard
        "Hyperion",       "RobloxPlayerBeta",                     // Roblox
        "faceit",         "ESEA",        "PnkBstr",   "mhyprot",  // FACEIT, ESEA, PunkBuster, miHoYo
        "TenProtect",     "ACE-Base",    "EQU8",
    };
    return names;
}

bool matches_anticheat(std::string_view name, const std::vector<std::string>& blocklist) { return matches(name, blocklist); }

std::vector<AnticheatFinding> scan_anticheat_modules(const std::vector<std::string>& user_blocklist, uint32_t pid) {
    std::vector<AnticheatFinding> found;
    scan_modules(pid, merged(user_blocklist), &found);
    return found;
}

std::vector<AnticheatFinding> scan_anticheat(const std::vector<std::string>& user_blocklist, std::optional<uint32_t> pid,
                                             const std::filesystem::path& game_dir) {
    const std::vector<std::string> list = merged(user_blocklist);
    std::vector<AnticheatFinding> found;
    if (pid) scan_modules(*pid, list, &found);
    scan_processes(list, &found);
    scan_drivers(list, &found);
    scan_folder(game_dir, list, &found);
    return found;
}

}  // namespace rec
