// Global hotkey and audible cue (recorder plan §13.1, R7, X4).
//
// A thread with its own message loop registers the hotkey with RegisterHotKey, so it works while a
// game has the focus. If another program owns the key, a low-level keyboard hook on the same thread
// takes over (E7003). Presses within 300 ms of the last one, and key auto-repeat, are ignored (W7002).
#pragma once

#include <windows.h>

#include <atomic>
#include <string>
#include <thread>

#include "rec/options.h"

namespace rec {

enum class Cue { Start, Stop, Error };

class Hotkeys {
public:
    Hotkeys() = default;
    ~Hotkeys();
    Hotkeys(const Hotkeys&) = delete;
    Hotkeys& operator=(const Hotkeys&) = delete;

    // Starts the thread. False (with the reason) if the key can't be registered in either way.
    bool start(const Hotkey& key, std::string* error);
    void stop();

    // Signalled on every accepted press; wait on it instead of polling.
    HANDLE event() const { return event_; }
    // True once per accepted press.
    bool take_press() { return pending_.exchange(0) != 0; }
    bool using_keyboard_hook() const { return hook_mode_; }

    // Starts playing a short sound and returns at once.
    static void play(Cue cue);

private:
    friend LRESULT CALLBACK low_level_proc(int, WPARAM, LPARAM);
    void run();
    void on_press();

    Hotkey key_{};
    std::thread thread_;
    DWORD thread_id_ = 0;
    HANDLE event_ = nullptr;
    HANDLE ready_ = nullptr;
    std::atomic<int> pending_{0};
    std::atomic<bool> hook_mode_{false};
    std::string start_error_;
    bool start_ok_ = false;
    int64_t last_press_qpc_ = 0;
    bool key_down_ = false;
    HHOOK hook_ = nullptr;
};

}  // namespace rec
