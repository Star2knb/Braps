#pragma once

#include "ICapture.h"
#include "SharedFrameChannel.h"

#include <vector>
#include <cstdint>

// ICapture implementation that reads frames out of the shared memory
// channel populated by BrapsHook.dll (injected into the target game). This
// lets Recorder use whichever capture backend is active — DXGI Desktop
// Duplication, GDI, or this hook-based path — through the same interface
// and downstream pipeline (ring buffer, disk writer, encoder) unchanged.
class CaptureHook : public ICapture {
public:
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
};
