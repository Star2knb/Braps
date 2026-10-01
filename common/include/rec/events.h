// Event codes (recorder plan §10.1, §10.4). Shared by the host and the hook DLLs (x86 and x64):
// constant data only, no allocation.
//
// A code is a level letter and four digits; the first digit is the subsystem (1 hook, 2 transport,
// 3 encoder/rate, 4 disk, 5 audio, 6 system, 7 CLI/hotkeys). The whole code is the identifier: the
// plan gives two numbers two letters each (I1301 / W1301), so hook log records carry level + number.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace rec {

enum class Level : uint8_t { Debug, Info, Warn, Error, Fatal };

enum class Subsystem : uint8_t { Hook = 1, Transport = 2, Encoder = 3, Disk = 4, Audio = 5, System = 6, Cli = 7 };

// X(id, code, name, description). Keep in code order; codes added beyond the plan say so.
#define REC_EVENTS(X)                                                                                        \
    X(TargetElevated, "E1003", "target_elevated", "Target runs elevated and rec does not")                  \
    X(AnticheatBlocked, "E1004", "anticheat_blocked", "Anti-cheat module in the target; injection refused") \
    X(BackendSelected, "I1101", "backend_selected", "Graphics backend selected")                            \
    X(OverlaysDetected, "I1102", "overlays_detected", "Overlay modules found in the target")                \
    X(HookInstallDeferred, "I1103", "hook_install_deferred", "Graphics DLL not loaded yet; install deferred") \
    X(LocateFailed, "E1105", "locate_failed", "Address lookup failed; that API stays unhooked")             \
    X(VtablePointerOutsideModule, "W1106", "vtable_pointer_outside_module",                                 \
      "Vtable pointer outside the expected module; hook skipped")                                           \
    X(GpuBacklog, "W1201", "gpu_backlog", "All staging slots pending; frame skipped")                       \
    X(RingFull, "W1202", "ring_full", "Shared frame ring full; frame dropped")                               \
    X(SlowHook, "W1203", "slow_hook", "Hook cost above slow_hook_ms")                                       \
    X(HdrApproximated, "W1204", "hdr_approximated", "HDR back buffer clamped to SDR")                        \
    X(UnsupportedFormat, "E1205", "unsupported_format", "Unsupported back-buffer format; capture off")      \
    X(UnsupportedGlVersion, "E1206", "unsupported_gl_version", "Unsupported OpenGL version; capture off")   \
    X(GlContextChanged, "I1207", "gl_context_changed", "OpenGL context changed; objects recreated")          \
    X(BackbufferResized, "I1208", "backbuffer_resized", "Back buffer resized")                              \
    X(DeviceRemoved, "E1209", "device_removed", "Device removed or reset")                                  \
    X(GameStall, "W1210", "game_stall", "Game frame time above 4 x T or 250 ms")                            \
    X(DisplayRefresh, "I1301", "display_refresh", "Display refresh rate (frame-rate cap)")                  \
    X(PacingError, "W1301", "pacing_error", "Capture tick more than 1 ms late")                             \
    X(HostLost, "E1401", "host_lost", "Host stopped responding; capture idle")                              \
    X(LateFrameDropped, "W2301", "late_frame_dropped", "Frame tick went backwards; frame dropped")          \
    X(TargetExited, "W2401", "target_exited", "Target process exited")                                      \
    X(HookLogOverflow, "W2402", "hook_log_overflow", "Hook log ring full; records lost")                    \
    X(RateLevelChange, "W3101", "rate_level_change", "Rate-control level changed")                          \
    X(EncoderOverloaded, "W3102", "encoder_overloaded", "Average encode time above budget")                 \
    X(SlowEncode, "W3103", "slow_encode", "Frame encode longer than the frame interval")                    \
    X(DiskMayBeTooSlow, "W4001", "disk_may_be_too_slow", "Disk benchmark below 1.2x the needed rate")       \
    X(SlowWrite, "W4101", "slow_write", "Write latency above slow_write_ms")                                  \
    X(VerySlowWrite, "E4102", "very_slow_write", "Write latency above very_slow_write_ms")                  \
    X(LowDiskSpace, "W4103", "low_disk_space", "Free space below low_space_gb")                             \
    X(CriticalDiskSpace, "E4104", "critical_disk_space", "Free space below critical_space_gb; stopped")     \
    X(WriteFailed, "E4105", "write_failed", "Write failed; recording stopped and file finalised")           \
    X(FileSplit, "I4106", "file_split", "Output file split")                                                \
    X(ProcessLoopbackUnavailable, "I5001", "process_loopback_unavailable",                                  \
      "Game-audio capture unavailable; using system audio")                                                 \
    X(AudioDiscontinuity, "W5101", "audio_discontinuity", "Audio data discontinuity")                       \
    X(AvDriftCorrected, "W5102", "av_drift_corrected", "Audio/video drift corrected")                       \
    X(AudioDeviceChanged, "W5103", "audio_device_changed", "Audio device changed or lost; restarted")       \
    X(SessionStart, "I6001", "session_start", "Session start: system, target and settings")                 \
    X(SessionStats, "I6002", "session_stats", "Per-second recording statistics")                            \
    X(PowerStateChange, "W6101", "power_state_change", "Power source or battery saver changed")             \
    X(HighSystemCpu, "W6102", "high_system_cpu", "System CPU above 95% for 3 s")                            \
    X(UnhandledException, "F6999", "unhandled_exception", "Unhandled exception; minidump written")          \
    X(Hotkey, "I7001", "hotkey", "Hotkey pressed")                                                           \
    X(HotkeyIgnored, "W7002", "hotkey_ignored", "Hotkey ignored (debounced or busy)")                        \
    X(HotkeyRegistrationFailed, "E7003", "hotkey_registration_failed", "Hotkey taken; using a keyboard hook") \
    X(CommandStart, "I7004", "command", "rec command started (added in M0)")                                \
    X(DoctorResult, "I7005", "doctor_result", "rec doctor summary (added in M0)")                           \
    X(ConfigChanged, "I7006", "config_changed", "Configuration changed (added in M0)")

enum class Ev : uint16_t {
#define REC_EV_ENUM(id, code, name, desc) id,
    REC_EVENTS(REC_EV_ENUM)
#undef REC_EV_ENUM
};

struct EventInfo {
    Ev id;
    const char* code;         // "W4101"
    const char* name;         // "slow_write"
    const char* description;  // short English text
};

inline constexpr EventInfo kEvents[] = {
#define REC_EV_INFO(id, code, name, desc) {Ev::id, code, name, desc},
    REC_EVENTS(REC_EV_INFO)
#undef REC_EV_INFO
};
inline constexpr size_t kEventCount = sizeof(kEvents) / sizeof(kEvents[0]);

constexpr const EventInfo& event_info(Ev e) { return kEvents[size_t(e)]; }

constexpr Level level_from_letter(char c) {
    return c == 'D' ? Level::Debug : c == 'I' ? Level::Info : c == 'W' ? Level::Warn : c == 'E' ? Level::Error : Level::Fatal;
}
constexpr Level event_level(Ev e) { return level_from_letter(event_info(e).code[0]); }
constexpr Subsystem event_subsystem(Ev e) { return Subsystem(event_info(e).code[1] - '0'); }
constexpr uint16_t event_number(Ev e) {
    const char* c = event_info(e).code;
    return uint16_t((c[1] - '0') * 1000 + (c[2] - '0') * 100 + (c[3] - '0') * 10 + (c[4] - '0'));
}

// Every code is a level letter, four digits, and a known subsystem digit; checked at compile time.
constexpr bool events_well_formed() {
    for (size_t i = 0; i < kEventCount; ++i) {
        const EventInfo& e = kEvents[i];
        if (size_t(e.id) != i) return false;
        const char* c = e.code;
        if (c[0] != 'D' && c[0] != 'I' && c[0] != 'W' && c[0] != 'E' && c[0] != 'F') return false;
        for (int k = 1; k <= 4; ++k)
            if (c[k] < '0' || c[k] > '9') return false;
        if (c[5] != 0 || c[1] < '1' || c[1] > '7') return false;
    }
    return true;
}
static_assert(events_well_formed(), "malformed event code in REC_EVENTS");

// Looks up an event by code ("W4101") or name ("slow_write"); nullptr if unknown.
const EventInfo* find_event(std::string_view code_or_name);

// Upper-case level names as written in logs ("WARN"), and short subsystem names ("writer").
const char* level_name(Level level);
const char* subsystem_name(Subsystem subsystem);

}  // namespace rec
