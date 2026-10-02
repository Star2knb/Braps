#include "rec/rate.h"

#include <algorithm>

namespace rec {

int RateController::target_level(int queue_pct) const {
    if (s_.strict) return queue_pct >= s_.levels[3] ? 4 : 0;
    int level = 0;
    for (int i = 0; i < 4; ++i)
        if (queue_pct >= s_.levels[i]) level = i + 1;
    return level;
}

RateDecision RateController::decide(const RateInputs& in) {
    if (!started_) {
        started_ = true;
        last_ms_ = in.now_ms;
    }
    time_ms_[size_t(level_)] += double((std::max)(int64_t(0), in.now_ms - last_ms_));
    last_ms_ = in.now_ms;

    // Disk: up at once, down one level at a time after the queue stayed below the level's entry threshold.
    const int want = target_level(in.queue_pct);
    auto change = [&](int to) {
        const int from = level_;
        level_ = to;
        below_since_ms_ = -1;
        if (hooks.level_changed) hooks.level_changed(hooks.user, from, to, in);
    };
    if (want > level_) {
        change(want);
    } else if (level_ == 4) {
        // Dropping goes on until the queue is below the third threshold, then the normal stepping down takes over.
        if (in.queue_pct < s_.levels[2]) change(s_.strict ? 0 : 3);
    } else if (level_ > 0 && want < level_) {
        if (below_since_ms_ < 0) below_since_ms_ = in.now_ms;
        if (in.now_ms - below_since_ms_ >= s_.step_down_after_ms) change(level_ - 1);
    } else {
        below_since_ms_ = -1;
    }

    // CPU: the ring filling up, or encoding slower than the budget while the ring is not empty.
    const bool slow_encode = in.encode_avg_ms > s_.encode_budget * s_.frame_ms;
    if (!cpu_overload_ && (in.ring_pct >= s_.ring_high_pct || (slow_encode && in.ring_pct >= s_.ring_low_pct))) {
        cpu_overload_ = true;
        if (hooks.cpu_overload_started) hooks.cpu_overload_started(hooks.user, in);
    } else if (cpu_overload_ && in.ring_pct < s_.ring_low_pct) {
        cpu_overload_ = false;
        if (hooks.cpu_overload_ended) hooks.cpu_overload_ended(hooks.user, in);
    }

    RateDecision d;
    d.near_level = level_ >= 4 ? 3 : level_;
    d.drop = level_ == 4 || cpu_overload_;
    return d;
}

std::array<double, RateController::kLevels> RateController::ms_at_level(int64_t now_ms) const {
    std::array<double, kLevels> t = time_ms_;
    if (started_) t[size_t(level_)] += double((std::max)(int64_t(0), now_ms - last_ms_));
    return t;
}

}  // namespace rec
