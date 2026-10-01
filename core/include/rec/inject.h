// Getting the hook DLL into a game (recorder plan §4.1): VirtualAllocEx + WriteProcessMemory +
// CreateRemoteThread(LoadLibraryW). x64 targets only for now; 32-bit games need rec_inject32 (M7).
#pragma once

#include <windows.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace rec {

// A process opened with the rights injection needs.
class Target {
public:
    Target() = default;
    Target(const Target&) = delete;
    Target& operator=(const Target&) = delete;
    Target(Target&& o) noexcept { *this = std::move(o); }
    Target& operator=(Target&& o) noexcept;
    ~Target();

    // Opens `pid`. On failure *error says why; *elevated_target is set when the cause is that the
    // target runs elevated and rec does not (E1003).
    static bool open(uint32_t pid, Target* out, std::string* error, bool* elevated_target = nullptr);

    // Takes ownership of a handle from CreateProcessW (full access).
    static Target adopt(HANDLE process, uint32_t pid);

    uint32_t pid() const { return pid_; }
    HANDLE handle() const { return process_; }
    bool is_64bit() const { return is_64bit_; }
    std::string name() const { return name_; }          // executable file name
    std::filesystem::path exe_path() const { return exe_path_; }

    // True once the process has ended.
    bool exited() const;
    // Waits up to `ms` for the process to end.
    bool wait_exit(uint32_t ms) const;

private:
    uint32_t pid_ = 0;
    HANDLE process_ = nullptr;
    bool is_64bit_ = true;
    std::string name_;
    std::filesystem::path exe_path_;
};

// Loads `dll` into the target and waits for LoadLibraryW to return. False (with the reason) if the
// load failed in the target, e.g. blocked by its code-integrity policy, or a dependency is missing.
bool inject_dll(const Target& target, const std::filesystem::path& dll, std::string* error, uint32_t timeout_ms = 15000);

// The hook DLL for this bitness, next to rec.exe. Empty path if it isn't there.
std::filesystem::path hook_dll_path(bool is_64bit, std::string* error);

// A game started suspended, so hooks go in before it creates its graphics device.
struct LaunchedProcess {
    Target target;
    HANDLE main_thread = nullptr;
    ~LaunchedProcess();
    bool resume();        // ResumeThread on the main thread
    void terminate();     // for when injection failed: the user did not ask for a game without the hook
};

// CreateProcessW(CREATE_SUSPENDED) in the game's own folder. *elevated_required is set for
// ERROR_ELEVATION_REQUIRED (the game needs administrator rights).
bool launch_suspended(const std::filesystem::path& exe, const std::vector<std::string>& args, LaunchedProcess* out,
                      std::string* error, bool* elevated_required = nullptr);

}  // namespace rec
