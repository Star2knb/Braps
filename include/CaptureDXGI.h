#pragma once

#include "ICapture.h"

#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <vector>
#include <cstdint>

using Microsoft::WRL::ComPtr;

// Captures the primary output via DXGI Desktop Duplication. This runs above
// the game's rendering pipeline (post-composition), giving near-zero CPU
// overhead on modern hardware/drivers. Requires Windows 8+ and DXGI 1.2.
class CaptureDXGI : public ICapture {
public:
    bool Initialize() override;
    bool CaptureFrame(const FrameCallback& onFrame, bool needsPixelData = true) override;
    void Shutdown() override;

    // How long AcquireNextFrame blocks waiting for a new frame before
    // timing out. Should be set below the caller's frame interval (e.g.
    // Recorder scales this to fpsTarget_) so a high FPS target isn't stuck
    // waiting out a fixed 16ms timeout longer than the whole frame budget.
    void SetPollTimeoutMs(unsigned int timeoutMs) { pollTimeoutMs_ = timeoutMs; }

    // Diagnostic breakdown of the most recent CaptureFrame() call, in ms.
    double LastAcquireMs() const { return lastAcquireMs_; }
    double LastCopyMapMs() const { return lastCopyMapMs_; }

private:
    bool CopyStagingTextureToBuffer(int width, int height);

    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<IDXGIOutputDuplication> duplication_;
    ComPtr<ID3D11Texture2D> stagingTexture_;

    int width_ = 0;
    int height_ = 0;
    unsigned int pollTimeoutMs_ = 16;
    std::vector<uint8_t> scratchBuffer_;

    double lastAcquireMs_ = 0.0;
    double lastCopyMapMs_ = 0.0;
};
