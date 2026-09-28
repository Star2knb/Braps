#include "CaptureHook.h"

#include <iostream>
#include <chrono>
#include <algorithm>

CaptureHook::CaptureHook(int fpsTarget)
    : minFrameInterval_(1000.0 / std::max(1, fpsTarget)) {}

bool CaptureHook::Initialize() {
    if (!channel_.OpenAsConsumer()) {
        std::cerr << "[CaptureHook] Could not open shared memory channel — "
                     "is BrapsHook.dll injected into the target process?\n";
        return false;
    }
    scratchBuffer_.resize(braps::kMaxFrameBytes);
    return true;
}

bool CaptureHook::CaptureFrame(const FrameCallback& onFrame, bool needsPixelData) {
    // Tell the hook (running inside the game's process) whether we
    // actually want frames right now, BEFORE it does any GPU readback
    // work on its next Present call. Without this, the hook paid full
    // CopyResource/Map cost on every single frame from the moment it was
    // injected — a real, measured FPS cost to the game even while Braps
    // was idle and not recording anything.
    channel_.Header().consumerWantsFrames.store(needsPixelData, std::memory_order_release);

    // The hook is push-driven — frames arrive on the game's own Present()
    // schedule, not ours — so this wait IS the pacing mechanism (Recorder
    // skips its fixed sleep_for for push-driven backends; see
    // ICapture::IsPushDriven). The timeout just bounds how long we can
    // block before re-checking running_/isRecording_, not a frame budget,
    // so it can be generous without costing throughput.
    //
    // A single WaitForFrame + TryPop per call let the shared ring buffer's
    // auto-reset event coalesce multiple pushes into one wakeup: only one
    // frame got drained per wakeup, the buffer stayed full, TryPush kept
    // dropping (and dropped pushes never re-signal), and callers ended up
    // blocked for the full wait timeout repeatedly. Draining everything
    // currently queued and keeping only the newest avoids that pileup and
    // gives the most up-to-date frame rather than a stale queued one.
    auto waitStart = std::chrono::steady_clock::now();
    bool signaled = channel_.WaitForFrame(needsPixelData ? 250 : 0);
    lastWaitMs_ = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - waitStart).count();
    if (!signaled) {
        lastDrainMs_ = 0.0;
        lastFramesDrained_ = 0;
        return false;
    }

    auto drainStart = std::chrono::steady_clock::now();
    bool gotAny = false;
    int framesDrained = 0;
    uint32_t width = 0, height = 0, byteCount = 0;
    uint64_t timestampMs = 0;
    while (channel_.TryPop(scratchBuffer_.data(), width, height, timestampMs, byteCount)) {
        gotAny = true;
        ++framesDrained;
    }
    lastDrainMs_ = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - drainStart).count();
    lastFramesDrained_ = framesDrained;
    if (!gotAny) return false;

    // Throttle to fpsTarget: the channel is always drained above (so a
    // fast-rendering game never backs up shared memory), but a "new"
    // frame is only actually delivered to the caller often enough to
    // match the target rate. Without this, frames arriving faster than
    // requested still got through (Recorder's own pacing sleep is
    // intentionally skipped for push-driven backends), which downstream
    // FFmpeg's VFR encoder would then silently merge/drop as too-close-
    // together — capture looked uncapped and part of it never reached
    // the final video.
    //
    // Compares against a fixed schedule (nextAllowedDeliveryAt_, advanced
    // by exactly minFrameInterval_ on every accepted delivery) rather than
    // "time since the last accepted delivery" — the latter measured "now"
    // AFTER this call's own wait+drain latency, so an accepted delivery's
    // timestamp silently included whatever jitter that call happened to
    // take, making the next check's baseline drift. When the real source
    // rate is close to fpsTarget (the common case), that drift produced a
    // self-reinforcing reject/accept alternation that roughly halved
    // throughput instead of passing a same-rate source through cleanly.
    auto now = std::chrono::steady_clock::now();
    if (haveDelivered_ && now < nextAllowedDeliveryAt_) {
        return false;
    }
    // Schedule from the target time, not from "now": if this delivery ran
    // a little late, the next slot is still minFrameInterval_ after the
    // slot we just filled, not shifted later by that lateness — otherwise
    // occasional scheduling delays compound instead of self-correcting.
    nextAllowedDeliveryAt_ = haveDelivered_
        ? nextAllowedDeliveryAt_ + std::chrono::duration_cast<std::chrono::steady_clock::duration>(minFrameInterval_)
        : now + std::chrono::duration_cast<std::chrono::steady_clock::duration>(minFrameInterval_);
    haveDelivered_ = true;

    if (needsPixelData) {
        CapturedFrame frame{scratchBuffer_.data(), byteCount, static_cast<int>(width), static_cast<int>(height)};
        onFrame(frame);
    }
    return true;
}

void CaptureHook::Shutdown() {
    channel_.Close();
}

bool CaptureHook::IsHookAttached() {
    return channel_.Header().hookAttached.load(std::memory_order_acquire);
}
