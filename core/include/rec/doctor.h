// `rec doctor`: environment checks (recorder plan §12.3). Read-only: nothing is created or changed
// (the output folder is tested with a temporary file that deletes itself).
#pragma once

#include <string>
#include <vector>

#include "rec/config.h"

namespace rec {

enum class CheckStatus { Pass, Warn, Fail };

struct CheckResult {
    std::string name;    // "Windows", "CPU", ...
    CheckStatus status = CheckStatus::Pass;
    std::string detail;  // what was found
    std::string fix;     // one line, for WARN and FAIL
};

std::vector<CheckResult> run_doctor(const Config& cfg);

// Estimated write rate for a configuration, MB/s: simple scenes (Minecraft-like, with temporal
// skip) and detailed scenes (Warframe-like), from the codec's measured ratios (codec M9 report).
struct RateEstimate {
    double typical_mbps = 0, detailed_mbps = 0;
};
RateEstimate estimate_write_rate(int width, int height, int fps, bool rgb);

}  // namespace rec
