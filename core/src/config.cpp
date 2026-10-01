#include "rec/config.h"

#include <cstdint>
#include <fstream>
#include <sstream>

#include <toml.hpp>

#include "rec/options.h"
#include "rec/paths.h"

namespace rec {
namespace {

// One entry per key: where it lives in Config and what type it has. Order = file order (§17).
enum class Kind { Int, Bool, Str, Double, IntList, StrList };
struct Field {
    const char* section;
    const char* key;
    Kind kind;
    void* (*get)(Config&);
};

#define REC_FIELD(sec, k, kind, member) \
    Field { sec, k, kind, [](Config& c) -> void* { return &c.member; } }

const Field kFields[] = {
    REC_FIELD("record", "fps", Kind::Int, record.fps),
    REC_FIELD("record", "size", Kind::Str, record.size),
    REC_FIELD("record", "lock", Kind::Bool, record.lock),
    REC_FIELD("record", "encoder", Kind::Str, record.encoder),
    REC_FIELD("record", "format", Kind::Str, record.format),
    REC_FIELD("record", "out_dir", Kind::Str, record.out_dir),
    REC_FIELD("record", "split_gb", Kind::Int, record.split_gb),
    REC_FIELD("record", "queue_mb", Kind::Int, record.queue_mb),
    REC_FIELD("record", "staging_slots", Kind::Int, record.staging_slots),
    REC_FIELD("record", "frame_slots", Kind::Int, record.frame_slots),
    REC_FIELD("hotkeys", "toggle", Kind::Str, hotkeys.toggle),
    REC_FIELD("hotkeys", "marker_key", Kind::Str, hotkeys.marker_key),
    REC_FIELD("hotkeys", "sound", Kind::Bool, hotkeys.sound),
    REC_FIELD("audio", "source", Kind::Str, audio.source),
    REC_FIELD("audio", "mic", Kind::Str, audio.mic),
    REC_FIELD("rate", "levels", Kind::IntList, rate.levels),
    REC_FIELD("rate", "step_down_after_s", Kind::Int, rate.step_down_after_s),
    REC_FIELD("log", "level", Kind::Str, log.level),
    REC_FIELD("log", "slow_write_ms", Kind::Int, log.slow_write_ms),
    REC_FIELD("log", "very_slow_write_ms", Kind::Int, log.very_slow_write_ms),
    REC_FIELD("log", "slow_hook_ms", Kind::Double, log.slow_hook_ms),
    REC_FIELD("log", "low_space_gb", Kind::Int, log.low_space_gb),
    REC_FIELD("log", "critical_space_gb", Kind::Int, log.critical_space_gb),
    REC_FIELD("safety", "anticheat_blocklist", Kind::StrList, safety.anticheat_blocklist),
};
#undef REC_FIELD

const char* kind_name(Kind k) {
    switch (k) {
    case Kind::Int: return "an integer";
    case Kind::Bool: return "true or false";
    case Kind::Str: return "a string";
    case Kind::Double: return "a number";
    case Kind::IntList: return "a list of integers";
    case Kind::StrList: return "a list of strings";
    }
    return "?";
}

std::string full_key(const Field& f) { return std::string(f.section) + "." + f.key; }

const Field* find_field(std::string_view key) {
    for (const Field& f : kFields)
        if (key == full_key(f)) return &f;
    return nullptr;
}

// Stores a TOML node into the field if it has the right type.
bool assign(const Field& f, Config& cfg, const toml::node& n) {
    void* p = f.get(cfg);
    switch (f.kind) {
    case Kind::Int:
        if (auto v = n.value_exact<int64_t>(); v && *v >= INT32_MIN && *v <= INT32_MAX) {
            *static_cast<int*>(p) = int(*v);
            return true;
        }
        return false;
    case Kind::Bool:
        if (auto v = n.value_exact<bool>()) {
            *static_cast<bool*>(p) = *v;
            return true;
        }
        return false;
    case Kind::Str:
        if (auto v = n.value_exact<std::string>()) {
            *static_cast<std::string*>(p) = *v;
            return true;
        }
        return false;
    case Kind::Double:
        if (auto v = n.value<double>(); v && (n.is_floating_point() || n.is_integer())) {
            *static_cast<double*>(p) = *v;
            return true;
        }
        return false;
    case Kind::IntList: {
        const toml::array* a = n.as_array();
        if (!a) return false;
        std::vector<int> out;
        for (const toml::node& e : *a) {
            auto v = e.value_exact<int64_t>();
            if (!v || *v < INT32_MIN || *v > INT32_MAX) return false;
            out.push_back(int(*v));
        }
        *static_cast<std::vector<int>*>(p) = std::move(out);
        return true;
    }
    case Kind::StrList: {
        const toml::array* a = n.as_array();
        if (!a) return false;
        std::vector<std::string> out;
        for (const toml::node& e : *a) {
            auto v = e.value_exact<std::string>();
            if (!v) return false;
            out.push_back(*v);
        }
        *static_cast<std::vector<std::string>*>(p) = std::move(out);
        return true;
    }
    }
    return false;
}

template <class T>
std::string toml_text(const T& v) {
    std::ostringstream os;
    os << toml::value<T>(v);
    return os.str();
}

std::string value_text(const Field& f, Config& cfg) {
    void* p = f.get(cfg);
    switch (f.kind) {
    case Kind::Int: return std::to_string(*static_cast<int*>(p));
    case Kind::Bool: return *static_cast<bool*>(p) ? "true" : "false";
    case Kind::Str: return toml_text(*static_cast<std::string*>(p));
    case Kind::Double: return toml_text(*static_cast<double*>(p));
    case Kind::IntList: {
        std::string s = "[";
        for (int v : *static_cast<std::vector<int>*>(p)) s += (s.size() > 1 ? ", " : "") + std::to_string(v);
        return s + "]";
    }
    case Kind::StrList: {
        std::string s = "[";
        for (const std::string& v : *static_cast<std::vector<std::string>*>(p))
            s += (s.size() > 1 ? ", " : "") + toml_text(v);
        return s + "]";
    }
    }
    return {};
}

std::string u8(const std::filesystem::path& p) { return to_utf8(p.wstring()); }

// The first problem in `after` that isn't in `before` ("" if none): what a change broke.
std::string new_problem(const std::vector<std::string>& before, const std::vector<std::string>& after) {
    for (const std::string& a : after) {
        bool seen = false;
        for (const std::string& b : before) seen = seen || a == b;
        if (!seen) return a;
    }
    return {};
}

bool one_of(const std::string& v, std::initializer_list<const char*> allowed) {
    for (const char* a : allowed)
        if (v == a) return true;
    return false;
}

}  // namespace

std::vector<std::string> config_keys() {
    std::vector<std::string> keys;
    for (const Field& f : kFields) keys.push_back(full_key(f));
    return keys;
}

std::vector<std::string> validate_config(const Config& c) {
    std::vector<std::string> e;
    auto range = [&](const char* key, double v, double lo, double hi) {
        if (v < lo || v > hi) {
            std::ostringstream os;
            os << key << " = " << v << " is out of range (" << lo << " to " << hi << ")";
            e.push_back(os.str());
        }
    };
    range("record.fps", c.record.fps, 1, 240);
    OutputSize size;
    if (!parse_size(c.record.size, &size))
        e.push_back("record.size = \"" + c.record.size + "\": use WxH with even sizes from 16 to 8192, or native");
    if (!one_of(c.record.encoder, {"rcv", "rcv-strict", "hw"}))
        e.push_back("record.encoder = \"" + c.record.encoder + "\": use rcv, rcv-strict or hw");
    if (!one_of(c.record.format, {"yuv420", "rgb"}))
        e.push_back("record.format = \"" + c.record.format + "\": use yuv420 or rgb");
    if (c.record.out_dir.empty()) e.push_back("record.out_dir is empty");
    range("record.split_gb", c.record.split_gb, 0, 1 << 20);
    range("record.queue_mb", c.record.queue_mb, 32, 4096);
    range("record.staging_slots", c.record.staging_slots, 2, 8);
    range("record.frame_slots", c.record.frame_slots, 2, 64);
    Hotkey toggle, marker;
    if (!parse_hotkey(c.hotkeys.toggle, &toggle))
        e.push_back("hotkeys.toggle = \"" + c.hotkeys.toggle + "\" is not a key (e.g. F9, Ctrl+Shift+R)");
    if (!c.hotkeys.marker_key.empty()) {
        if (!parse_hotkey(c.hotkeys.marker_key, &marker))
            e.push_back("hotkeys.marker_key = \"" + c.hotkeys.marker_key + "\" is not a key");
        else if (marker.vk == toggle.vk && marker.modifiers == toggle.modifiers)
            e.push_back("hotkeys.marker_key is the same key as hotkeys.toggle");
    }
    if (!one_of(c.audio.source, {"game", "system", "none"}))
        e.push_back("audio.source = \"" + c.audio.source + "\": use game, system or none");
    bool levels_ok = c.rate.levels.size() == 4;
    for (size_t i = 0; levels_ok && i < 4; ++i)
        levels_ok = c.rate.levels[i] >= 1 && c.rate.levels[i] <= 99 && (i == 0 || c.rate.levels[i] > c.rate.levels[i - 1]);
    if (!levels_ok) e.push_back("rate.levels needs four increasing percentages between 1 and 99");
    range("rate.step_down_after_s", c.rate.step_down_after_s, 1, 600);
    if (!one_of(c.log.level, {"debug", "info", "warn", "error"}))
        e.push_back("log.level = \"" + c.log.level + "\": use debug, info, warn or error");
    range("log.slow_write_ms", c.log.slow_write_ms, 1, 60000);
    if (c.log.very_slow_write_ms <= c.log.slow_write_ms)
        e.push_back("log.very_slow_write_ms must be greater than log.slow_write_ms");
    range("log.slow_hook_ms", c.log.slow_hook_ms, 0.01, 1000);
    range("log.critical_space_gb", c.log.critical_space_gb, 0, 1 << 20);
    if (c.log.low_space_gb < c.log.critical_space_gb)
        e.push_back("log.low_space_gb must be at least log.critical_space_gb");
    return e;
}

bool load_config(const std::filesystem::path& path, Config* cfg, std::vector<std::string>* problems) {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) return true;
    toml::table tbl;
    try {
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            problems->push_back("can't read " + u8(path));
            return false;
        }
        tbl = toml::parse(in, u8(path));
    } catch (const toml::parse_error& err) {
        std::ostringstream os;
        os << u8(path) << ": " << err.description() << " (line " << err.source().begin.line << ")";
        problems->push_back(os.str());
        return false;
    }
    Config loaded = *cfg;
    for (const auto& [sec_key, sec_node] : tbl) {
        const std::string sec(sec_key.str());
        const toml::table* sec_tbl = sec_node.as_table();
        if (!sec_tbl) {
            problems->push_back("unknown key " + sec + " (ignored)");
            continue;
        }
        for (const auto& [key, node] : *sec_tbl) {
            const std::string k = sec + "." + std::string(key.str());
            const Field* f = find_field(k);
            if (!f) {
                problems->push_back("unknown key " + k + " (ignored)");
                continue;
            }
            Config trial = loaded;
            if (!assign(*f, trial, node)) {
                problems->push_back(k + ": expected " + kind_name(f->kind) + " (default kept)");
                continue;
            }
            // Keep the default for a value that is out of range.
            const std::string broke = new_problem(validate_config(loaded), validate_config(trial));
            if (!broke.empty()) {
                problems->push_back(broke + " (default kept)");
                continue;
            }
            loaded = std::move(trial);
        }
    }
    *cfg = std::move(loaded);
    return true;
}

std::string config_to_toml(const Config& cfg) {
    Config c = cfg;
    std::string out = "# rec configuration (Recorder_Implementation_Planv2.md §17). Edit, or use: rec config set <key> <value>\n";
    const char* section = "";
    for (const Field& f : kFields) {
        if (std::string_view(section) != f.section) {
            section = f.section;
            out += std::string("\n[") + section + "]\n";
        }
        out += std::string(f.key) + " = " + value_text(f, c) + "\n";
    }
    return out;
}

bool save_config(const std::filesystem::path& path, const Config& cfg, std::string* error) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        *error = "can't write " + u8(path);
        return false;
    }
    out << config_to_toml(cfg);
    out.close();
    if (!out) {
        *error = "write failed: " + u8(path);
        return false;
    }
    return true;
}

bool set_config_value(Config* cfg, std::string_view key, std::string_view value, std::string* error) {
    const Field* f = find_field(key);
    if (!f) {
        *error = "unknown key " + std::string(key) + "; known keys: rec config show";
        return false;
    }
    // Parse the text as a TOML value; a bare word is taken as a string for string keys.
    toml::table t;
    bool parsed = false;
    try {
        t = toml::parse("v = " + std::string(value));
        parsed = true;
    } catch (const toml::parse_error&) {
    }
    if (!parsed && f->kind == Kind::Str) {
        t = toml::table{{"v", std::string(value)}};
        parsed = true;
    }
    Config trial = *cfg;
    if (!parsed || !assign(*f, trial, *t.get("v"))) {
        *error = std::string(key) + ": expected " + kind_name(f->kind) + ", got " + std::string(value);
        return false;
    }
    const std::string broke = new_problem(validate_config(*cfg), validate_config(trial));
    if (!broke.empty()) {
        *error = broke;
        return false;
    }
    *cfg = std::move(trial);
    return true;
}

}  // namespace rec
