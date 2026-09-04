#pragma once

#include <cstdint>
#include <functional>

// A single captured frame, always normalized to top-down BGRA before the
// callback fires so downstream code (ring buffer, encoder) never needs to
// know which backend produced it.
struct CapturedFrame {
    const uint8_t* data;
    size_t byteCount;
    int width;
    int height;
};

using FrameCallback = std::function<void(const CapturedFrame&)>;

// Common interface for capture backends. DXGI Desktop Duplication is the
// primary path (low overhead, GPU-side); GDI BitBlt is the compatibility
// fallback for hardware/drivers that don't support DXGI 1.2; CaptureHook
// (hook/) reads frames pushed by an injected D3D11 Present hook.
class ICapture {
public:
    virtual ~ICapture() = default;
    virtual bool Initialize() = 0;

    // needsPixelData: when false, the caller only cares that a new frame
    // was signaled (or not) — e.g. idle polling between recordings. A
    // backend can use this to skip its expensive GPU->CPU readback
    // (CopyResource + Map + memcpy) entirely and just acknowledge/release
    // the frame, since nothing will consume the pixels anyway.
    virtual bool CaptureFrame(const FrameCallback& onFrame, bool needsPixelData = true) = 0;
    virtual void Shutdown() = 0;

    // Poll-based backends (DXGI, GDI) have no natural frame cadence of
    // their own — the caller (Recorder::CaptureLoop) must sleep for the
    // remainder of each frame budget itself to pace polling.
    // Push-based backends (CaptureHook) receive frames on the producer's
    // own schedule — the game's Present() calls define the real cadence —
    // and CaptureFrame's own blocking wait already provides that pacing.
    // Layering a fixed sleep_for on top of an already-blocking wait was
    // exactly the bug that made the hook path fight its own timing, so
    // Recorder skips its sleep entirely when this returns true.
    virtual bool IsPushDriven() const { return false; }
};
