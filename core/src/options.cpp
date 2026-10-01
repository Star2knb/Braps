#include "rec/options.h"

#include <windows.h>

#include <cctype>
#include <charconv>

namespace rec {
namespace {

bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
            return false;
    return true;
}

bool parse_int(std::string_view s, int* out) {
    if (s.empty()) return false;
    const auto r = std::from_chars(s.data(), s.data() + s.size(), *out);
    return r.ec == std::errc() && r.ptr == s.data() + s.size();
}

struct NamedKey {
    const char* name;
    unsigned vk;
};
constexpr NamedKey kNamedKeys[] = {
    {"Insert", VK_INSERT}, {"Delete", VK_DELETE},  {"Home", VK_HOME},          {"End", VK_END},
    {"PageUp", VK_PRIOR},  {"PageDown", VK_NEXT},  {"Pause", VK_PAUSE},        {"ScrollLock", VK_SCROLL},
    {"PrintScreen", VK_SNAPSHOT},
};

bool parse_key(std::string_view s, unsigned* vk) {
    if (s.size() == 1) {
        const char c = char(std::toupper(static_cast<unsigned char>(s[0])));
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
            *vk = unsigned(c);  // VK codes for letters and digits are their ASCII values
            return true;
        }
        return false;
    }
    if (s.size() >= 2 && (s[0] == 'F' || s[0] == 'f')) {
        int n = 0;
        if (parse_int(s.substr(1), &n) && n >= 1 && n <= 24) {
            *vk = unsigned(VK_F1 + n - 1);
            return true;
        }
        return false;
    }
    for (const NamedKey& k : kNamedKeys)
        if (iequals(s, k.name)) {
            *vk = k.vk;
            return true;
        }
    return false;
}

}  // namespace

bool parse_size(std::string_view text, OutputSize* out) {
    if (iequals(text, "native")) {
        *out = OutputSize{true, 0, 0};
        return true;
    }
    const size_t x = text.find_first_of("xX");
    int w = 0, h = 0;
    if (x == std::string_view::npos || !parse_int(text.substr(0, x), &w) || !parse_int(text.substr(x + 1), &h))
        return false;
    // YUV 4:2:0 needs even sizes; the codec allows 2..8192, a useful recording at least 16.
    if (w < 16 || h < 16 || w > 8192 || h > 8192 || (w & 1) || (h & 1)) return false;
    *out = OutputSize{false, w, h};
    return true;
}

bool parse_hotkey(std::string_view text, Hotkey* out) {
    Hotkey k;
    bool have_key = false;
    while (!text.empty()) {
        const size_t plus = text.find('+');
        const std::string_view part = text.substr(0, plus);
        if (part.empty() || have_key) return false;  // "Ctrl++F9", or something after the key
        if (iequals(part, "Ctrl") || iequals(part, "Control")) k.modifiers |= MOD_CONTROL;
        else if (iequals(part, "Alt")) k.modifiers |= MOD_ALT;
        else if (iequals(part, "Shift")) k.modifiers |= MOD_SHIFT;
        else if (iequals(part, "Win")) k.modifiers |= MOD_WIN;
        else if (parse_key(part, &k.vk)) have_key = true;
        else return false;
        if (plus == std::string_view::npos) break;
        text.remove_prefix(plus + 1);
        if (text.empty()) return false;  // trailing '+'
    }
    if (!have_key) return false;
    *out = k;
    return true;
}

std::string hotkey_name(const Hotkey& key) {
    std::string s;
    if (key.modifiers & MOD_CONTROL) s += "Ctrl+";
    if (key.modifiers & MOD_ALT) s += "Alt+";
    if (key.modifiers & MOD_SHIFT) s += "Shift+";
    if (key.modifiers & MOD_WIN) s += "Win+";
    if (key.vk >= VK_F1 && key.vk <= VK_F24) return s + "F" + std::to_string(key.vk - VK_F1 + 1);
    if ((key.vk >= 'A' && key.vk <= 'Z') || (key.vk >= '0' && key.vk <= '9')) return s + char(key.vk);
    for (const NamedKey& k : kNamedKeys)
        if (k.vk == key.vk) return s + k.name;
    return s + "?";
}

}  // namespace rec
