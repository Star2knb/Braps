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
        // CPU priority of the host's encoder threads: a busy game can otherwise starve them for 100+ ms and
        // the recording loses frames. above-normal: they take ~25% of a core and win it back from the game.
        std::string encoder_priority = "above-normal";  // normal | above-normal
        std::string out_dir = "%USERPROFILE%\\Videos\\rec";
        int split_gb = 0;  // 0 = automatic (FAT32 only)
        int queue_mb = 256;
        int staging_slots = 6;  // the GPU finishes a copy about 4 Presents late (deep frame queue)
        int frame_slots = 16;  // frames the host can fall behind by (16 = 267 ms at 60 fps); 1.4 MB each at 720p
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
    // Fault injection (recorder plan §14.3): command-line only (--debug-*), never read from or written to rec.toml.
    struct Debug {
        double throttle_disk_mb_s = 0;  // the writer behaves like a disk of this speed
        int write_stall_ms = 0;         // every write_stall_every_s seconds one write takes this much longer
        int write_stall_every_s = 0;
        int encoder_delay_ms = 0;       // every frame takes this much longer to encode
        int drop_readback = 0;          // the hook loses one finished read-back in this many
        bool hook_throw = false;        // the hook throws inside its guard
        bool fill_disk = false;         // free space shrinks by 1 GB per second
        bool device_removed = false;    // the backend behaves as if the device was removed, once
        std::string kiero_fail;         // d3d11 | dxgi | opengl: that API's address lookup fails
        bool no_rate_control = false;   // switch the rate controller off (diagnosis, and tests that feed the ring in bursts)
        bool any() const {
            return no_rate_control || throttle_disk_mb_s > 0 || write_stall_ms > 0 || encoder_delay_ms > 0 || drop_readback > 0 || hook_throw || fill_disk ||
                   device_removed || !kiero_fail.empty();
        }
    } debug;
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
