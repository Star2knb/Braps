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

// Write rate for a configuration, MB/s. `required` is the plan's startup-check estimate (§9:
// raw rate / 2.49, the FRAPS ratio), the figure a disk must sustain; `typical` uses the codec's
// measured whole-file ratio on Minecraft (5.7:1 YUV, 7.4:1 RGB; codec M9 report).
struct RateEstimate {
    double required_mbps = 0, typical_mbps = 0;
};
RateEstimate estimate_write_rate(int width, int height, int fps, bool rgb);

}  // namespace rec
