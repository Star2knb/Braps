#include "capture_core.h"

#include <algorithm>
#include <cstring>

#include "hlog.h"

namespace rec::hook {
namespace {

constexpr uint64_t kWarnIntervalSeconds = 1;  // W1201 / W1202 are summarised at most once a second

Core* g_core = nullptr;  // heap, never destroyed by the CRT: GPU objects must not be released during process teardown
std::atomic<bool> g_active{false};
std::atomic<bool> g_busy{false};

// ---- The copy worker -----------------------------------------------------------------------------
// Copies rows out of mapped memory. If the memory becomes invalid (device removed under us) the
// exception is caught here and reported, so the game's process never dies of it.
bool safe_copy(const CopyJob& j) {
    __try {
        if (j.src_pitch == j.row_bytes) {
            std::memcpy(j.dst, j.src, size_t(j.row_bytes) * j.rows);
        } else {
            for (uint32_t r = 0; r < j.rows; ++r) std::memcpy(j.dst + size_t(r) * j.row_bytes, j.src + size_t(r) * j.src_pitch, j.row_bytes);
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

DWORD WINAPI copy_worker(LPVOID param) {
    Core& c = *static_cast<Core*>(param);
    for (;;) {
        WaitForSingleObject(c.worker_wake, INFINITE);
        bool did;
        do {
            did = false;
            for (uint32_t i = 0; i < kMaxStaging; ++i) {
                CopyJob& j = c.jobs[i];
                if (j.state.load(std::memory_order_acquire) != 1) continue;
                const bool ok = safe_copy(j);
                j.state.store(ok ? 2 : 3, std::memory_order_release);
                did = true;
            }
        } while (did);
        if (c.worker_quit.load(std::memory_order_acquire)) break;
    }
    return 0;
}

void log_phases(const Core& c) {
    if (c.ph_submits) log_text(Level::Debug, "per frame us: setup %llu issue %llu", c.ph_setup / c.ph_submits, c.ph_issue / c.ph_submits);
    if (c.ph_reads) log_text(Level::Debug, "per frame us: begin %llu finish %llu", c.ph_begin / c.ph_reads, c.ph_finish / c.ph_reads);
}

void warn_rate_limited(Core& c, uint64_t now) {
    if (!c.warn_backlog && !c.warn_ring) return;
    if (now - c.last_warn_qpc < kWarnIntervalSeconds * uint64_t(g.qpc_freq)) return;
    c.last_warn_qpc = now;
    if (c.warn_backlog) log_event(Ev::GpuBacklog, "%u frames skipped (GPU behind)", c.warn_backlog);
    if (c.warn_ring) log_event(Ev::RingFull, "%u frames dropped (host behind)", c.warn_ring);
    c.warn_backlog = c.warn_ring = 0;
}

void end_recording(Core& c, Backend& b) {
    log_phases(c);
    b.release();
    if (c.pace_timer) CloseHandle(c.pace_timer);
    c.pace_timer = nullptr;
    if (c.ring) UnmapViewOfFile(c.ring);
    if (c.ring_map) CloseHandle(c.ring_map);
    if (c.sem) CloseHandle(c.sem);
    c.ring = nullptr;
    c.ring_map = c.sem = nullptr;
    c.active = c.failed = false;
    c.owner = nullptr;
    g_active.store(false, std::memory_order_release);
    g.ctl->capture_state.store(uint32_t(proto::CaptureState::Off), std::memory_order_release);
}

// ---- Starting a recording ------------------------------------------------------------------------
bool begin_recording(Core& c, Backend& b) {
    proto::ControlBlock* ctl = g.ctl;
    c.active = true;
    c.owner = &b;
    g_active.store(true, std::memory_order_release);
    c.failed = false;
    c.generation = ctl->rec_generation.load(std::memory_order_acquire);
    c.fps = ctl->rec_fps;
    c.out_w = ctl->rec_out_w;
    c.out_h = ctl->rec_out_h;
    c.staging_n = ctl->rec_staging_slots;
    c.ring_slots = ctl->rec_frame_slots;
    c.slot_bytes = ctl->rec_slot_bytes;
    c.t0 = ctl->rec_t0_qpc;
    c.last_tick = -1;
    c.lock = ctl->rec_lock != 0;
    c.anchored = false;
    c.next_k = 0;
    c.ring_next = 0;
    c.seq = 0;
    c.present_index = 0;
    c.ph_setup = c.ph_issue = c.ph_begin = c.ph_finish = 0;
    c.ph_submits = c.ph_reads = 0;
    c.slow_logged = 0;
    c.warn_backlog = c.warn_ring = 0;
    c.last_warn_qpc = qpc();

    const bool sane = c.fps >= 1 && c.fps <= 1000 && c.out_w >= 16 && c.out_h >= 16 && c.out_w <= 8192 && c.out_h <= 5460 &&
                      !(c.out_w & 1) && !(c.out_h & 1) && c.staging_n >= 2 && c.staging_n <= kMaxStaging && c.ring_slots >= 2 &&
                      c.ring_slots <= 256 && c.slot_bytes == b.slot_bytes(c.out_w, c.out_h);
    if (!sane) {
        core_fail(b, long(0x80070057), "recording settings");  // E_INVALIDARG
        return false;
    }
    wchar_t name[64];
    proto::frame_ring_name(name, 64, GetCurrentProcessId(), c.generation, false);
    c.ring_map = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, name);
    if (!c.ring_map) {
        core_fail(b, long(HRESULT_FROM_WIN32(GetLastError())), "open frame ring");
        return false;
    }
    c.ring = static_cast<proto::FrameRingHeader*>(
        MapViewOfFile(c.ring_map, FILE_MAP_ALL_ACCESS, 0, 0, proto::frame_ring_bytes(c.ring_slots, c.slot_bytes)));
    if (!c.ring || c.ring->magic != proto::kFrameRingMagic || c.ring->slot_count != c.ring_slots || c.ring->slot_bytes != c.slot_bytes ||
        c.ring->out_w != c.out_w || c.ring->out_h != c.out_h || c.ring->layout != uint32_t(b.layout())) {
        core_fail(b, c.ring ? long(0x80070057) : long(HRESULT_FROM_WIN32(GetLastError())), "map frame ring");
        return false;
    }
    proto::frame_ring_name(name, 64, GetCurrentProcessId(), c.generation, true);
    c.sem = OpenSemaphoreW(SEMAPHORE_MODIFY_STATE | SYNCHRONIZE, FALSE, name);
    if (!c.sem) {
        core_fail(b, long(HRESULT_FROM_WIN32(GetLastError())), "open frame semaphore");
        return false;
    }
    if (!core_start_worker()) {
        core_fail(b, long(HRESULT_FROM_WIN32(GetLastError())), "start copy worker");
        return false;
    }
    if (c.lock) c.pace_timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);  // null: spin instead
    ctl->capture_state.store(uint32_t(proto::CaptureState::Capturing), std::memory_order_release);
    return true;
}

// Lock mode (§6.2): hold the game until `target` (QPC), with a high-resolution timer for the bulk of the
// wait and a spin for the last stretch. Returns the QPC ticks spent.
uint64_t pace_until(Core& c, uint64_t target) {
    const uint64_t start = qpc();
    const int64_t remaining = int64_t(target) - int64_t(start);
    const int64_t freq = g.qpc_freq;
    if (remaining <= 0 || remaining > freq / 4) return 0;  // already there, or implausibly far (clock jump): don't hold the game
    const int64_t slack = freq / 5000;                     // wake 0.2 ms early and spin the rest
    if (c.pace_timer && remaining > freq / 1000 + slack) {
        LARGE_INTEGER due;
        due.QuadPart = -((remaining - slack) * 10000000ll / freq);  // 100 ns units, relative
        if (SetWaitableTimer(c.pace_timer, &due, 0, nullptr, nullptr, FALSE)) WaitForSingleObject(c.pace_timer, 300);
    }
    while (qpc() < target) YieldProcessor();
    return qpc() - start;
}

struct BusyGuard {
    bool owner;
    BusyGuard() : owner(!g_busy.exchange(true, std::memory_order_acquire)) {}
    ~BusyGuard() {
        if (owner) g_busy.store(false, std::memory_order_release);
    }
};

}  // namespace

Core& core() {
    if (!g_core) g_core = new Core;
    return *g_core;
}

bool core_start_worker() {
    Core& c = core();
    if (c.worker) return true;
    c.worker_quit = false;
    for (CopyJob& j : c.jobs) j.state = 0;
    c.worker_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!c.worker_wake) return false;
    c.worker = CreateThread(nullptr, 0, copy_worker, &c, 0, nullptr);
    return c.worker != nullptr;
}

void core_stop_worker() {
    Core& c = core();
    if (c.worker) {
        c.worker_quit = true;
        SetEvent(c.worker_wake);
        WaitForSingleObject(c.worker, 3000);
        CloseHandle(c.worker);
        c.worker = nullptr;
    }
    if (c.worker_wake) CloseHandle(c.worker_wake);
    c.worker_wake = nullptr;
    for (CopyJob& j : c.jobs) j.state = 0;
}

void core_post_copy(uint32_t index, uint8_t* dst, const uint8_t* src, uint32_t src_pitch, uint32_t row_bytes, uint32_t rows) {
    Core& c = core();
    CopyJob& j = c.jobs[index];
    j.dst = dst;
    j.src = src;
    j.src_pitch = src_pitch;
    j.row_bytes = row_bytes;
    j.rows = rows;
    j.state.store(1, std::memory_order_release);
    SetEvent(c.worker_wake);
}

bool core_mark_failed() {
    Core& c = core();
    proto::ControlBlock* ctl = g.ctl;
    const bool first = !c.failed;
    if (first) {
        c.failed = true;
        ctl->capture_error_code.store(event_number(Ev::CaptureFailed));
        ctl->capture_state.store(uint32_t(proto::CaptureState::Error), std::memory_order_release);
    }
    ctl->capture_errors.fetch_add(1, std::memory_order_relaxed);
    return first;
}

void core_fail(Backend& b, long code, const char* stage) {
    if (core_mark_failed()) {
        log_event(Ev::CaptureFailed, "%s hr=0x%08lX", stage, static_cast<unsigned long>(code));
        b.release();
    }
}

proto::SlotHeader* core_acquire_slot() {
    Core& c = core();
    proto::SlotHeader* slot = proto::ring_slot(c.ring, c.ring_next);
    uint32_t expected = uint32_t(proto::SlotState::Free);
    if (!slot->state.compare_exchange_strong(expected, uint32_t(proto::SlotState::Writing), std::memory_order_acq_rel)) {
        g.ctl->ring_full_drops.fetch_add(1, std::memory_order_relaxed);
        ++c.warn_ring;
        return nullptr;
    }
    return slot;
}

void core_publish(proto::SlotHeader* slot, const FrameInfo& info, proto::Layout layout, uint32_t stride0, uint32_t stride1, uint64_t t_begin) {
    Core& c = core();
    // --debug-drop-readback N: the read-back of every N-th frame "fails". The slot goes back, the frame is counted as
    // skipped (W1201), and the host fills its tick with a DUP.
    const uint32_t drop_every = g.ctl->debug_drop_readback;
    if (drop_every && ++c.debug_reads % drop_every == 0) {
        slot->state.store(uint32_t(proto::SlotState::Free), std::memory_order_release);
        g.ctl->gpu_backlog_skips.fetch_add(1, std::memory_order_relaxed);
        ++c.warn_backlog;
        return;
    }
    slot->seq = ++c.seq;
    slot->tick = info.tick;
    slot->present_qpc = info.present_qpc;
    slot->capture_done_qpc = qpc();
    slot->width = uint16_t(c.out_w);
    slot->height = uint16_t(c.out_h);
    slot->layout = uint8_t(layout);
    slot->flags = 0;
    slot->stride0 = stride0;
    slot->stride1 = stride1;
    slot->game_frame_time_us = info.frame_time_us;
    slot->readback_frames = uint32_t(c.present_index - info.submit_index);
    slot->pacing_wait_us = info.pacing_wait_us;
    slot->pacing_error_us = uint16_t((std::min)(info.pacing_error_us, 65535u));
    const uint32_t finish_us = us_since(t_begin);
    const uint32_t cost_us = info.cost_us + finish_us;
    slot->hook_cost_us = cost_us;
    if (cost_us > 1000 && c.slow_logged < 12) {  // the tail of the cost distribution: where did the time go?
        ++c.slow_logged;
        log_text(Level::Debug, "slow %u us: setup %u issue %u begin %u fin %u", cost_us, info.setup_us, info.issue_us, info.begin_us, finish_us);
    }
    slot->state.store(uint32_t(proto::SlotState::Ready), std::memory_order_release);
    ReleaseSemaphore(c.sem, 1, nullptr);
    c.ring_next = (c.ring_next + 1) % c.ring_slots;
    g.ctl->frames_captured.fetch_add(1, std::memory_order_relaxed);
    c.ph_finish += to_us(qpc() - t_begin);
    ++c.ph_reads;
}

uint64_t core_on_present(Backend& b, void* target, uint64_t now, uint32_t frame_time_us) {
    proto::ControlBlock* ctl = g.ctl;
    const auto want = proto::SessionState(ctl->host_state.load(std::memory_order_acquire));
    if (want == proto::SessionState::Idle && !g_active.load(std::memory_order_acquire)) return 0;  // the common case: one load

    BusyGuard guard;
    if (!guard.owner) return 0;  // another thread is capturing; skip this one
    Core& c = core();

    // The host stopping, or gone (E1401), ends new captures.
    const bool host_gone = proto::HookState(ctl->hook_state.load(std::memory_order_relaxed)) == proto::HookState::Idle;
    const bool recording = want == proto::SessionState::Recording && !host_gone;

    if (c.active && recording && ctl->rec_generation.load(std::memory_order_acquire) != c.generation) end_recording(c, b);  // a new recording
    if (!c.active) {
        if (!recording) return 0;
        begin_recording(c, b);
    }
    ++c.present_index;
    if (c.failed) {
        if (!recording) end_recording(c, b);
        return 0;
    }

    if (recording && c.present_index > 120 && (ctl->debug_flags.load(std::memory_order_relaxed) & proto::kDebugHookThrow)) {
        // --debug-hook-throw: an exception inside the guard of the Present hook. The guard turns it into E1107 and
        // switches the hook off; the game goes on. The exception skips the destructor of `guard`, so release it here.
        ctl->debug_flags.fetch_and(~uint32_t(proto::kDebugHookThrow));
        g_busy.store(false, std::memory_order_release);
        RaiseException(0xE0DEB006u, 0, 0, nullptr);
    }
    b.service();
    uint64_t held = 0;
    if (recording && now >= c.t0) {
        const uint64_t freq = uint64_t(g.qpc_freq);
        auto tick_time = [&](int64_t k) { return (c.lock ? c.grid0 : c.t0) + uint64_t(k) * freq / c.fps; };
        int64_t tick = -1;
        uint32_t error_us = 0;
        if (!c.lock) {
            // Free mode: the first Present at or after each tick; a second Present in the same tick is not captured.
            tick = int64_t(((now - c.t0) * c.fps + freq / 2) / freq);
            if (tick <= c.last_tick) tick = -1;
        } else if (!c.anchored) {
            // Lock mode, first frame: it is tick 0 and the grid starts at this moment, so the grid has the
            // game's phase from the start. From here on the game is held to the grid.
            c.anchored = true;
            c.grid0 = c.grid_ref = now;
            g.ctl->grid0_qpc.store(c.grid0, std::memory_order_release);
            tick = 0;
            c.next_k = 1;
        } else {
            const int64_t k = c.next_k;
            if (now < tick_time(k)) {
                held = pace_until(c, tick_time(k));  // early: wait for the tick
                now = qpc();
            }
            // Decide the tick after the wait: the timer can oversleep, and a hitch can leave the game a tick or more behind.
            tick = k;
            if (now >= tick_time(k + 1)) {
                tick = int64_t((now - c.grid0) * c.fps / freq);  // the ticks in between get DUPs from the host
                if (tick < k) tick = k;
            }
            const uint64_t late = now > tick_time(tick) ? now - tick_time(tick) : 0;
            // Follow the game's phase: a game that is regularly a little late means the grid is ahead of it. Move
            // the grid halfway toward the frame, so the game soon arrives just ahead of it (and is held for the
            // difference). Only later, never earlier; and never more than one tick from where the grid started, so
            // the video stays tied to the wall clock (a game slower than the grid gets DUPs, not a slowed-down timeline).
            const uint64_t room = c.grid_ref + freq / c.fps - c.grid0;
            const uint64_t shift = (std::min)(late / 2, room);
            if (shift) {
                c.grid0 += shift;
                g.ctl->grid0_qpc.store(c.grid0, std::memory_order_release);
            }
            error_us = uint32_t(late * 1000000ull / freq);
            c.next_k = tick + 1;
        }
        if (tick >= 0) {
            FrameInfo info;
            info.present_qpc = now;
            info.tick = uint64_t(tick);
            info.frame_time_us = frame_time_us;
            info.pacing_wait_us = uint32_t(held * 1000000ull / freq);
            info.pacing_error_us = error_us;
            info.submit_index = c.present_index;
            const int64_t before = c.last_tick;
            b.submit(info, target);
            if (before != c.last_tick) g.ctl->last_capture_tick.store(uint64_t(tick), std::memory_order_relaxed);
        }
    }
    if (!recording) {
        ctl->capture_state.store(uint32_t(proto::CaptureState::Draining), std::memory_order_release);
        if (b.pending() == 0) end_recording(c, b);
    }
    warn_rate_limited(c, now);
    return held;
}

void core_shutdown(Backend& b) {
    if (!g_core) return;
    if (g_core->active) {
        if (g_core->owner == &b) end_recording(*g_core, b);
    } else {
        b.release();
    }
}

}  // namespace rec::hook
