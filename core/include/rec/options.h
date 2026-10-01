// Parsing of user-facing values: output size and hotkeys (recorder plan §12.2, §13.1).
#pragma once

#include <string>
#include <string_view>

namespace rec {

// "WxH" (even, 16..8192) or "native" (the game's back-buffer size).
struct OutputSize {
    bool native = false;
    int width = 0, height = 0;
};
bool parse_size(std::string_view text, OutputSize* out);

// "F9", "Ctrl+Shift+F10", "Alt+R" ... Keys: F1-F24, A-Z, 0-9, Insert, Delete, Home, End, PageUp,
// PageDown, Pause, ScrollLock, PrintScreen. Modifiers: Ctrl, Alt, Shift, Win. Case-insensitive.
struct Hotkey {
    unsigned vk = 0;         // virtual-key code
    unsigned modifiers = 0;  // MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_WIN
};
bool parse_hotkey(std::string_view text, Hotkey* out);
std::string hotkey_name(const Hotkey& key);  // canonical spelling, e.g. "Ctrl+Shift+F10"

}  // namespace rec
