#include "rec/hotkeys.h"

#include <mmsystem.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#include "rec/log.h"
#include "rec/paths.h"

namespace rec {

namespace {

Hotkeys* g_hook_owner = nullptr;  // the instance the low-level hook reports to (one hotkey per process)

constexpr int kDebounceMs = 300;
constexpr int kHotkeyId = 1;

int64_t qpc_now() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

int64_t qpc_freq() {
    static const int64_t f = [] {
        LARGE_INTEGER x;
        QueryPerformanceFrequency(&x);
        return x.QuadPart;
    }();
    return f;
}

// ---- Cue sounds: short tones built in memory as WAV, so no resource files are needed. -------------
std::vector<uint8_t> make_wav(const std::vector<std::pair<double, int>>& tones /* frequency Hz, length ms */) {
    constexpr int kRate = 22050;
    std::vector<int16_t> samples;
    for (const auto& [freq, ms] : tones) {
        const int n = kRate * ms / 1000;
        for (int i = 0; i < n; ++i) {
            const double t = double(i) / kRate;
            const double fade = std::min({1.0, double(i) / 200.0, double(n - i) / 200.0});  // no clicks
            samples.push_back(int16_t(std::sin(2.0 * 3.14159265358979 * freq * t) * 9000.0 * fade));
        }
    }
    const uint32_t data_bytes = uint32_t(samples.size() * 2);
    std::vector<uint8_t> wav(44 + data_bytes);
    auto put32 = [&](size_t at, uint32_t v) { std::memcpy(&wav[at], &v, 4); };
    auto put16 = [&](size_t at, uint16_t v) { std::memcpy(&wav[at], &v, 2); };
    std::memcpy(&wav[0], "RIFF", 4);
    put32(4, 36 + data_bytes);
    std::memcpy(&wav[8], "WAVEfmt ", 8);
    put32(16, 16);
    put16(20, 1);  // PCM
    put16(22, 1);  // mono
    put32(24, kRate);
    put32(28, kRate * 2);
    put16(32, 2);
    put16(34, 16);
    std::memcpy(&wav[36], "data", 4);
    put32(40, data_bytes);
    std::memcpy(&wav[44], samples.data(), data_bytes);
    return wav;
}

}  // namespace

LRESULT CALLBACK low_level_proc(int code, WPARAM wparam, LPARAM lparam) {
    Hotkeys* self = g_hook_owner;
    if (code == HC_ACTION && self) {
        const auto* k = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lparam);
        if (k->vkCode == self->key_.vk) {
            if (wparam == WM_KEYUP || wparam == WM_SYSKEYUP) {
                self->key_down_ = false;
            } else if ((wparam == WM_KEYDOWN || wparam == WM_SYSKEYDOWN) && !self->key_down_) {
                auto held = [](int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; };
                const unsigned want = self->key_.modifiers;
                const bool ok = ((want & MOD_CONTROL) != 0) == held(VK_CONTROL) && ((want & MOD_ALT) != 0) == held(VK_MENU) &&
                                ((want & MOD_SHIFT) != 0) == held(VK_SHIFT) && ((want & MOD_WIN) != 0) == (held(VK_LWIN) || held(VK_RWIN));
                if (ok) {
                    self->key_down_ = true;
                    self->on_press();
                }
            }
        }
    }
    return CallNextHookEx(nullptr, code, wparam, lparam);
}

Hotkeys::~Hotkeys() { stop(); }

bool Hotkeys::start(const Hotkey& key, std::string* error) {
    if (thread_.joinable()) return true;
    key_ = key;
    event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    ready_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    thread_ = std::thread([this] { run(); });
    WaitForSingleObject(ready_, 5000);
    if (!start_ok_) {
        *error = start_error_;
        stop();
        return false;
    }
    return true;
}

void Hotkeys::stop() {
    if (thread_.joinable()) {
        PostThreadMessageW(thread_id_, WM_QUIT, 0, 0);
        thread_.join();
    }
    if (event_) CloseHandle(event_);
    if (ready_) CloseHandle(ready_);
    event_ = ready_ = nullptr;
}

void Hotkeys::on_press() {
    const int64_t now = qpc_now();
    if (last_press_qpc_ && (now - last_press_qpc_) * 1000 < int64_t(kDebounceMs) * qpc_freq()) {
        logging::event(Ev::HotkeyIgnored, "{} pressed again within {} ms", hotkey_name(key_), kDebounceMs);
        return;
    }
    last_press_qpc_ = now;
    pending_.fetch_add(1);
    SetEvent(event_);
}

void Hotkeys::run() {
    thread_id_ = GetCurrentThreadId();
    MSG msg;
    PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);  // create this thread's message queue

    if (RegisterHotKey(nullptr, kHotkeyId, key_.modifiers | MOD_NOREPEAT, key_.vk)) {
        start_ok_ = true;
    } else {
        const DWORD err = GetLastError();
        g_hook_owner = this;
        hook_ = SetWindowsHookExW(WH_KEYBOARD_LL, low_level_proc, GetModuleHandleW(nullptr), 0);
        if (hook_) {
            hook_mode_ = true;
            start_ok_ = true;
            logging::event(Ev::HotkeyRegistrationFailed, "{} is taken ({}); using a keyboard hook", hotkey_name(key_), win32_error_text(err));
        } else {
            start_error_ = hotkey_name(key_) + " can't be registered (" + win32_error_text(err) + ") and the keyboard hook failed too";
        }
    }
    SetEvent(ready_);
    if (!start_ok_) return;

    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (msg.message == WM_HOTKEY && msg.wParam == kHotkeyId) on_press();
    }
    if (hook_) UnhookWindowsHookEx(hook_);
    else UnregisterHotKey(nullptr, kHotkeyId);
    hook_ = nullptr;
    g_hook_owner = nullptr;
}

void Hotkeys::play(Cue cue) {
    // The sound data must outlive an asynchronous PlaySound: keep one buffer per cue for the process.
    static const std::vector<uint8_t> start = make_wav({{660, 70}, {880, 110}});
    static const std::vector<uint8_t> stop = make_wav({{880, 70}, {587, 110}});
    static const std::vector<uint8_t> error = make_wav({{220, 140}, {185, 220}});
    const std::vector<uint8_t>& wav = cue == Cue::Start ? start : cue == Cue::Stop ? stop : error;
    if (!PlaySoundW(reinterpret_cast<LPCWSTR>(wav.data()), nullptr, SND_MEMORY | SND_ASYNC | SND_NODEFAULT))
        logging::get(Subsystem::Cli).warn("the start/stop sound could not be played (no audio device?)");
}

}  // namespace rec
