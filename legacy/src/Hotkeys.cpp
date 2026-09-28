#include "Hotkeys.h"

#include <windows.h>
#include <iostream>

namespace {
constexpr int kHotkeyIdRecord = 1;
constexpr int kHotkeyIdScreenshot = 2;
}

Hotkeys::Hotkeys(Callback onToggleRecording, Callback onScreenshot)
    : onToggleRecording_(std::move(onToggleRecording)), onScreenshot_(std::move(onScreenshot)) {}

Hotkeys::~Hotkeys() {
    Stop();
}

void Hotkeys::Start() {
    running_ = true;
    thread_ = std::thread(&Hotkeys::MessageLoop, this);
}

void Hotkeys::Stop() {
    if (running_.exchange(false) && thread_.joinable()) {
        PostThreadMessageW(threadId_, WM_QUIT, 0, 0);
        thread_.join();
    }
}

void Hotkeys::MessageLoop() {
    threadId_ = GetCurrentThreadId();

    if (!RegisterHotKey(nullptr, kHotkeyIdRecord, 0, VK_F9)) {
        std::cerr << "[Hotkeys] Failed to register F9 (toggle recording).\n";
    }
    if (!RegisterHotKey(nullptr, kHotkeyIdScreenshot, 0, VK_F10)) {
        std::cerr << "[Hotkeys] Failed to register F10 (screenshot).\n";
    }

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (msg.message == WM_HOTKEY) {
            if (msg.wParam == kHotkeyIdRecord && onToggleRecording_) {
                onToggleRecording_();
            } else if (msg.wParam == kHotkeyIdScreenshot && onScreenshot_) {
                onScreenshot_();
            }
        }
    }

    UnregisterHotKey(nullptr, kHotkeyIdRecord);
    UnregisterHotKey(nullptr, kHotkeyIdScreenshot);
}
