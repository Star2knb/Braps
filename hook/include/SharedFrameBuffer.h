#pragma once

#include <cstdint>
#include <atomic>

// Cross-process frame transport between BrapsHook.dll (injected into the
// target game) and Braps.exe (the recorder/consumer process). Both sides
// map the same named CreateFileMapping section and must agree exactly on
// this layout — it's raw shared memory, not a serialized message format.
//
// This mirrors the in-process FrameRingBuffer (RingBuffer.h) in spirit —
// same "producer never blocks, drop on full" policy — but every field here
// has to be a POD/atomic type usable in shared memory across process
// boundaries, so it can't reuse std::vector/std::mutex directly.

namespace braps {

// Bumped whenever the layout changes. Both sides check this on connect so
// a stale/mismatched DLL and exe pairing fails loudly instead of reading
// garbage frame data.
constexpr uint32_t kSharedMemoryProtocolVersion = 2;

constexpr wchar_t kSharedMemoryName[] = L"Local\\BrapsSharedFrameBuffer";
constexpr wchar_t kNewFrameEventName[] = L"Local\\BrapsNewFrameEvent";
constexpr wchar_t kHookReadyEventName[] = L"Local\\BrapsHookReadyEvent";

// Fixed slot count and max resolution the shared region is sized for. A
// fixed layout (rather than a dynamically sized one) keeps both sides'
// struct definitions identical without needing to renegotiate sizes over
// IPC — the tradeoff is capping supported resolution and buffer depth.
// Capped at 1080p: covers the large majority of games, and keeps the
// shared region a modest ~65MB (1080p @ 4 slots) rather than hundreds of
// MB committed just in case of 4K. Bump this later if needed.
constexpr int kMaxWidth = 1920;
constexpr int kMaxHeight = 1080;
constexpr size_t kMaxFrameBytes = static_cast<size_t>(kMaxWidth) * kMaxHeight * 4; // BGRA
constexpr int kSlotCount = 4; // shallow: this is a hot path buffer, not a recording buffer

#pragma pack(push, 1)
struct FrameSlot {
    uint32_t width = 0;
    uint32_t height = 0;
    uint64_t timestampMs = 0;
    uint32_t byteCount = 0;
    // Frame pixel data follows in-place; sized to the worst case so the
    // struct's memory layout is fixed regardless of actual capture
    // resolution, avoiding cross-process pointer/offset math entirely.
    uint8_t data[kMaxFrameBytes];
};

struct SharedHeader {
    uint32_t protocolVersion = kSharedMemoryProtocolVersion;
    std::atomic<uint32_t> head{0}; // next slot the hook (producer) will write
    std::atomic<uint32_t> tail{0}; // next slot Braps.exe (consumer) will read
    std::atomic<uint32_t> hookProcessId{0};
    std::atomic<bool> hookAttached{false};

    // Set by Braps.exe (the consumer) to reflect Recorder::isRecording_,
    // checked by the Present hook (the producer) BEFORE doing any GPU
    // readback work (CopyResource/Map/row-copy). Without this, the hook
    // was paying full readback cost on every single Present call from the
    // moment it was injected — a real, measured FPS cost to the game even
    // while Braps was just sitting idle, not recording anything at all.
    std::atomic<bool> consumerWantsFrames{false};
};

struct SharedMemoryLayout {
    SharedHeader header;
    FrameSlot slots[kSlotCount];
};
#pragma pack(pop)

constexpr size_t kSharedMemoryTotalSize = sizeof(SharedMemoryLayout);

} // namespace braps
