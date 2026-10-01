// rec.toml (recorder plan §17). Defaults below are the plan's; the file only needs the keys a user
// changes. Recording options on the command line (§12.2) are applied through set_config_value, so
// both go through the same type and range checks.
#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace rec {

struct Config {
    struct Record {
        int fps = 60;
        std::string size = "1280x720";  // "WxH" or "native"
        bool lock = true;
        std::string encoder = "rcv";    // rcv | rcv-strict | hw
        std::string format = "yuv420";  // yuv420 | rgb
        std::string out_dir = "%USERPROFILE%\\Videos\\rec";
        int split_gb = 0;  // 0 = automatic (FAT32 only)
        int queue_mb = 256;
        int staging_slots = 3;
        int frame_slots = 8;
    } record;
    struct Hotkeys {
        std::string toggle = "F9";
        std::string marker_key;  // "" = off
        bool sound = true;
    } hotkeys;
    struct Audio {
        std::string source = "game";  // game | system | none
        std::string mic;              // device name, "" = off
    } audio;
    struct Rate {
        std::vector<int> levels{40, 60, 75, 90};  // packet-queue % thresholds (§9)
        int step_down_after_s = 2;
    } rate;
    struct Log {
        std::string level = "info";  // debug | info | warn | error
        int slow_write_ms = 50;
        int very_slow_write_ms = 250;
        double slow_hook_ms = 1.0;
        int low_space_gb = 5;
        int critical_space_gb = 1;
    } log;
    struct Safety {
        std::vector<std::string> anticheat_blocklist{"EasyAntiCheat", "BEService", "BEDaisy", "vgc", "vgk"};
    } safety;
};

// Reads `path` over the defaults. A missing file leaves the defaults. Unknown keys, values of the
// wrong type and out-of-range values are listed in *problems and the default is kept for them.
// Returns false (with the reason in *problems) only if the file exists but can't be read or parsed.
bool load_config(const std::filesystem::path& path, Config* cfg, std::vector<std::string>* problems);

// Writes every key, in the plan's order. Creates the folder if needed.
bool save_config(const std::filesystem::path& path, const Config& cfg, std::string* error);
std::string config_to_toml(const Config& cfg);

// Sets "section.key" from text as typed on a command line: "50", "true", "1920x1080",
// "[40, 60, 75, 90]", quotes optional for strings. Checks type and range; leaves cfg unchanged on error.
bool set_config_value(Config* cfg, std::string_view key, std::string_view value, std::string* error);

std::vector<std::string> config_keys();                  // "record.fps", ...
std::vector<std::string> validate_config(const Config& cfg);  // empty if every value is in range

}  // namespace rec
