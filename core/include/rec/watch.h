// Watching a hooked game: the host's main loop while a hook is attached (recorder plan §3.2, §12.4).
// Heartbeat, hook log, live status line, the hotkey and the recording session (frames are encoded
// and written to an AVI file by the session's pipeline).
#pragma once

#include <atomic>
#include <filesystem>
#include <string>
#include <vector>

#include "rec/config.h"
#include "rec/hooklink.h"
#include "rec/inject.h"
#include "rec/session.h"

namespace rec {

struct WatchOptions {
    int duration_s = 0;                    // stop after this many seconds (0 = until Ctrl+C or the game exits)
    bool force = false;                    // --force: don't act on anti-cheat modules that appear
    std::vector<std::string> blocklist;    // safety.anticheat_blocklist
    const Config* config = nullptr;        // recording settings and hotkey; null: measure only
    int record_for_s = 0;                  // > 0: record this many seconds as soon as the game presents (scripts, benchmarks)
    std::filesystem::path save_frame;      // test aid: save one captured frame of the recording as a PNG
    uint64_t save_frame_index = 30;        // ... this one (0-based)
};

enum class WatchEnd {
    UserStop,        // Ctrl+C
    Duration,        // --duration reached, or --record-for finished
    TargetExited,    // the game ended (W2401)
    HookFailed,      // the hook could not install, or its state went to Failed
    HookDetached,    // someone ran `rec detach`
    AnticheatFound,  // an anti-cheat module showed up in the game; the hook was removed (E1004)
};

// One line for the live display, e.g.
//   "○ IDLE javaw.exe D3D11 1280x720 | game 59.9 fps | hook 0.4 us (max 2.1) | press F9 to record"
std::string format_status_line(const std::string& exe_name, const HookStats& stats, const std::string& hotkey = {});

// "● REC 00:00:07 javaw.exe D3D11 1366x745 -> 1280x720 @60 | game 59.9 fps | hook 0.31 ms | 420 frames | backlog 0 drops 0"
std::string format_recording_line(const std::string& exe_name, const HookStats& stats, const OutputPlan& plan, const LiveStats& live,
                                  bool stopping, bool locked);

// "D3D11" etc. for a proto::Api bit.
const char* backend_name(uint32_t backend);

// Waits (draining the hook log) until the hook has installed or failed. False on failure or timeout,
// with the reason in *error. `target` is watched too: false if the game exits first.
bool wait_for_hook(HookLink& link, const Target& target, int timeout_ms, std::string* error);

// Runs until the end condition; `stop` is set by Ctrl+C. Detaches the hook when the user stops it.
WatchEnd watch_hooked_game(const Target& target, HookLink& link, const WatchOptions& options, const std::atomic<bool>& stop);

}  // namespace rec
