#include "CaptureDXGI.h"

#include <dxgi1_2.h>
#include <cstring>
#include <iostream>
#include <chrono>

bool CaptureDXGI::Initialize() {
    D3D_FEATURE_LEVEL featureLevel;
    HRESULT hr = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        nullptr, 0, D3D11_SDK_VERSION,
        device_.GetAddressOf(), &featureLevel, context_.GetAddressOf());
    if (FAILED(hr)) {
        std::cerr << "[CaptureDXGI] D3D11CreateDevice failed: 0x" << std::hex << hr << "\n";
        return false;
    }

    ComPtr<IDXGIDevice> dxgiDevice;
    if (FAILED(device_.As(&dxgiDevice))) return false;

    ComPtr<IDXGIAdapter> adapter;
    if (FAILED(dxgiDevice->GetAdapter(adapter.GetAddressOf()))) return false;

    ComPtr<IDXGIOutput> output;
    if (FAILED(adapter->EnumOutputs(0, output.GetAddressOf()))) {
        std::cerr << "[CaptureDXGI] No primary output found.\n";
        return false;
    }

    ComPtr<IDXGIOutput1> output1;
    if (FAILED(output.As(&output1))) return false;

    hr = output1->DuplicateOutput(device_.Get(), duplication_.GetAddressOf());
    if (FAILED(hr)) {
        std::cerr << "[CaptureDXGI] DuplicateOutput failed (hardware/driver may lack DXGI 1.2): 0x"
                   << std::hex << hr << "\n";
        return false;
    }

    DXGI_OUTDUPL_DESC dupDesc{};
    duplication_->GetDesc(&dupDesc);
    width_ = static_cast<int>(dupDesc.ModeDesc.Width);
    height_ = static_cast<int>(dupDesc.ModeDesc.Height);
    scratchBuffer_.resize(static_cast<size_t>(width_) * height_ * 4);

    D3D11_TEXTURE2D_DESC stagingDesc{};
    stagingDesc.Width = dupDesc.ModeDesc.Width;
    stagingDesc.Height = dupDesc.ModeDesc.Height;
    stagingDesc.MipLevels = 1;
    stagingDesc.ArraySize = 1;
    stagingDesc.Format = dupDesc.ModeDesc.Format;
    stagingDesc.SampleDesc.Count = 1;
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

    hr = device_->CreateTexture2D(&stagingDesc, nullptr, stagingTexture_.GetAddressOf());
    if (FAILED(hr)) {
        std::cerr << "[CaptureDXGI] Failed to create staging texture: 0x" << std::hex << hr << "\n";
        return false;
    }

    double refreshHz = 0.0;
    if (dupDesc.ModeDesc.RefreshRate.Denominator != 0) {
        refreshHz = static_cast<double>(dupDesc.ModeDesc.RefreshRate.Numerator) /
                    dupDesc.ModeDesc.RefreshRate.Denominator;
    }
    std::cout << "[CaptureDXGI] Initialized at " << width_ << "x" << height_
               << " (display refresh: " << refreshHz << " Hz)\n";
    if (refreshHz > 0.0) {
        std::cout << "[CaptureDXGI] Note: DXGI Desktop Duplication can only deliver a new frame "
                      "as fast as the display refreshes, so capture is hard-capped at ~"
                   << refreshHz << " fps regardless of --fps.\n";
    }
    return true;
}

bool CaptureDXGI::CaptureFrame(const FrameCallback& onFrame, bool needsPixelData) {
    if (!duplication_) return false;

    ComPtr<IDXGIResource> desktopResource;
    DXGI_OUTDUPL_FRAME_INFO frameInfo{};

    auto acquireStart = std::chrono::steady_clock::now();
    HRESULT hr = duplication_->AcquireNextFrame(pollTimeoutMs_, &frameInfo, desktopResource.GetAddressOf());
    lastAcquireMs_ = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - acquireStart).count();

    if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
        return false; // no new frame yet, not an error
    }
    if (FAILED(hr)) {
        std::cerr << "[CaptureDXGI] AcquireNextFrame failed: 0x" << std::hex << hr << "\n";
        return false;
    }

    // When idle (not recording), the caller only needs to know a frame
    // arrived, not its pixels. Skipping CopyResource/Map/memcpy here avoids
    // a GPU sync point and a full-frame memcpy on every single poll while
    // nothing is actually consuming the data — real savings on integrated
    // graphics sharing memory bandwidth with whatever's being captured.
    lastCopyMapMs_ = 0.0;
    if (needsPixelData) {
        auto copyMapStart = std::chrono::steady_clock::now();
        ComPtr<ID3D11Texture2D> acquiredTexture;
        hr = desktopResource.As(&acquiredTexture);
        if (SUCCEEDED(hr)) {
            context_->CopyResource(stagingTexture_.Get(), acquiredTexture.Get());

            D3D11_MAPPED_SUBRESOURCE mapped{};
            hr = context_->Map(stagingTexture_.Get(), 0, D3D11_MAP_READ, 0, &mapped);
            if (SUCCEEDED(hr)) {
                uint8_t* dst = scratchBuffer_.data();
                const uint8_t* src = static_cast<const uint8_t*>(mapped.pData);
                const size_t rowBytes = static_cast<size_t>(width_) * 4;
                for (int y = 0; y < height_; ++y) {
                    std::memcpy(dst + y * rowBytes, src + y * mapped.RowPitch, rowBytes);
                }
                context_->Unmap(stagingTexture_.Get(), 0);

                CapturedFrame frame{scratchBuffer_.data(), scratchBuffer_.size(), width_, height_};
                onFrame(frame);
            }
        }
        lastCopyMapMs_ = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - copyMapStart).count();
    }

    duplication_->ReleaseFrame();
    return true;
}

void CaptureDXGI::Shutdown() {
    duplication_.Reset();
    stagingTexture_.Reset();
    context_.Reset();
    device_.Reset();
}
