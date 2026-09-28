#pragma once

#include <functional>
#include <atomic>
#include <thread>

// Registers global hotkeys (F9 = toggle recording, F10 = screenshot) using
// the native Win32 RegisterHotKey API so they work even when the app window
// is unfocused or minimized, with zero external hotkey library dependency.
class Hotkeys {
public:
    using Callback = std::function<void()>;

    Hotkeys(Callback onToggleRecording, Callback onScreenshot);
    ~Hotkeys();

    void Start();
    void Stop();

private:
    void MessageLoop();

    Callback onToggleRecording_;
    Callback onScreenshot_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    unsigned long threadId_ = 0;
};
