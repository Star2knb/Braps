// Optional per-stage encoder timing for rcv_bench (codec plan §11.5).
// Internal API: not exported from the DLL. Costs one branch per stage when no sink is attached.
#pragma once

#include <chrono>
#include <cstdint>

#include "rcv/rcv.h"

namespace rcv {

struct StageTimes {
    uint64_t skip_ns = 0;     // phase A: temporal skip compare
    uint64_t skip_frames = 0; // frames where phase A ran (incl. frames that became DUP)
    uint64_t load_ns = 0;     // input copy / NV12 de-interleave into the reference
    uint64_t predict_ns = 0;  // residuals + histogram
    uint64_t table_ns = 0;    // chunk mode decision + Huffman table build
    uint64_t entropy_ns = 0;  // bitstream / RAW payload writing
    uint64_t total_ns = 0;    // whole rcv_encode_frame for I/P frames (the rest is assembly, CRC)
    uint64_t frames = 0;      // coded (I/P) frames accumulated
};

// Attaches a sink that accumulates stage times for every coded frame (nullptr detaches).
void set_stage_profile(rcv_encoder* enc, StageTimes* sink);

class StageTimer {
public:
    using Clock = std::chrono::steady_clock;

    explicit StageTimer(StageTimes* sink) : sink_(sink) {
        if (sink_) last_ = Clock::now();
    }
    // Adds the time since the previous lap to the given stage.
    void lap(uint64_t StageTimes::*stage) {
        if (!sink_) return;
        const Clock::time_point now = Clock::now();
        sink_->*stage += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(now - last_).count());
        last_ = now;
    }

private:
    StageTimes* sink_;
    Clock::time_point last_{};
};

}  // namespace rcv
