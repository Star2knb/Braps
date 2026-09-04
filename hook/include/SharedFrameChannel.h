#pragma once

#include "SharedFrameBuffer.h"

#include <windows.h>
#include <cstring>

// Thin wrapper around the shared memory section + named event described in
// SharedFrameBuffer.h. Both BrapsHook.dll (Open as producer) and Braps.exe
// (Open as consumer) use the same class; which side calls TryPush vs
// TryPop is a usage convention, not something this type enforces.
namespace braps {

class SharedFrameChannel {
public:
    ~SharedFrameChannel() { Close(); }

    // Producer (hook DLL) creates the section; consumer (Braps.exe) opens
    // an existing one. Both must succeed for the channel to be usable —
    // if the consumer opens before any producer has created it, this fails
    // and the caller should retry rather than proceeding with a null map.
    bool CreateAsProducer() {
        mapping_ = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                       static_cast<DWORD>(kSharedMemoryTotalSize), kSharedMemoryName);
        if (!mapping_) return false;
        return MapAndInit(true);
    }

    bool OpenAsConsumer() {
        mapping_ = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, kSharedMemoryName);
        if (!mapping_) return false;
        return MapAndInit(false);
    }

    void Close() {
        if (view_) { UnmapViewOfFile(view_); view_ = nullptr; }
        if (mapping_) { CloseHandle(mapping_); mapping_ = nullptr; }
        if (newFrameEvent_) { CloseHandle(newFrameEvent_); newFrameEvent_ = nullptr; }
    }

    bool IsValid() const { return view_ != nullptr; }

    SharedHeader& Header() { return view_->header; }

    // Producer side: copies frame data into the next slot and advances
    // head. Returns false (drops the frame) if the buffer is full — same
    // policy as the in-process ring buffer: never block the capture path.
    bool TryPush(const uint8_t* data, size_t byteCount, uint32_t width, uint32_t height, uint64_t timestampMs) {
        if (!view_ || byteCount > kMaxFrameBytes) return false;

        uint32_t head = view_->header.head.load(std::memory_order_relaxed);
        uint32_t nextHead = (head + 1) % kSlotCount;
        if (nextHead == view_->header.tail.load(std::memory_order_acquire)) {
            return false; // full, drop this frame
        }

        FrameSlot& slot = view_->slots[head];
        slot.width = width;
        slot.height = height;
        slot.timestampMs = timestampMs;
        slot.byteCount = static_cast<uint32_t>(byteCount);
        std::memcpy(slot.data, data, byteCount);

        view_->header.head.store(nextHead, std::memory_order_release);
        if (newFrameEvent_) SetEvent(newFrameEvent_);
        return true;
    }

    // Consumer side: copies the oldest unread slot's data into the
    // caller-provided buffer (must be at least kMaxFrameBytes) and
    // advances tail. Returns false if the buffer is currently empty.
    bool TryPop(uint8_t* outData, uint32_t& outWidth, uint32_t& outHeight, uint64_t& outTimestampMs, uint32_t& outByteCount) {
        if (!view_) return false;

        uint32_t tail = view_->header.tail.load(std::memory_order_relaxed);
        if (tail == view_->header.head.load(std::memory_order_acquire)) {
            return false; // empty
        }

        FrameSlot& slot = view_->slots[tail];
        outWidth = slot.width;
        outHeight = slot.height;
        outTimestampMs = slot.timestampMs;
        outByteCount = slot.byteCount;
        std::memcpy(outData, slot.data, slot.byteCount);

        view_->header.tail.store((tail + 1) % kSlotCount, std::memory_order_release);
        return true;
    }

    // Consumer side: blocks until a new frame is signaled or the timeout
    // elapses. Avoids busy-polling while waiting for the hook to produce.
    bool WaitForFrame(DWORD timeoutMs) {
        if (!newFrameEvent_) return false;
        return WaitForSingleObject(newFrameEvent_, timeoutMs) == WAIT_OBJECT_0;
    }

private:
    bool MapAndInit(bool isProducer) {
        view_ = static_cast<SharedMemoryLayout*>(
            MapViewOfFile(mapping_, FILE_MAP_ALL_ACCESS, 0, 0, kSharedMemoryTotalSize));
        if (!view_) {
            CloseHandle(mapping_);
            mapping_ = nullptr;
            return false;
        }

        if (isProducer) {
            // First producer to create the section initializes the header;
            // a re-attach (hook restarted) resets head/tail so stale slot
            // indices from a previous session can't be read as valid.
            view_->header.protocolVersion = kSharedMemoryProtocolVersion;
            view_->header.head.store(0, std::memory_order_relaxed);
            view_->header.tail.store(0, std::memory_order_relaxed);
        } else if (view_->header.protocolVersion != kSharedMemoryProtocolVersion) {
            UnmapViewOfFile(view_);
            view_ = nullptr;
            CloseHandle(mapping_);
            mapping_ = nullptr;
            return false;
        }

        newFrameEvent_ = isProducer
            ? CreateEventW(nullptr, FALSE, FALSE, kNewFrameEventName)
            : OpenEventW(EVENT_ALL_ACCESS | SYNCHRONIZE, FALSE, kNewFrameEventName);
        return newFrameEvent_ != nullptr;
    }

    HANDLE mapping_ = nullptr;
    HANDLE newFrameEvent_ = nullptr;
    SharedMemoryLayout* view_ = nullptr;
};

} // namespace braps
