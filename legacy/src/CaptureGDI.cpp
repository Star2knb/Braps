#include "CaptureGDI.h"

#include <iostream>

bool CaptureGDI::Initialize() {
    width_ = GetSystemMetrics(SM_CXSCREEN);
    height_ = GetSystemMetrics(SM_CYSCREEN);
    if (width_ <= 0 || height_ <= 0) {
        std::cerr << "[CaptureGDI] Invalid screen metrics.\n";
        return false;
    }

    screenDC_ = GetDC(nullptr);
    if (!screenDC_) return false;

    memDC_ = CreateCompatibleDC(screenDC_);
    bitmap_ = CreateCompatibleBitmap(screenDC_, width_, height_);
    oldBitmap_ = static_cast<HBITMAP>(SelectObject(memDC_, bitmap_));

    scratchBuffer_.resize(static_cast<size_t>(width_) * height_ * 4);

    std::cout << "[CaptureGDI] Initialized at " << width_ << "x" << height_ << "\n";
    return true;
}

bool CaptureGDI::CaptureFrame(const FrameCallback& onFrame, bool needsPixelData) {
    if (!memDC_) return false;

    if (!BitBlt(memDC_, 0, 0, width_, height_, screenDC_, 0, 0, SRCCOPY)) {
        return false;
    }

    // GDI has no separate "acknowledge without reading" step like DXGI's
    // AcquireNextFrame/ReleaseFrame — BitBlt itself is unavoidable — but the
    // DIB readback (GetDIBits) is skippable when nothing will consume the
    // pixels, so idle polling still avoids that copy.
    if (needsPixelData) {
        BITMAPINFOHEADER bi{};
        bi.biSize = sizeof(BITMAPINFOHEADER);
        bi.biWidth = width_;
        bi.biHeight = -height_; // negative = top-down DIB, matches DXGI path
        bi.biPlanes = 1;
        bi.biBitCount = 32;
        bi.biCompression = BI_RGB;

        int lines = GetDIBits(memDC_, bitmap_, 0, height_, scratchBuffer_.data(),
                               reinterpret_cast<BITMAPINFO*>(&bi), DIB_RGB_COLORS);
        if (lines == 0) return false;

        CapturedFrame frame{scratchBuffer_.data(), scratchBuffer_.size(), width_, height_};
        onFrame(frame);
    }
    return true;
}

void CaptureGDI::Shutdown() {
    if (memDC_) {
        SelectObject(memDC_, oldBitmap_);
        DeleteObject(bitmap_);
        DeleteDC(memDC_);
        memDC_ = nullptr;
    }
    if (screenDC_) {
        ReleaseDC(nullptr, screenDC_);
        screenDC_ = nullptr;
    }
}
