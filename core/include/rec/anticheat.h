// Anti-cheat safety block (recorder plan §4.2, X6): injecting into a game protected by anti-cheat can
// get the account banned, so rec refuses unless told otherwise with --force.
//
// Looks for known anti-cheat components in: the target's loaded modules, every running process, the
// loaded kernel drivers, and (before a launch, when the target has no modules yet) the file and folder
// names in the game's install folder. The list is the user's `safety.anticheat_blocklist` plus the
// built-in names below.
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace rec {

struct AnticheatFinding {
    std::string name;   // what was found, e.g. "EasyAntiCheat_x64.dll"
    std::string where;  // "loaded in the game", "running process", "kernel driver", "game folder"
};

// Names of well-known anti-cheat components, matched like the user's entries.
const std::vector<std::string>& builtin_anticheat_names();

// Matching rule (case-insensitive, extension ignored): a name matches an entry if they are equal, or
// if the entry is at least 6 characters long and occurs in the name at its start or after a
// non-alphanumeric character ("EasyAntiCheat_x64" matches "EasyAntiCheat"; "WindscribeService" does not
// match "BEService"). Short entries ("vgc", "vgk") must match exactly so ordinary names are not caught.
bool matches_anticheat(std::string_view name, const std::vector<std::string>& blocklist);

// Every finding. `pid` is the target (its modules are scanned) and `game_dir` its folder (scanned two
// levels deep, names only); either may be omitted.
std::vector<AnticheatFinding> scan_anticheat(const std::vector<std::string>& user_blocklist, std::optional<uint32_t> pid,
                                             const std::filesystem::path& game_dir);

// The names of the target's loaded modules that match (used again shortly after a launch, when the
// anti-cheat has had time to load).
std::vector<AnticheatFinding> scan_anticheat_modules(const std::vector<std::string>& user_blocklist, uint32_t pid);

}  // namespace rec
