// The host's end of the shared memory shared with the hook DLL in one game (recorder plan §3.1, §7):
// the control block, the hook log ring and the command event. The host creates them before injecting;
// the hook only opens them.
#pragma once

#include <windows.h>

#include <cstdint>
#include <memory>
#include <string>

#include "rec/protocol.h"

namespace rec {

struct HookStats {
    proto::HookState state = proto::HookState::None;
    uint32_t backend = 0;      // proto::Api bit of the swap chain being measured, 0 = none yet
    uint32_t hooked_apis = 0;
    uint32_t width = 0, height = 0, format = 0;
    uint32_t error_code = 0;
    uint64_t present_count = 0;
    uint64_t ignored = 0;      // Present calls from other swap chains
    double fps = 0;            // game frame rate over about the last second
    double hook_cost_us = 0;   // average own time per Present since the previous sample
    double hook_cost_max_us = 0;  // worst since the previous sample
    bool hook_alive = false;   // the hook's heartbeat is recent
    proto::CaptureState capture_state = proto::CaptureState::Off;
    uint32_t display_refresh_hz = 0;
    uint64_t frames_captured = 0, gpu_backlog = 0, ring_drops = 0;
};

class HookLink {
public:
    ~HookLink();

    // Creates the shared objects for `game_pid` (call before injecting). If a hook from an earlier
    // host is still loaded in that process, its objects exist already: they are taken over
    // (*took_over = true) and the old hook carries on.
    static std::unique_ptr<HookLink> open_or_create(uint32_t game_pid, std::string* error, bool* took_over = nullptr);

    // Opens objects that exist already (rec detach); nullptr if there are none.
    static std::unique_ptr<HookLink> open_existing(uint32_t game_pid, std::string* error);

    uint32_t game_pid() const { return pid_; }
    proto::ControlBlock* control() { return ctl_; }

    // The host that owned the objects before a takeover (0 if none): lets the caller refuse to take
    // a game away from another rec that is still running.
    uint32_t previous_host_pid() const { return previous_host_; }

    // Host heartbeat; the hook goes idle if it stops for 3 seconds (E1401). Call a few times a second.
    void heartbeat();

    // Takes the hook's log records and writes them to the application log. Returns how many.
    int drain_log();

    // Current statistics. Call regularly: the frame rate is measured over the interval between calls.
    HookStats sample();

    // Asks the hook to remove itself; poll sample().state for Detached.
    void request_detach();

private:
    HookLink() = default;
    bool map(bool create, std::string* error, bool* existed);

    uint32_t pid_ = 0;
    HANDLE ctl_map_ = nullptr, log_map_ = nullptr, cmd_event_ = nullptr;
    proto::ControlBlock* ctl_ = nullptr;
    proto::LogRing* ring_ = nullptr;
    int64_t qpc_freq_ = 0;

    struct Point {
        uint64_t qpc, count, cost_total;
    };
    Point history_[16]{};
    int history_size_ = 0;
    uint64_t reported_dropped_ = 0;
    uint32_t previous_host_ = 0;
};

const char* hook_state_name(proto::HookState state);

}  // namespace rec
