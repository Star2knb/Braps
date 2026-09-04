#pragma once

#include <vector>
#include <atomic>
#include <cstdint>
#include <cstring>

// Fixed-capacity, single-producer/single-consumer lock-free ring buffer of
// raw BGRA frames. The capture thread pushes; the disk/encoder-feed thread
// pops. When full, the newest frame is dropped rather than blocking capture,
// since keeping the game's render loop unstalled matters more than never
// losing a frame.
class FrameRingBuffer {
public:
    struct Frame {
        std::vector<uint8_t> data;
        int width = 0;
        int height = 0;
        uint64_t timestampMs = 0;
    };

    explicit FrameRingBuffer(size_t capacity) : capacity_(capacity), slots_(capacity) {}

    bool TryPush(const uint8_t* srcData, size_t byteCount, int width, int height, uint64_t timestampMs) {
        size_t head = head_.load(std::memory_order_relaxed);
        size_t nextHead = (head + 1) % capacity_;
        if (nextHead == tail_.load(std::memory_order_acquire)) {
            return false; // full, drop this frame
        }

        Frame& slot = slots_[head];
        slot.data.resize(byteCount);
        std::memcpy(slot.data.data(), srcData, byteCount);
        slot.width = width;
        slot.height = height;
        slot.timestampMs = timestampMs;

        head_.store(nextHead, std::memory_order_release);
        return true;
    }

    bool TryPop(Frame& out) {
        size_t tail = tail_.load(std::memory_order_relaxed);
        if (tail == head_.load(std::memory_order_acquire)) {
            return false; // empty
        }
        out = std::move(slots_[tail]);
        tail_.store((tail + 1) % capacity_, std::memory_order_release);
        return true;
    }

    bool Empty() const {
        return tail_.load(std::memory_order_acquire) == head_.load(std::memory_order_acquire);
    }

    void Clear() {
        head_.store(0, std::memory_order_relaxed);
        tail_.store(0, std::memory_order_relaxed);
    }

private:
    size_t capacity_;
    std::vector<Frame> slots_;
    std::atomic<size_t> head_{0};
    std::atomic<size_t> tail_{0};
};
