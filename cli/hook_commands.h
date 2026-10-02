// rec list / launch / attach / detach (recorder plan §12.1): the commands that deal with a game.
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "rec/config.h"

namespace rec_cli {

struct HookCommandOptions {
    bool force = false;   // --force: go ahead although anti-cheat was found
    int duration_s = 0;   // --duration: detach and exit after this many seconds
    int record_for_s = 0;           // --record-for: record this long as soon as the game presents, then exit (scripts)
    std::string save_frame;         // --save-frame: write one captured frame as a PNG (test aid)
    uint64_t save_frame_index = 30; // ... frame number (0-based)
};

int cmd_list();
int cmd_launch(const rec::Config& cfg, const std::string& exe, const std::vector<std::string>& game_args,
               const HookCommandOptions& options);
int cmd_attach(const rec::Config& cfg, std::optional<unsigned> pid, const std::optional<std::string>& name,
               const HookCommandOptions& options);
int cmd_detach(std::optional<unsigned> pid, const std::optional<std::string>& name);

}  // namespace rec_cli
