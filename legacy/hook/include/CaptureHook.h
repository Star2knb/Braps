#pragma once

#include "ICapture.h"
#include "SharedFrameChannel.h"

#include <vector>
#include <cstdint>
#include <chrono>

// ICapture implementation that reads frames out of the shared memory
// channel populated by BrapsHook.dll (injected into the target game). This
// lets Recorder use whichever capture backend is active — DXGI Desktop
// Duplication, GDI, or this hook-based path — through the same interface
// and downstream pipeline (ring buffer, disk writer, encoder) unchanged.
class CaptureHook : public ICapture {
public:
    // fpsTarget bounds how often CaptureFrame delivers a "new" frame to
    // the caller — without this, a fast-rendering game could deliver
    // frames well above the requested rate (Recorder skips its own
    // sleep_for for push-driven backends, since the hook's blocking wait
    // is meant to BE the pacing mechanism; nothing else throttled it).
    // That excess then hit FFmpeg's VFR encoder as sub-20ms gaps, which it
    // silently merged/dropped — capture looked uncapped and roughly half
    // of it never made it into the final video. The channel is still
    // drained on every call regardless, so a burst of frames faster than
    // the target doesn't back up in shared memory; only which one gets
    // delivered (always the newest) and how often is throttled.
    explicit CaptureHook(int fpsTarget = 30);

    bool Initialize() override;
    bool CaptureFrame(const FrameCallback& onFrame, bool needsPixelData = true) override;
    void Shutdown() override;
    bool IsPushDriven() const override { return true; }

    // True once the injected hook has actually attached and is producing
    // frames (checked via the shared header), as opposed to just having
    // successfully opened the shared memory section.
    bool IsHookAttached();

    // Diagnostic breakdown of the most recent CaptureFrame() call, in ms.
    double LastWaitMs() const { return lastWaitMs_; }
    double LastDrainMs() const { return lastDrainMs_; }
    int LastFramesDrained() const { return lastFramesDrained_; }

private:
    double lastWaitMs_ = 0.0;
    double lastDrainMs_ = 0.0;
    int lastFramesDrained_ = 0;
    braps::SharedFrameChannel channel_;
    std::vector<uint8_t> scratchBuffer_;

    std::chrono::duration<double, std::milli> minFrameInterval_;
    // The throttle's next allowed delivery time, advanced by a fixed
    // minFrameInterval_ on every ACCEPTED delivery (not set to "now") —
    // this is a fixed schedule, not "time since the last frame we let
    // through". Using "now" as the baseline created a self-reinforcing
    // reject/accept alternation whenever the real source rate was close
    // to fpsTarget (exactly the common case: game and target both near
    // 60fps): each accepted frame nudged the baseline forward by however
    // late that delivery happened to run, making the very next check more
    // likely to reject, and a rejection (which didn't move the baseline)
    // made the one after THAT more likely to accept — measured on real
    // hardware to roughly halve throughput (~60fps in, ~39fps delivered)
    // instead of passing a same-rate source through cleanly.
    std::chrono::steady_clock::time_point nextAllowedDeliveryAt_;
    bool haveDelivered_ = false;
};
