// What a recording watches besides its own frames (recorder plan §10.4): free space on the output drive
// (W4103, E4104 which stops the recording), the power source (W6101) and the machine's CPU (W6102). Called once a
// second with a sample; `sample()` reads the real values, `evaluate()` decides and logs (so it can be tested).
#pragma once

#include <windows.h>

#include <cstdint>
#include <filesystem>
#include <string>

namespace rec {

struct ResourceSample {
    double free_gb = -1;          // on the output drive, -1 = unknown
    int on_ac = -1;               // 1 AC, 0 battery, -1 unknown
    bool battery_saver = false;
    double system_cpu_pct = -1;   // of all logical CPUs; -1 = no figure yet
    double game_cpu_pct = -1;     // the game's share of the same
    double host_cpu_pct = -1;     // rec's share
};

struct ResourceLimits {
    double low_space_gb = 5;
    double critical_space_gb = 1;
    double high_cpu_pct = 95;
    int high_cpu_seconds = 3;
};

class ResourceMonitor {
public:
    ResourceMonitor() = default;
    ~ResourceMonitor();
    ResourceMonitor(const ResourceMonitor&) = delete;
    ResourceMonitor& operator=(const ResourceMonitor&) = delete;

    // `fill_disk`: --debug-fill-disk, free space is reported as 8 GB minus 1 GB per second since start().
    void start(uint32_t game_pid, const std::filesystem::path& folder, const ResourceLimits& limits, bool fill_disk);
    // Reads free space, power state and CPU use now. Call about once a second; CPU figures are over the time since the last call.
    ResourceSample sample();
    // Logs W4103 / E4104 / W6101 / W6102 as the sample deserves (each at most once per condition, W6102 every 10 s while it lasts).
    // True once free space has fallen below the critical limit: the recording has to stop.
    bool evaluate(const ResourceSample& s);

    bool critical_space() const { return critical_; }
    const ResourceSample& last() const { return last_; }

private:
    ResourceLimits limits_;
    std::filesystem::path folder_;
    bool fill_disk_ = false;
    int64_t start_qpc_ = 0;
    HANDLE game_ = nullptr;
    uint32_t cpus_ = 1;
    // CPU accounting.
    uint64_t prev_idle_ = 0, prev_kernel_ = 0, prev_user_ = 0;
    uint64_t prev_game_ = 0, prev_host_ = 0;
    int64_t prev_wall_ = 0;
    bool have_prev_ = false;
    // Conditions already reported.
    bool low_reported_ = false, critical_ = false;
    int on_ac_ = -1;
    bool battery_saver_ = false;
    int hot_seconds_ = 0;
    int64_t last_cpu_log_s_ = -100;
    int64_t evaluated_ = 0;
    ResourceSample last_;
};

}  // namespace rec
