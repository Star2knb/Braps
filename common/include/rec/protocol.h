// Shared memory between the host (rec.exe, x64) and the hook DLL (x86 or x64), recorder plan §7.
// Fixed-width fields only, so the layout is identical for both bitnesses (checked below).
// Objects, named per game PID in the session namespace:
//   Local\rec_<pid>_ctl   ControlBlock
//   Local\rec_<pid>_log   LogRing (hook -> host log records, §10.2)
//   Local\rec_<pid>_cmd   auto-reset event, host -> hook: a command is waiting
// The host creates them before injecting; the hook only opens them.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4324)  // padded due to alignas: intended, keeps producer/consumer counters on separate cache lines
#endif

namespace rec::proto {

constexpr uint32_t kMagic = 0x31434552;  // "REC1"
constexpr uint32_t kVersion = 2;  // 2: recording fields in the control block, frame ring

// Hook lifecycle, written by the hook.
enum class HookState : uint32_t {
    None = 0,      // shared memory created, DLL not loaded yet
    Loading,       // DLL loaded, install thread starting
    Waiting,       // graphics DLL not loaded yet; install deferred (I1103)
    Hooked,        // hooks installed
    Failed,        // nothing could be hooked (see error_code)
    Idle,          // host lost (E1401): hooks stay, capture off
    Detaching,
    Detached,      // hooks removed; the DLL unloads next
};

// ControlBlock::error_code values that are not event numbers.
constexpr uint32_t kErrProtocolMismatch = 1;  // the block's magic, version or size is not ours

// Host -> hook commands (with the cmd event).
enum class Command : uint32_t { None = 0, Detach = 1 };

// Graphics APIs, as bits (list, detection, backend in use).
enum Api : uint32_t {
    kApiD3D9 = 1u << 0,
    kApiD3D10 = 1u << 1,
    kApiD3D11 = 1u << 2,
    kApiD3D12 = 1u << 3,
    kApiOpenGL = 1u << 4,
    kApiVulkan = 1u << 5,
    kApiDXGI = 1u << 6,
};

struct alignas(64) ControlBlock {
    // Written once by the host.
    uint32_t magic;
    uint32_t version;
    uint32_t size;  // sizeof(ControlBlock)
    uint32_t game_pid;
    int64_t qpc_frequency;
    // Host <-> hook.
    std::atomic<uint32_t> host_pid;     // host attached to this block (a new host may take over)
    std::atomic<uint32_t> command;      // Command
    std::atomic<uint64_t> host_heartbeat_qpc;
    std::atomic<uint64_t> hook_heartbeat_qpc;
    std::atomic<uint32_t> hook_state;   // HookState
    std::atomic<uint32_t> hooked_apis;  // Api bits whose Present is hooked
    std::atomic<uint32_t> backend;      // Api bit of the first Present that fired (0 = none yet)
    std::atomic<uint32_t> error_code;   // last fatal hook error (an event number), 0 = none
    // Present statistics (M1: measuring only). Written by the Present hook.
    std::atomic<uint64_t> present_count;
    std::atomic<uint64_t> last_present_qpc;
    std::atomic<uint32_t> frame_time_us;   // last frame interval
    std::atomic<uint32_t> hook_cost_ns;    // our own time in the last Present (excluding the original)
    std::atomic<uint32_t> hook_cost_max_ns;
    std::atomic<uint32_t> attach_count;    // times a host attached to this DLL instance
    // The swap chain being measured (the first one to present), set when `backend` is.
    std::atomic<uint32_t> backbuffer_width;
    std::atomic<uint32_t> backbuffer_height;
    std::atomic<uint32_t> backbuffer_format;  // DXGI_FORMAT
    std::atomic<uint32_t> present_ignored;    // Present calls from other swap chains (overlays)
    std::atomic<uint64_t> hook_cost_total_ns;  // sum of hook_cost over present_count calls (host takes averages)

    // ---- Recording (M2). The host fills the configuration, then publishes it with host_state. ----
    std::atomic<uint32_t> host_state;          // SessionState: what the hook should be doing
    std::atomic<uint32_t> capture_state;       // CaptureState: what it is doing
    std::atomic<uint32_t> display_refresh_hz;  // of the monitor the game window is on (I1301), 0 = unknown
    std::atomic<uint32_t> rec_generation;      // names the frame ring of this recording (host increments)
    uint32_t rec_fps;                          // tick rate: ticks of 1/rec_fps s from rec_t0_qpc
    uint32_t rec_out_w, rec_out_h;             // output size (even); the back buffer is scaled and letterboxed into it
    uint32_t rec_staging_slots;                // GPU read-back slots (2-5)
    uint32_t rec_frame_slots;                  // slots in the shared frame ring
    uint32_t rec_slot_bytes;                   // bytes per slot, header included
    uint64_t rec_t0_qpc;                       // session start (§6.1)
    std::atomic<uint64_t> frames_captured;     // frames delivered into the ring
    std::atomic<uint64_t> ring_full_drops;     // W1202: no free ring slot
    std::atomic<uint64_t> gpu_backlog_skips;   // W1201: every staging slot still pending
    std::atomic<uint32_t> capture_errors;      // capture failures (the first one disables capture)
    std::atomic<uint32_t> capture_error_code;  // event number of the first, 0 = none
    std::atomic<uint64_t> last_capture_tick;   // tick of the newest captured frame
    uint32_t rec_lock;                         // 1: hold the game to the tick grid (lock mode, §6.2), 0: capture what it presents
    uint32_t reserved0;
    std::atomic<uint64_t> grid0_qpc;           // lock mode: QPC time of tick 0 as the hook has the grid now (it follows the game's phase)
    uint32_t reserved[72];
};

// Host -> hook: what to do with Present.
enum class SessionState : uint32_t {
    Idle = 0,       // measure only
    Recording = 1,  // capture one frame per tick
    Stopping = 2,   // stop capturing new frames, finish the read-backs in flight
};

// Hook -> host: what the hook is doing.
enum class CaptureState : uint32_t {
    Off = 0,
    Capturing = 1,
    Draining = 2,  // stopping; waiting for in-flight read-backs
    Error = 3,     // capture disabled (capture_error_code says why); measuring goes on
};

constexpr uint32_t kLogMagic = 0x474C4552;  // "RELG"
constexpr uint32_t kLogCapacity = 4096;     // records, power of two

// One log record (§10.2): 64 bytes.
struct LogRecord {
    uint64_t qpc;
    uint8_t level;   // rec::Level
    uint8_t flags;   // kLogText: arg[] + tag hold 48 bytes of text instead of a code's data
    uint16_t code;   // event number (with level, identifies the event; 0 for plain text)
    uint32_t thread_id;
    uint64_t arg[4];
    char tag[16];    // NUL-padded, not necessarily terminated
};
constexpr uint8_t kLogText = 1;
constexpr size_t kLogTextMax = sizeof(LogRecord::arg) + sizeof(LogRecord::tag);  // 48

// Lock-free ring, many producers (hook threads), one consumer (host logger): a bounded queue in the
// style of D. Vyukov, with a sequence number per cell. Producers never wait; when the ring is full
// the record is dropped and counted (W2402).
struct alignas(64) LogRing {
    uint32_t magic;
    uint32_t capacity;
    uint32_t reserved0[14];
    alignas(64) std::atomic<uint64_t> head;  // next position to claim (producers)
    alignas(64) std::atomic<uint64_t> tail;  // next position to read (consumer)
    alignas(64) std::atomic<uint64_t> dropped;
    alignas(64) std::atomic<uint64_t> seq[kLogCapacity];
    LogRecord rec[kLogCapacity];

    void init() {
        magic = kLogMagic;
        capacity = kLogCapacity;
        head.store(0);
        tail.store(0);
        dropped.store(0);
        for (uint32_t i = 0; i < kLogCapacity; ++i) seq[i].store(i, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
    }

    // Producer. Never blocks; false (and counted) if the ring is full.
    bool push(const LogRecord& r) {
        uint64_t pos = head.load(std::memory_order_relaxed);
        for (;;) {
            std::atomic<uint64_t>& s = seq[pos & (kLogCapacity - 1)];
            const uint64_t q = s.load(std::memory_order_acquire);
            const int64_t diff = int64_t(q) - int64_t(pos);
            if (diff == 0) {
                if (head.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
                    rec[pos & (kLogCapacity - 1)] = r;
                    s.store(pos + 1, std::memory_order_release);
                    return true;
                }
            } else if (diff < 0) {
                dropped.fetch_add(1, std::memory_order_relaxed);
                return false;
            } else {
                pos = head.load(std::memory_order_relaxed);
            }
        }
    }

    // Single consumer. False if nothing is ready.
    bool pop(LogRecord* out) {
        const uint64_t pos = tail.load(std::memory_order_relaxed);
        std::atomic<uint64_t>& s = seq[pos & (kLogCapacity - 1)];
        if (s.load(std::memory_order_acquire) != pos + 1) return false;
        *out = rec[pos & (kLogCapacity - 1)];
        s.store(pos + kLogCapacity, std::memory_order_release);
        tail.store(pos + 1, std::memory_order_relaxed);
        return true;
    }
};

// ---- Frame ring (§7): the host creates one per recording, the hook fills it. ----------------------
// Objects: Local\rec_<pid>_f<generation> (the mapping) and ..._f<generation>s (a semaphore the hook
// releases once per READY slot). The generation is ControlBlock::rec_generation, so a new recording
// never meets the names of the previous one.
constexpr uint32_t kFrameRingMagic = 0x31524652;  // "RFR1"

enum class SlotState : uint32_t { Free = 0, Writing, Ready, Reading };
enum class Layout : uint8_t { Nv12 = 0, Bgra = 1 };

// 64 bytes at the start of every slot, pixel data follows.
struct alignas(64) SlotHeader {
    std::atomic<uint32_t> state;  // SlotState
    uint32_t seq;                 // 1, 2, 3 ... in ring order
    uint64_t tick;                // round((present_qpc - t0) / T)
    uint64_t present_qpc;         // when the game called Present
    uint64_t capture_done_qpc;    // when the frame was in this slot
    uint16_t width, height;
    uint8_t layout;               // Layout
    uint8_t flags;
    uint16_t pacing_error_us;     // how late (after its tick) the capture began, saturating
    uint32_t stride0, stride1;    // bytes per row of the first (Y) and second (UV) plane
    uint32_t game_frame_time_us;  // interval to the previous Present
    uint32_t hook_cost_us;        // our time in the Presents that submitted and read back this frame
    uint32_t readback_frames;     // Presents between submitting the copy and reading it back
    uint32_t pacing_wait_us;      // time the game was held to reach this frame's tick (lock mode)
};

struct alignas(64) FrameRingHeader {
    uint32_t magic;
    uint32_t version;
    uint32_t slot_count;
    uint32_t slot_bytes;  // header included
    uint32_t out_w, out_h;
    uint32_t layout;      // Layout
    uint32_t reserved[9];
};

constexpr uint32_t align_up(uint32_t v, uint32_t a) { return (v + a - 1) / a * a; }

// NV12 slot: header, then the Y plane (w x h) and straight after it the UV plane (interleaved Cb Cr,
// w bytes per row, h/2 rows): one contiguous w x 3h/2 block, which is also how the GPU produces it.
constexpr uint32_t nv12_y_offset() { return sizeof(SlotHeader); }
constexpr uint32_t nv12_uv_offset(uint32_t w, uint32_t h) { return nv12_y_offset() + w * h; }
constexpr uint32_t nv12_slot_bytes(uint32_t w, uint32_t h) { return align_up(nv12_uv_offset(w, h) + w * (h / 2), 4096); }

inline SlotHeader* ring_slot(FrameRingHeader* ring, uint32_t index) {
    return reinterpret_cast<SlotHeader*>(reinterpret_cast<uint8_t*>(ring) + sizeof(FrameRingHeader) + size_t(index) * ring->slot_bytes);
}
inline size_t frame_ring_bytes(uint32_t slot_count, uint32_t slot_bytes) {
    return sizeof(FrameRingHeader) + size_t(slot_count) * slot_bytes;
}

// Same layout for x86 and x64 (the hook may be 32-bit, the host is 64-bit).
static_assert(std::atomic<uint64_t>::is_always_lock_free && std::atomic<uint32_t>::is_always_lock_free);
static_assert(sizeof(std::atomic<uint64_t>) == 8 && alignof(std::atomic<uint64_t>) == 8);
static_assert(sizeof(LogRecord) == 64);
static_assert(sizeof(SlotHeader) == 64 && sizeof(FrameRingHeader) == 64);
static_assert(offsetof(SlotHeader, tick) == 8 && offsetof(SlotHeader, width) == 32 && offsetof(SlotHeader, stride0) == 40);
static_assert(sizeof(ControlBlock) == 512);
static_assert(offsetof(ControlBlock, host_state) == 120 && offsetof(ControlBlock, rec_t0_qpc) == 160);
static_assert(offsetof(ControlBlock, frames_captured) == 168 && offsetof(ControlBlock, last_capture_tick) == 200);
static_assert(offsetof(ControlBlock, rec_lock) == 208 && offsetof(ControlBlock, grid0_qpc) == 216);
static_assert(offsetof(ControlBlock, qpc_frequency) == 16);
static_assert(offsetof(ControlBlock, host_heartbeat_qpc) == 32);
static_assert(offsetof(ControlBlock, present_count) == 64);
static_assert(offsetof(ControlBlock, backbuffer_width) == 96);
static_assert(offsetof(ControlBlock, hook_cost_total_ns) == 112);
static_assert(offsetof(LogRing, head) == 64 && offsetof(LogRing, seq) == 256);
static_assert(sizeof(LogRing) == 256 + kLogCapacity * 8 + kLogCapacity * 64);

// "Local\rec_<pid>_<suffix>" into out (at least 64 wide characters).
inline void object_name(wchar_t* out, size_t out_len, uint32_t pid, const wchar_t* suffix) {
    wchar_t digits[16];
    int n = 0;
    do {
        digits[n++] = wchar_t(L'0' + pid % 10);
        pid /= 10;
    } while (pid && n < 15);
    const wchar_t prefix[] = L"Local\\rec_";
    size_t k = 0;
    for (size_t i = 0; prefix[i] && k + 1 < out_len; ++i) out[k++] = prefix[i];
    while (n > 0 && k + 1 < out_len) out[k++] = digits[--n];
    if (k + 1 < out_len) out[k++] = L'_';
    for (size_t i = 0; suffix[i] && k + 1 < out_len; ++i) out[k++] = suffix[i];
    out[k] = 0;
}

// "Local\rec_<pid>_f<generation>" (the frame ring) or "..._f<generation>s" (its semaphore).
inline void frame_ring_name(wchar_t* out, size_t out_len, uint32_t pid, uint32_t generation, bool semaphore) {
    wchar_t suffix[24];
    size_t k = 0;
    suffix[k++] = L'f';
    wchar_t digits[12];
    int n = 0;
    do {
        digits[n++] = wchar_t(L'0' + generation % 10);
        generation /= 10;
    } while (generation && n < 11);
    while (n > 0) suffix[k++] = digits[--n];
    if (semaphore) suffix[k++] = L's';
    suffix[k] = 0;
    object_name(out, out_len, pid, suffix);
}

inline void set_tag(LogRecord* r, const char* tag) {
    std::memset(r->tag, 0, sizeof(r->tag));
    for (size_t i = 0; tag && tag[i] && i < sizeof(r->tag); ++i) r->tag[i] = tag[i];
}

}  // namespace rec::proto

#ifdef _MSC_VER
#pragma warning(pop)
#endif
