// Watching a hooked game: the host's main loop while a hook is attached (recorder plan §3.2, §12.4).
// Milestone M1: heartbeat, hook log, live status line with the game's frame rate. Recording (hotkey,
// capture, encoding) is added to this loop by later milestones.
#pragma once

#include <atomic>
#include <string>
#include <vector>

#include "rec/hooklink.h"
#include "rec/inject.h"

namespace rec {

struct WatchOptions {
    int duration_s = 0;                    // stop after this many seconds (0 = until Ctrl+C or the game exits)
    bool force = false;                    // --force: don't act on anti-cheat modules that appear
    std::vector<std::string> blocklist;    // safety.anticheat_blocklist
};

enum class WatchEnd {
    UserStop,        // Ctrl+C
    Duration,        // --duration reached
    TargetExited,    // the game ended (W2401)
    HookFailed,      // the hook could not install, or its state went to Failed
    HookDetached,    // someone ran `rec detach`
    AnticheatFound,  // an anti-cheat module showed up in the game; the hook was removed (E1004)
};

// One line for the live display, e.g.
//   "○ IDLE javaw.exe D3D11 1280x720 | game 59.9 fps | hook 0.004 ms (max 0.021)"
std::string format_status_line(const std::string& exe_name, const HookStats& stats);

// "D3D11" etc. for a proto::Api bit.
const char* backend_name(uint32_t backend);

// Waits (draining the hook log) until the hook has installed or failed. False on failure or timeout,
// with the reason in *error. `target` is watched too: false if the game exits first.
bool wait_for_hook(HookLink& link, const Target& target, int timeout_ms, std::string* error);

// Runs until the end condition; `stop` is set by Ctrl+C. Detaches the hook when the user stops it.
WatchEnd watch_hooked_game(const Target& target, HookLink& link, const WatchOptions& options, const std::atomic<bool>& stop);

}  // namespace rec
