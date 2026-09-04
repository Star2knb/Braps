#pragma once

#include "ICapture.h"

#include <windows.h>
#include <vector>
#include <cstdint>

// Ultra-compatible fallback capture using classic BitBlt against the
// desktop DC. Heavier on the CPU than DXGI Desktop Duplication, but works on
// virtually any Windows machine, including hardware/drivers that don't
// support DXGI 1.2.
class CaptureGDI : public ICapture {
public:
    bool Initialize() override;
    bool CaptureFrame(const FrameCallback& onFrame, bool needsPixelData = true) override;
    void Shutdown() override;

private:
    HDC screenDC_ = nullptr;
    HDC memDC_ = nullptr;
    HBITMAP bitmap_ = nullptr;
    HBITMAP oldBitmap_ = nullptr;

    int width_ = 0;
    int height_ = 0;
    std::vector<uint8_t> scratchBuffer_;
};
