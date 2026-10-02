// The rate controller (recorder plan §9). Two different overloads, two different answers:
//
//   packet queue filling  -> the disk is too slow: compress harder (NEAR 1..3), and when even that is not
//                            enough replace incoming frames with DUPs until the queue drains
//   frame ring filling or encoding slower than the frame budget -> the CPU is too slow: near-lossless
//                            costs more CPU, not less, so frames are dropped (turned into DUPs) on purpose
//
// Pure logic, no clocks or threads of its own: the pipeline calls decide() for every frame it is handed.
#pragma once

#include <array>
#include <cstdint>

namespace rec {

struct RateSettings {
    int levels[4] = {40, 60, 75, 90};  // packet-queue % at which level 1, 2, 3, 4 begin
    int step_down_after_ms = 2000;     // a level is left only after the queue stayed below its entry threshold this long
    bool strict = false;               // rcv-strict: lossless or nothing, levels 1-3 are skipped
    double frame_ms = 16.667;          // budget per frame (1000 / fps)
    int ring_high_pct = 50;            // CPU overload starts here ...
    int ring_low_pct = 25;             // ... and ends below this
    double encode_budget = 0.8;        // ... or when the average encode time is over this share of a frame and the ring is filling
};

struct RateInputs {
    int64_t now_ms = 0;
    int queue_pct = 0;           // packet queue fill
    int ring_pct = 0;            // frame ring fill
    double encode_avg_ms = 0;    // rolling average
    double write_mb_s = 0;       // recent disk speed (for the log)
};

struct RateDecision {
    int near_level = 0;   // NEAR for this frame (0 = lossless)
    bool drop = false;    // replace this frame with a DUP
};

class RateController {
public:
    static constexpr int kLevels = 5;  // 0 lossless, 1-3 NEAR, 4 dropping

    RateController() = default;
    explicit RateController(const RateSettings& s) : s_(s) {}
    void configure(const RateSettings& s) { s_ = s; }

    RateDecision decide(const RateInputs& in);

    int level() const { return level_; }
    bool cpu_overloaded() const { return cpu_overload_; }
    // Milliseconds spent at each level since the first decision (the last interval up to `now_ms`).
    std::array<double, kLevels> ms_at_level(int64_t now_ms) const;

    // Called when a level changes (W3101) and when CPU overload starts (W3102).
    struct Hooks {
        void (*level_changed)(void* user, int old_level, int new_level, const RateInputs& in) = nullptr;
        void (*cpu_overload_started)(void* user, const RateInputs& in) = nullptr;
        void (*cpu_overload_ended)(void* user, const RateInputs& in) = nullptr;
        void* user = nullptr;
    } hooks;

private:
    int target_level(int queue_pct) const;  // the level the queue asks for, ignoring hysteresis

    RateSettings s_;
    int level_ = 0;
    bool cpu_overload_ = false;
    bool started_ = false;
    int64_t last_ms_ = 0;
    int64_t below_since_ms_ = -1;  // since when the queue has been below the current level's threshold
    std::array<double, kLevels> time_ms_{};
};

}  // namespace rec
