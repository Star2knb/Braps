// MSB-first bit writer/reader (codec plan §5.6 steps 5-6).
#pragma once

#include <cstddef>
#include <cstdint>

namespace rcv {

// 64-bit accumulator; whole 32-bit words are emitted as soon as they are complete.
// The caller guarantees the destination is large enough (sizes are known before encoding).
class BitWriter {
public:
    explicit BitWriter(uint8_t* dst) : start_(dst), p_(dst) {}

    // code must fit in len bits; 1 <= len <= 32.
    void put(uint32_t code, int len) {
        acc_ = (acc_ << len) | code;
        bits_ += len;
        if (bits_ >= 32) {
            bits_ -= 32;
            const uint32_t v = uint32_t(acc_ >> bits_);
            p_[0] = uint8_t(v >> 24);
            p_[1] = uint8_t(v >> 16);
            p_[2] = uint8_t(v >> 8);
            p_[3] = uint8_t(v);
            p_ += 4;
        }
    }

    // Emits the remaining bits, zero-padded to a byte boundary. Returns total bytes written.
    size_t finish() {
        while (bits_ >= 8) {
            bits_ -= 8;
            *p_++ = uint8_t(acc_ >> bits_);
        }
        if (bits_ > 0) {
            *p_++ = uint8_t(acc_ << (8 - bits_));
            bits_ = 0;
        }
        return size_t(p_ - start_);
    }

private:
    uint8_t* start_;
    uint8_t* p_;
    uint64_t acc_ = 0;
    int bits_ = 0;
};

// Bits are kept left-aligned in a 64-bit buffer; never reads past `end`.
class BitReader {
public:
    BitReader(const uint8_t* p, const uint8_t* end) : start_(p), p_(p), end_(end) {}

    void refill() {
        while (count_ <= 56 && p_ < end_) {
            buf_ |= uint64_t(*p_++) << (56 - count_);
            count_ += 8;
        }
    }
    int count() const { return count_; }
    uint32_t peek12() const { return uint32_t(buf_ >> 52); }
    // Returns false if fewer than n real bits remain.
    bool consume(int n) {
        if (n > count_) return false;
        buf_ <<= n;
        count_ -= n;
        return true;
    }
    size_t bits_consumed() const { return size_t(p_ - start_) * 8 - size_t(count_); }

private:
    const uint8_t* start_;
    const uint8_t* p_;
    const uint8_t* end_;
    uint64_t buf_ = 0;
    int count_ = 0;
};

}  // namespace rcv
