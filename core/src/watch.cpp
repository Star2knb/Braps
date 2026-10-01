#include "rec/watch.h"

#include <windows.h>

#include <chrono>
#include <cstdio>
#include <thread>

#include "rec/anticheat.h"
#include "rec/log.h"
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

void sleep_until(Clock::time_point t) { std::this_thread::sleep_until(t); }

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

std::string format_status_line(const std::string& exe_name, const HookStats& s) {
    char buf[256];
    if (s.state == proto::HookState::Waiting) {
        std::snprintf(buf, sizeof(buf), "○ IDLE %s | hook installs when the game loads Direct3D 11", exe_name.c_str());
    } else if (!s.backend) {
        std::snprintf(buf, sizeof(buf), "○ IDLE %s | hooked, waiting for the first Present", exe_name.c_str());
    } else if (s.state == proto::HookState::Idle) {
        std::snprintf(buf, sizeof(buf), "○ IDLE %s %s | hook idle: host lost", exe_name.c_str(), backend_name(s.backend));
    } else {
        char fps[16] = "--";
        if (s.fps > 0) std::snprintf(fps, sizeof(fps), "%.1f", s.fps);
        // The idle hook costs about a microsecond: show that, and switch to ms when it is large.
        char cost[64];
        if (s.hook_cost_max_us < 100.0)
            std::snprintf(cost, sizeof(cost), "%.1f us (max %.1f)", s.hook_cost_us, s.hook_cost_max_us);
        else
            std::snprintf(cost, sizeof(cost), "%.2f ms (max %.2f)", s.hook_cost_us / 1000.0, s.hook_cost_max_us / 1000.0);
        std::snprintf(buf, sizeof(buf), "○ IDLE %s %s %ux%u | game %s fps | hook %s", exe_name.c_str(),
                      backend_name(s.backend), s.width, s.height, fps, cost);
    }
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
    const auto start = Clock::now();
    auto next_tick = start;
    auto next_plain_line = start;
    bool rescanned = false;
    WatchEnd end = WatchEnd::UserStop;

    for (;;) {
        next_tick += kTick;
        link.heartbeat();
        link.drain_log();
        const HookStats stats = link.sample();

        if (target.exited()) {
            logging::event(Ev::TargetExited, "{} (pid {})", target.name(), target.pid());
            end = WatchEnd::TargetExited;
            break;
        }
        if (stats.state == proto::HookState::Failed) {
            end = WatchEnd::HookFailed;
            break;
        }
        if (stats.state == proto::HookState::Detached || stats.state == proto::HookState::Detaching) {
            end = WatchEnd::HookDetached;
            break;
        }
        if (stop.load()) {
            end = WatchEnd::UserStop;
            break;
        }
        if (options.duration_s && Clock::now() - start >= std::chrono::seconds(options.duration_s)) {
            end = WatchEnd::Duration;
            break;
        }
        if (!rescanned && !options.force && Clock::now() - start >= kAnticheatRescan) {
            rescanned = true;
            const auto found = scan_anticheat_modules(options.blocklist, target.pid());
            if (!found.empty()) {
                logging::event(Ev::AnticheatBlocked, "{} appeared in {}; removing the hook", found.front().name, target.name());
                std::printf("\n%s loaded an anti-cheat module (%s). Removing the hook to protect your account.\n",
                            target.name().c_str(), found.front().name.c_str());
                end = WatchEnd::AnticheatFound;
                break;
            }
        }

        const std::string line = format_status_line(target.name(), stats);
        if (console) {
            std::printf("\r%-110s", line.c_str());
            std::fflush(stdout);
        } else if (Clock::now() >= next_plain_line) {
            std::printf("%s\n", line.c_str());
            std::fflush(stdout);
            next_plain_line += std::chrono::seconds(1);
        }
        sleep_until(next_tick);
    }
    if (console) std::printf("\r%-110s\r", "");

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
