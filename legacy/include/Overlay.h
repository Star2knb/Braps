#pragma once

#include <atomic>
#include <thread>
#include <windows.h>

class Recorder;

// Small always-on-top HUD showing live capture FPS and recording state.
// Local to the user's screen only — reads Recorder state but never touches
// captured frame data, so it can't end up baked into the recorded video
// regardless of which capture backend (DXGI/GDI/hook) is active.
class Overlay {
public:
    explicit Overlay(Recorder& recorder);
    ~Overlay();

    void Start();
    void Stop();

private:
    void ThreadMain();
    void Paint(HWND hwnd);

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    Recorder& recorder_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    HWND hwnd_ = nullptr;
};
