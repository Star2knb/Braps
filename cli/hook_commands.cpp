#include "hook_commands.h"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>

#include "rec/anticheat.h"
#include "rec/hooklink.h"
#include "rec/inject.h"
#include "rec/log.h"
#include "rec/paths.h"
#include "rec/procscan.h"
#include "rec/watch.h"

namespace rec_cli {
namespace {

std::atomic<bool> g_stop{false};

BOOL WINAPI on_console_event(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT || type == CTRL_CLOSE_EVENT) {
        g_stop.store(true);
        return TRUE;  // we detach cleanly, then exit
    }
    return FALSE;
}

int fail(const std::string& message) {
    std::fprintf(stderr, "%s\n", message.c_str());
    rec::logging::get(rec::Subsystem::Cli).warn("{}", message);
    return 1;
}

// Refuses (E1004) when anti-cheat components are present, unless --force. True: go ahead.
bool anticheat_allows(const std::string& game, const std::vector<rec::AnticheatFinding>& found, bool force) {
    if (found.empty()) return true;
    std::string list;
    for (const rec::AnticheatFinding& f : found) list += "\n  - " + f.name + " (" + f.where + ")";
    if (force) {
        rec::logging::event(rec::Ev::AnticheatBlocked, "{}: {} found; continuing because of --force", game, found.front().name);
        std::fprintf(stderr, "Warning: anti-cheat components found, continuing because of --force:%s\n", list.c_str());
        return true;
    }
    rec::logging::event(rec::Ev::AnticheatBlocked, "{}: {} ({})", game, found.front().name, found.front().where);
    std::fprintf(stderr,
                 "rec: not hooking %s: anti-cheat components found:%s\n"
                 "Injecting into a game protected by anti-cheat can get your account banned.\n"
                 "If this is a game you own and know is safe to use offline, run again with --force.\n",
                 game.c_str(), list.c_str());
    return false;
}

const char* end_text(rec::WatchEnd end) {
    switch (end) {
    case rec::WatchEnd::UserStop: return "Stopped; the hook was removed from the game.";
    case rec::WatchEnd::Duration: return "Time is up; the hook was removed from the game.";
    case rec::WatchEnd::TargetExited: return "The game exited.";
    case rec::WatchEnd::HookFailed: return "The hook could not install (see the log).";
    case rec::WatchEnd::HookDetached: return "The hook was removed from the game.";
    case rec::WatchEnd::AnticheatFound: return "The hook was removed (anti-cheat detected).";
    }
    return "";
}

// Common tail of launch and attach: wait for the hook, then watch the game.
int hook_and_watch(const rec::Target& target, rec::HookLink& link, const rec::Config& cfg, const HookCommandOptions& options) {
    std::string error;
    if (!rec::wait_for_hook(link, target, 10000, &error)) return fail("rec: " + error);
    std::printf("Hooked %s (pid %u). Press Ctrl+C to detach and exit.\n", target.name().c_str(), target.pid());
    g_stop.store(false);
    SetConsoleCtrlHandler(on_console_event, TRUE);

    rec::WatchOptions watch;
    watch.duration_s = options.duration_s;
    watch.force = options.force;
    watch.blocklist = cfg.safety.anticheat_blocklist;
    const rec::WatchEnd end = rec::watch_hooked_game(target, link, watch, g_stop);
    std::printf("%s\n", end_text(end));
    return end == rec::WatchEnd::HookFailed ? 1 : end == rec::WatchEnd::AnticheatFound ? 1 : 0;
}

bool resolve_pid(std::optional<unsigned> pid, const std::optional<std::string>& name, uint32_t* out, std::string* error) {
    if (pid) {
        *out = *pid;
        return true;
    }
    const std::vector<uint32_t> pids = rec::find_processes_by_name(*name);
    if (pids.empty()) {
        *error = "no running process named " + *name;
        return false;
    }
    if (pids.size() > 1) {
        *error = "several processes are named " + *name + " (pids:";
        for (uint32_t p : pids) *error += " " + std::to_string(p);
        *error += "); choose one with --pid";
        return false;
    }
    *out = pids[0];
    return true;
}

}  // namespace

int cmd_list() {
    const std::vector<rec::ProcessInfo> processes = rec::list_graphics_processes();
    if (processes.empty()) {
        std::printf("No process with a graphics API loaded.\n");
        return 0;
    }
    std::printf("%-8s %-32s %-5s %-28s %s\n", "PID", "NAME", "BITS", "GRAPHICS", "HOOK");
    for (const rec::ProcessInfo& p : processes)
        std::printf("%-8u %-32s %-5d %-28s %s\n", p.pid, p.name.c_str(), p.is_64bit ? 64 : 32, rec::api_names(p.apis).c_str(),
                    p.hook_loaded ? "loaded" : "-");
    std::printf("\n%zu process(es). Games are the ones with a window and D3D11/D3D12/OpenGL/Vulkan; "
                "32-bit games are supported from recorder milestone M7.\n",
                processes.size());
    return 0;
}

int cmd_launch(const rec::Config& cfg, const std::string& exe, const std::vector<std::string>& game_args,
               const HookCommandOptions& options) {
    const std::filesystem::path path = std::filesystem::absolute(rec::from_utf8(exe));
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) return fail("rec launch: can't find " + exe);

    DWORD binary_type = 0;
    if (!GetBinaryTypeW(path.c_str(), &binary_type)) return fail("rec launch: " + exe + " is not a Windows program");
    if (binary_type == SCS_32BIT_BINARY)
        return fail("rec launch: " + exe + " is a 32-bit program; 32-bit games are supported from recorder milestone M7");
    if (binary_type != SCS_64BIT_BINARY) return fail("rec launch: " + exe + " is not a 64-bit Windows program");

    const std::string game = rec::to_utf8(path.filename().wstring());
    // The game isn't running yet, so look at the files in its folder (and at what's already running).
    if (!anticheat_allows(game, rec::scan_anticheat(cfg.safety.anticheat_blocklist, std::nullopt, path.parent_path()), options.force))
        return 1;

    std::string error;
    const std::filesystem::path dll = rec::hook_dll_path(true, &error);
    if (dll.empty()) return fail("rec launch: " + error);

    rec::LaunchedProcess game_process;
    bool elevated = false;
    if (!rec::launch_suspended(path, game_args, &game_process, &error, &elevated)) {
        if (elevated) rec::logging::event(rec::Ev::TargetElevated, "{}", game);
        return fail("rec launch: " + error);
    }
    const uint32_t pid = game_process.target.pid();

    // Shared memory first, then the hook, then let the game run: hooks are in before its first Direct3D call.
    std::unique_ptr<rec::HookLink> link = rec::HookLink::open_or_create(pid, &error);
    if (!link) {
        game_process.terminate();
        return fail("rec launch: " + error);
    }
    if (!rec::inject_dll(game_process.target, dll, &error)) {
        game_process.terminate();
        return fail("rec launch: couldn't hook " + game + ": " + error);
    }
    if (!game_process.resume()) {
        game_process.terminate();
        return fail("rec launch: couldn't resume " + game);
    }
    std::printf("Started %s (pid %u) with the hook loaded.\n", game.c_str(), pid);
    return hook_and_watch(game_process.target, *link, cfg, options);
}

int cmd_attach(const rec::Config& cfg, std::optional<unsigned> pid_arg, const std::optional<std::string>& name,
               const HookCommandOptions& options) {
    std::string error;
    uint32_t pid = 0;
    if (!resolve_pid(pid_arg, name, &pid, &error)) return fail("rec attach: " + error);
    if (pid == GetCurrentProcessId()) return fail("rec attach: that is rec itself");

    rec::Target target;
    bool elevated = false;
    if (!rec::Target::open(pid, &target, &error, &elevated)) {
        if (elevated) rec::logging::event(rec::Ev::TargetElevated, "pid {}", pid);
        return fail("rec attach: " + error);
    }
    if (!target.is_64bit())
        return fail("rec attach: " + target.name() + " is a 32-bit process; 32-bit games are supported from recorder milestone M7");
    if (!anticheat_allows(target.name(), rec::scan_anticheat(cfg.safety.anticheat_blocklist, pid, target.exe_path().parent_path()), options.force))
        return 1;

    const std::filesystem::path dll = rec::hook_dll_path(true, &error);
    if (dll.empty()) return fail("rec attach: " + error);

    bool took_over = false;
    std::unique_ptr<rec::HookLink> link = rec::HookLink::open_or_create(pid, &error, &took_over);
    if (!link) return fail("rec attach: " + error);
    if (took_over) {
        // An earlier rec left its hook in the game. Don't take the game away from one that's still running.
        const uint32_t previous = link->previous_host_pid();
        if (previous && previous != GetCurrentProcessId()) {
            HANDLE other = OpenProcess(SYNCHRONIZE, FALSE, previous);
            const bool alive = other && WaitForSingleObject(other, 0) == WAIT_TIMEOUT;
            if (other) CloseHandle(other);
            if (alive) return fail("rec attach: " + target.name() + " is already attached by rec (pid " + std::to_string(previous) + ")");
        }
        std::printf("The hook from an earlier rec is still in %s; reusing it.\n", target.name().c_str());
    } else if (!rec::inject_dll(target, dll, &error)) {
        return fail("rec attach: couldn't hook " + target.name() + ": " + error);
    }
    return hook_and_watch(target, *link, cfg, options);
}

int cmd_detach(std::optional<unsigned> pid_arg, const std::optional<std::string>& name) {
    std::vector<uint32_t> pids;
    if (pid_arg || name) {
        std::string error;
        uint32_t pid = 0;
        if (!resolve_pid(pid_arg, name, &pid, &error)) return fail("rec detach: " + error);
        pids.push_back(pid);
    } else {
        pids = rec::find_hooked_processes();
    }
    if (pids.empty()) {
        std::printf("No process has the recorder hook loaded.\n");
        return 0;
    }
    int status = 0;
    for (uint32_t pid : pids) {
        std::string error;
        std::unique_ptr<rec::HookLink> link = rec::HookLink::open_existing(pid, &error);
        if (!link) {
            std::fprintf(stderr, "pid %u: %s\n", pid, error.c_str());
            status = 1;
            continue;
        }
        link->request_detach();
        bool detached = false;
        for (int i = 0; i < 400 && !detached; ++i) {  // up to 8 s
            link->drain_log();
            detached = link->sample().state == rec::proto::HookState::Detached;
            if (!detached) std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        link->drain_log();
        // The DLL unloads a moment after the state changes.
        rec::ProcessInfo info;
        bool unloaded = false;
        for (int i = 0; i < 50 && detached && !unloaded; ++i) {
            unloaded = rec::process_info(pid, &info) && !info.hook_loaded;
            if (!unloaded) std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        if (detached) {
            std::printf("pid %u: detached%s.\n", pid, unloaded ? ", hook unloaded" : "; the hook DLL stays loaded (a thread was still inside it)");
        } else {
            std::fprintf(stderr, "pid %u: the hook did not answer.\n", pid);
            status = 1;
        }
    }
    return status;
}

}  // namespace rec_cli
