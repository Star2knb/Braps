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
constexpr uint32_t kVersion = 1;

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
    uint32_t reserved[40];
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

// Same layout for x86 and x64 (the hook may be 32-bit, the host is 64-bit).
static_assert(std::atomic<uint64_t>::is_always_lock_free && std::atomic<uint32_t>::is_always_lock_free);
static_assert(sizeof(std::atomic<uint64_t>) == 8 && alignof(std::atomic<uint64_t>) == 8);
static_assert(sizeof(LogRecord) == 64);
static_assert(sizeof(ControlBlock) == 256);
static_assert(offsetof(ControlBlock, qpc_frequency) == 16);
static_assert(offsetof(ControlBlock, host_heartbeat_qpc) == 32);
static_assert(offsetof(ControlBlock, present_count) == 64);
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

inline void set_tag(LogRecord* r, const char* tag) {
    std::memset(r->tag, 0, sizeof(r->tag));
    for (size_t i = 0; tag && tag[i] && i < sizeof(r->tag); ++i) r->tag[i] = tag[i];
}

}  // namespace rec::proto

#ifdef _MSC_VER
#pragma warning(pop)
#endif
