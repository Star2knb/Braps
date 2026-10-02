#include "measure.h"

#include "capture_d3d11.h"
#include "hlog.h"
#include "hook_state.h"

namespace rec::hook {

uint32_t note_present(uint64_t now) {
    proto::ControlBlock* c = g.ctl;
    const uint64_t previous = c->last_present_qpc.exchange(now, std::memory_order_relaxed);
    uint32_t frame_time_us = 0;
    if (previous) {
        frame_time_us = uint32_t((now - previous) * 1000000ull / uint64_t(g.qpc_freq));
        c->frame_time_us.store(frame_time_us, std::memory_order_relaxed);
    }
    c->present_count.fetch_add(1, std::memory_order_relaxed);
    return frame_time_us;
}

void note_cost(uint64_t t0, uint64_t held) {
    const uint64_t spent = qpc() - t0;
    const uint32_t cost_ns = uint32_t((spent > held ? spent - held : 0) * 1000000000ull / uint64_t(g.qpc_freq));
    proto::ControlBlock* c = g.ctl;
    c->hook_cost_ns.store(cost_ns, std::memory_order_relaxed);
    c->hook_cost_total_ns.fetch_add(cost_ns, std::memory_order_relaxed);
    uint32_t seen = c->hook_cost_max_ns.load(std::memory_order_relaxed);
    while (cost_ns > seen && !c->hook_cost_max_ns.compare_exchange_weak(seen, cost_ns, std::memory_order_relaxed)) {
    }
}

bool other_api_active(bool mine_is_gl, uint64_t now) {
    const uint32_t backend = g.ctl->backend.load(std::memory_order_acquire);
    if (!backend) return false;
    const bool owner_is_gl = backend == proto::kApiOpenGL;
    if (owner_is_gl == mine_is_gl) return false;
    return now - g.ctl->last_present_qpc.load(std::memory_order_relaxed) <= uint64_t(g.qpc_freq);
}

void note_exception(DWORD code) {
    g.enabled.store(false, std::memory_order_release);
    capture_on_exception();
    g.ctl->error_code.store(event_number(Ev::HookException), std::memory_order_relaxed);
    log_event(Ev::HookException, "exception 0x%08lX in Present", code);
}

uint32_t window_refresh_hz(HWND window) {
    HMONITOR monitor = MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (!monitor || !GetMonitorInfoW(monitor, &info)) return 0;
    DEVMODEW mode{};
    mode.dmSize = sizeof(mode);
    if (!EnumDisplaySettingsW(info.szDevice, ENUM_CURRENT_SETTINGS, &mode)) return 0;
    return mode.dmDisplayFrequency > 1 ? mode.dmDisplayFrequency : 0;
}

}  // namespace rec::hook
