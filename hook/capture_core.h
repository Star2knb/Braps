// The part of frame capture that does not depend on the graphics API (recorder plan §5, §6.2, §7):
// the recording settings from the control block, the shared frame ring (taking a slot, publishing it),
// the lock-mode pacer that decides each frame's tick, and the worker thread that copies mapped GPU
// memory into the ring. A backend (Direct3D 11, OpenGL) supplies only the GPU work through `Backend`.
#pragma once

#include <windows.h>

#include <atomic>
#include <cstdint>

#include "hook_state.h"
#include "rec/protocol.h"

namespace rec::hook {

constexpr uint32_t kMaxStaging = 8;

inline uint64_t to_us(uint64_t ticks) { return ticks * 1000000ull / uint64_t(g.qpc_freq); }
inline uint32_t us_since(uint64_t t0) { return uint32_t(to_us(qpc() - t0)); }

// What the host is told about one captured frame.
struct FrameInfo {
    uint64_t present_qpc = 0;
    uint64_t tick = 0;
    uint64_t submit_index = 0;  // Core::present_index when the copy was issued
    uint32_t frame_time_us = 0;
    uint32_t pacing_wait_us = 0, pacing_error_us = 0;  // lock mode: how long the game was held, how late the capture began
    uint32_t cost_us = 0;                              // hook time spent on this frame so far (the copy worker's time is not ours)
    uint32_t setup_us = 0, issue_us = 0, begin_us = 0;  // the parts of it, for the log of slow frames
};

// A copy for the worker: rows out of mapped GPU memory into a ring slot.
struct CopyJob {
    std::atomic<uint32_t> state{0};  // 0 idle, 1 requested, 2 done, 3 faulted
    uint8_t* dst = nullptr;
    const uint8_t* src = nullptr;
    uint32_t src_pitch = 0, row_bytes = 0, rows = 0;
};

class Backend;

struct Core {
    // Recording (from the control block).
    Backend* owner = nullptr;  // the backend this recording runs on
    bool active = false;
    bool failed = false;
    uint32_t generation = 0, fps = 0, out_w = 0, out_h = 0, staging_n = 0, ring_slots = 0, slot_bytes = 0;
    uint64_t t0 = 0;
    int64_t last_tick = -1;
    bool lock = false;            // lock mode: hold the game to the tick grid
    bool anchored = false;        // the first tick has been chosen
    int64_t next_k = 0;           // lock mode: the next tick to capture
    uint64_t grid0 = 0;           // lock mode: QPC time of tick 0; follows the game's phase (see core_on_present)
    uint64_t grid_ref = 0;        // ... where it started: grid0 never moves more than one tick later than this
    HANDLE pace_timer = nullptr;  // high-resolution timer for the wait
    HANDLE ring_map = nullptr;
    HANDLE sem = nullptr;
    proto::FrameRingHeader* ring = nullptr;
    uint32_t ring_next = 0;
    uint32_t seq = 0;
    uint64_t present_index = 0;

    // Copy worker.
    HANDLE worker = nullptr;
    HANDLE worker_wake = nullptr;
    std::atomic<bool> worker_quit{false};
    CopyJob jobs[kMaxStaging];

    // Where the render thread's time goes (microseconds summed over frames), logged at DEBUG.
    uint64_t ph_setup = 0, ph_issue = 0, ph_begin = 0, ph_finish = 0;
    uint32_t ph_submits = 0, ph_reads = 0;
    uint32_t slow_logged = 0;
    uint32_t debug_reads = 0;  // read-backs finished so far (--debug-drop-readback)

    // Rate-limited warnings.
    uint64_t last_warn_qpc = 0;
    uint32_t warn_backlog = 0, warn_ring = 0;
};

// What a graphics API provides. All calls come from the game's render thread, inside the Present hook.
class Backend {
public:
    virtual ~Backend() = default;
    virtual proto::Layout layout() const = 0;
    virtual uint32_t slot_bytes(uint32_t w, uint32_t h) const = 0;  // the ring's slot size for this output size
    virtual void service() = 0;                                     // move finished read-backs along; never waits
    // Issue the capture of the frame being presented; `target` is what the Present hook was called with (the
    // swap chain, the device context's window DC).
    virtual void submit(const FrameInfo& info, void* target) = 0;
    virtual uint32_t pending() const = 0;                           // read-backs not yet delivered
    virtual void release() = 0;                                     // drop every GPU object (unmapping what is mapped)
};

Core& core();

// The worker that copies mapped memory into the ring. The backend starts it (again after a new device) and
// stops it in release().
bool core_start_worker();
void core_stop_worker();
void core_post_copy(uint32_t index, uint8_t* dst, const uint8_t* src, uint32_t src_pitch, uint32_t row_bytes, uint32_t rows);
inline uint32_t core_job_state(uint32_t index) { return core().jobs[index].state.load(std::memory_order_acquire); }
inline void core_job_clear(uint32_t index) { core().jobs[index].state.store(0, std::memory_order_relaxed); }

// Failure. core_mark_failed returns true the first time (the caller then logs the cause and calls
// release()); it counts every call as a capture error.
bool core_mark_failed();
// Logs E1211 with `stage` and the code, marks capture failed and releases the backend. (Logs the first time only.)
void core_fail(Backend& b, long code, const char* stage);

// The shared ring: a free slot to write into (nullptr, counted, if the host is behind) and publishing a
// finished one. `t_begin` is when the backend started its final step, which is part of the frame's cost.
proto::SlotHeader* core_acquire_slot();
void core_publish(proto::SlotHeader* slot, const FrameInfo& info, proto::Layout layout, uint32_t stride0, uint32_t stride1, uint64_t t_begin);

// Called by the Present hook of the API in use: does everything a Present needs. Returns how long (QPC
// ticks) it deliberately held the game to the tick grid, which is not counted as the hook's cost.
// Does nothing (one atomic load) unless the host asked for a recording.
uint64_t core_on_present(Backend& b, void* target, uint64_t now, uint32_t frame_time_us);

// Detach: closes the frame ring and drops the GPU objects.
void core_shutdown(Backend& b);

}  // namespace rec::hook
