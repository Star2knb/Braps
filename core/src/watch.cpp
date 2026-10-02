#include "rec/watch.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <thread>

#include "rec/anticheat.h"
#include "rec/hotkeys.h"
#include "rec/log.h"
#include "rec/options.h"
#include "rec/procscan.h"

namespace rec {
namespace {

using Clock = std::chrono::steady_clock;

constexpr auto kTick = std::chrono::milliseconds(250);
constexpr auto kAnticheatRescan = std::chrono::seconds(3);  // modules of a launched game load late

bool stdout_is_console() {
    DWORD mode = 0;
    return GetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), &mode) != 0;
}

// Prints a message on its own line, clearing the status line first when there is one.
void say(bool console, const std::string& text) {
    if (console) std::printf("\r%-130s\r", "");
    std::printf("%s\n", text.c_str());
    std::fflush(stdout);
}

std::string clock_text(double seconds) {
    const int s = int(seconds);
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d", s / 3600, s / 60 % 60, s % 60);
    return buf;
}

}  // namespace

const char* backend_name(uint32_t backend) {
    switch (backend) {
    case proto::kApiD3D9: return "D3D9";
    case proto::kApiD3D10: return "D3D10";
    case proto::kApiD3D11: return "D3D11";
    case proto::kApiD3D12: return "D3D12";
    case proto::kApiOpenGL: return "OpenGL";
    case proto::kApiVulkan: return "Vulkan";
    case proto::kApiDXGI: return "DXGI";
    default: return "";
    }
}

namespace {

// "1.3 us (max 2.0)" for the idle hook, milliseconds once it does real work.
std::string cost_text(double avg_us, double max_us) {
    char cost[64];
    if (max_us < 100.0)
        std::snprintf(cost, sizeof(cost), "%.1f us (max %.1f)", avg_us, max_us);
    else
        std::snprintf(cost, sizeof(cost), "%.2f ms (max %.2f)", avg_us / 1000.0, max_us / 1000.0);
    return cost;
}

}  // namespace

std::string format_status_line(const std::string& exe_name, const HookStats& s, const std::string& hotkey) {
    char buf[256];
    if (s.state == proto::HookState::Waiting) {
        std::snprintf(buf, sizeof(buf), "○ IDLE %s | hook installs when the game loads Direct3D 11 or OpenGL", exe_name.c_str());
    } else if (!s.backend) {
        std::snprintf(buf, sizeof(buf), "○ IDLE %s | hooked, waiting for the first Present", exe_name.c_str());
    } else if (s.state == proto::HookState::Idle) {
        std::snprintf(buf, sizeof(buf), "○ IDLE %s %s | hook idle: host lost", exe_name.c_str(), backend_name(s.backend));
    } else {
        char fps[16] = "--";
        if (s.fps > 0) std::snprintf(fps, sizeof(fps), "%.1f", s.fps);
        std::snprintf(buf, sizeof(buf), "○ IDLE %s %s %ux%u | game %s fps | hook %s%s%s", exe_name.c_str(), backend_name(s.backend), s.width,
                      s.height, fps, cost_text(s.hook_cost_us, s.hook_cost_max_us).c_str(), hotkey.empty() ? "" : " | press ",
                      hotkey.empty() ? "" : (hotkey + " to record").c_str());
    }
    return buf;
}

std::string format_recording_line(const std::string& exe_name, const HookStats& s, const OutputPlan& plan, const LiveStats& live, bool stopping,
                                  bool locked) {
    char fps[16] = "--";
    if (s.fps > 0) std::snprintf(fps, sizeof(fps), "%.1f", s.fps);
    char free_text[24] = "";
    if (live.free_gb >= 0) std::snprintf(free_text, sizeof(free_text), " | free %.0f GB", live.free_gb);
    char buf[420];
    std::snprintf(buf, sizeof(buf),
                  "\u25CF %s %s %s %ux%u->%ux%u @%u%s | game %s fps | hook %.2f ms | enc %.1f ms | ratio %.1f | lossless | queue %d%% | disk %.0f MB/s | "
                  "drops %llu | %llu frames (%llu DUP)%s",
                  stopping ? "STOPPING" : ("REC " + clock_text(live.seconds)).c_str(), exe_name.c_str(), backend_name(s.backend), s.width, s.height,
                  plan.width, plan.height, plan.fps, locked ? " lock" : "", fps, live.avg_cost_ms, live.encode_ms, live.ratio, live.queue_pct,
                  live.write_mb_s, (unsigned long long)(live.gpu_backlog + live.ring_drops), (unsigned long long)live.output_frames,
                  (unsigned long long)live.dup_filled, free_text);
    return buf;
}

bool wait_for_hook(HookLink& link, const Target& target, int timeout_ms, std::string* error) {
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
    HookStats s;
    while (Clock::now() < deadline) {
        link.heartbeat();
        link.drain_log();
        s = link.sample();
        if (s.state == proto::HookState::Hooked || s.state == proto::HookState::Waiting) return true;
        if (s.state == proto::HookState::Failed) {
            *error = s.error_code ? "the hook could not install (see the log, error " + std::to_string(s.error_code) + ")"
                                  : "the hook could not install";
            return false;
        }
        if (target.exited()) {
            *error = "the game exited";
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    *error = std::string("the hook did not start in time (state: ") + hook_state_name(s.state) +
             (s.hook_alive ? "" : ", the hook's thread is not running") + ")";
    return false;
}

WatchEnd watch_hooked_game(const Target& target, HookLink& link, const WatchOptions& options, const std::atomic<bool>& stop) {
    const bool console = stdout_is_console();
    const Config* cfg = options.config;
    const auto start = Clock::now();
    auto next_tick = start;
    auto next_plain_line = start;
    bool rescanned = false;
    WatchEnd end = WatchEnd::UserStop;

    // Hotkey and recording (M2).
    Hotkeys hotkeys;
    bool hotkey_ok = false;
    std::string hotkey_label;
    std::unique_ptr<RecordingSession> session;
    if (cfg) {
        session = std::make_unique<RecordingSession>(link, *cfg, target.name());
        if (!options.save_frame.empty()) session->save_frame_to(options.save_frame, options.save_frame_index);
        Hotkey key;
        std::string error;
        if (!parse_hotkey(cfg->hotkeys.toggle, &key)) {
            say(console, "The hotkey \"" + cfg->hotkeys.toggle + "\" is not valid; no hotkey.");
        } else if (!hotkeys.start(key, &error)) {
            logging::event(Ev::HotkeyRegistrationFailed, "{}", error);
            say(console, "No hotkey: " + error);
            if (cfg->hotkeys.sound) Hotkeys::play(Cue::Error);
        } else {
            hotkey_ok = true;
            hotkey_label = hotkey_name(key);
            say(console, "Press " + hotkey_label + " to start and stop recording.");
        }
    }
    bool auto_started = false;
    bool auto_finished = false;
    Clock::time_point auto_stop_at{};
    auto cue = [&](Cue c) {
        if (cfg && cfg->hotkeys.sound) Hotkeys::play(c);
    };
    auto announce_summary = [&] {
        const std::string text = session->summary().text();
        say(console, text);
    };

    for (;;) {
        const auto now = Clock::now();

        // Hotkey presses are handled as they come, not once per tick.
        if (hotkey_ok) {
            while (hotkeys.take_press()) {
                logging::event(Ev::Hotkey, "{} while {}", hotkey_label,
                               session->state() == RecordingSession::State::Idle        ? "idle"
                               : session->state() == RecordingSession::State::Recording ? "recording"
                                                                                       : "stopping");
                std::string error;
                switch (session->state()) {
                case RecordingSession::State::Idle:
                    if (session->start(&error)) {
                        cue(Cue::Start);
                        const OutputPlan& p = session->plan();
                        say(console, "Recording started: " + std::to_string(p.width) + "x" + std::to_string(p.height) + " @" + std::to_string(p.fps) + " fps" +
                                         (session->locked() ? ", game locked to the frame rate" : ""));
                    } else {
                        cue(Cue::Error);
                        say(console, "Can't start: " + error);
                    }
                    break;
                case RecordingSession::State::Recording:
                    session->request_stop();
                    cue(Cue::Stop);
                    break;
                case RecordingSession::State::Stopping:
                    logging::event(Ev::HotkeyIgnored, "pressed while stopping");
                    break;
                }
            }
        }

        if (now >= next_tick) {
            next_tick += kTick;
            link.heartbeat();
            link.drain_log();
            const HookStats stats = link.sample();

            if (target.exited()) {
                logging::event(Ev::TargetExited, "{} (pid {})", target.name(), target.pid());
                if (session && session->state() != RecordingSession::State::Idle) {
                    session->finish_now();
                    announce_summary();
                }
                end = WatchEnd::TargetExited;
                break;
            }
            if (stats.state == proto::HookState::Failed) {
                end = WatchEnd::HookFailed;
                break;
            }
            if (stats.state == proto::HookState::Detached || stats.state == proto::HookState::Detaching) {
                if (session && session->state() != RecordingSession::State::Idle) {
                    session->finish_now();
                    announce_summary();
                }
                end = WatchEnd::HookDetached;
                break;
            }
            if (stop.load()) {
                end = WatchEnd::UserStop;
                break;
            }
            if (options.duration_s && now - start >= std::chrono::seconds(options.duration_s)) {
                end = WatchEnd::Duration;
                break;
            }
            if (!rescanned && !options.force && now - start >= kAnticheatRescan) {
                rescanned = true;
                const auto found = scan_anticheat_modules(options.blocklist, target.pid());
                if (!found.empty()) {
                    logging::event(Ev::AnticheatBlocked, "{} appeared in {}; removing the hook", found.front().name, target.name());
                    say(console, target.name() + " loaded an anti-cheat module (" + found.front().name + "). Removing the hook to protect your account.");
                    end = WatchEnd::AnticheatFound;
                    break;
                }
            }

            if (session) {
                std::string message;
                if (session->take_error(&message)) {
                    cue(Cue::Error);
                    say(console, message);
                }
                if (session->poll()) announce_summary();

                // --record-for: record as soon as the game has been presenting for a moment, then stop.
                if (options.record_for_s > 0 && !auto_started && stats.backend && stats.fps > 0) {
                    std::string error;
                    if (session->start(&error)) {
                        auto_started = true;
                        auto_stop_at = now + std::chrono::seconds(options.record_for_s);
                        say(console, "Recording for " + std::to_string(options.record_for_s) + " s...");
                    } else {
                        say(console, "Can't start: " + error);
                        end = WatchEnd::HookFailed;
                        break;
                    }
                }
                if (auto_started && !auto_finished) {
                    if (session->state() == RecordingSession::State::Recording && now >= auto_stop_at) session->request_stop();
                    if (session->state() == RecordingSession::State::Idle && session->summary().valid) auto_finished = true;
                }
                if (auto_finished) {
                    end = WatchEnd::Duration;
                    break;
                }
            }

            std::string line;
            if (session && session->state() != RecordingSession::State::Idle)
                line = format_recording_line(target.name(), stats, session->plan(), session->live(), session->state() == RecordingSession::State::Stopping, session->locked());
            else
                line = format_status_line(target.name(), stats, hotkey_ok ? hotkey_label : std::string());
            if (console) {
                std::printf("\r%-130s", line.c_str());
                std::fflush(stdout);
            } else if (now >= next_plain_line) {
                std::printf("%s\n", line.c_str());
                std::fflush(stdout);
                next_plain_line += std::chrono::seconds(1);
            }
        }

        // Sleep until the next tick, or until the hotkey is pressed.
        const auto wait = std::chrono::duration_cast<std::chrono::milliseconds>(next_tick - Clock::now());
        const DWORD ms = DWORD((std::max)(std::chrono::milliseconds(1), (std::min)(wait, kTick)).count());
        if (hotkey_ok) WaitForSingleObject(hotkeys.event(), ms);
        else Sleep(ms);
    }
    if (console) std::printf("\r%-130s\r", "");

    // A recording still running is stopped properly first (Ctrl+C, --duration).
    if (session && session->state() == RecordingSession::State::Recording) session->request_stop();
    if (session && session->state() == RecordingSession::State::Stopping && end != WatchEnd::TargetExited && end != WatchEnd::HookDetached) {
        const auto deadline = Clock::now() + std::chrono::seconds(4);
        while (Clock::now() < deadline && session->state() != RecordingSession::State::Idle) {
            link.heartbeat();
            link.drain_log();
            if (session->poll()) {
                announce_summary();
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        if (session->state() != RecordingSession::State::Idle) {
            session->finish_now();
            announce_summary();
        }
    }
    hotkeys.stop();

    // Take the hook out unless the game is gone or it is gone already.
    if (end != WatchEnd::TargetExited && end != WatchEnd::HookDetached) {
        link.request_detach();
        const auto deadline = Clock::now() + std::chrono::seconds(8);
        while (Clock::now() < deadline) {
            link.heartbeat();
            link.drain_log();
            if (link.sample().state == proto::HookState::Detached || target.exited()) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }
    link.drain_log();
    return end;
}

}  // namespace rec
