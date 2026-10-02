#include "rec/monitor.h"

#include <algorithm>

#include "rec/log.h"
#include "rec/paths.h"

namespace rec {
namespace {

int64_t qpc_now() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

int64_t qpc_freq() {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    return f.QuadPart;
}

uint64_t to_u64(const FILETIME& f) { return (uint64_t(f.dwHighDateTime) << 32) | f.dwLowDateTime; }

uint64_t process_cpu_100ns(HANDLE process) {
    FILETIME create, exit, kernel, user;
    if (!process || !GetProcessTimes(process, &create, &exit, &kernel, &user)) return 0;
    return to_u64(kernel) + to_u64(user);
}

}  // namespace

ResourceMonitor::~ResourceMonitor() {
    if (game_) CloseHandle(game_);
}

void ResourceMonitor::start(uint32_t game_pid, const std::filesystem::path& folder, const ResourceLimits& limits, bool fill_disk) {
    limits_ = limits;
    folder_ = folder;
    fill_disk_ = fill_disk;
    start_qpc_ = qpc_now();
    if (game_) CloseHandle(game_);
    game_ = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, game_pid);
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    cpus_ = (std::max)(1u, uint32_t(si.dwNumberOfProcessors));
    have_prev_ = false;
    low_reported_ = critical_ = false;
    on_ac_ = -1;
    battery_saver_ = false;
    hot_seconds_ = 0;
    last_cpu_log_s_ = -100;
    evaluated_ = 0;
    last_ = ResourceSample{};
}

ResourceSample ResourceMonitor::sample() {
    ResourceSample s;
    ULARGE_INTEGER free_bytes{};
    if (GetDiskFreeSpaceExW(folder_.c_str(), &free_bytes, nullptr, nullptr)) s.free_gb = double(free_bytes.QuadPart) / 1073741824.0;
    if (fill_disk_) {
        const double elapsed = double(qpc_now() - start_qpc_) / double(qpc_freq());
        const double fake = (std::max)(0.0, 8.0 - elapsed);
        s.free_gb = s.free_gb < 0 ? fake : (std::min)(s.free_gb, fake);
    }
    SYSTEM_POWER_STATUS p{};
    if (GetSystemPowerStatus(&p)) {
        s.on_ac = p.ACLineStatus == 1 ? 1 : p.ACLineStatus == 0 ? 0 : -1;
        s.battery_saver = p.SystemStatusFlag == 1;
    }
    FILETIME idle, kernel, user;
    const int64_t wall = qpc_now();
    if (GetSystemTimes(&idle, &kernel, &user)) {
        const uint64_t i = to_u64(idle), k = to_u64(kernel), u = to_u64(user);  // kernel includes idle
        const uint64_t game = process_cpu_100ns(game_), host = process_cpu_100ns(GetCurrentProcess());
        if (have_prev_) {
            const double total = double((k - prev_kernel_) + (u - prev_user_));
            if (total > 0) s.system_cpu_pct = 100.0 * (1.0 - double(i - prev_idle_) / total);
            const double wall_100ns = double(wall - prev_wall_) * 1e7 / double(qpc_freq()) * double(cpus_);
            if (wall_100ns > 0) {
                if (game_) s.game_cpu_pct = 100.0 * double(game - prev_game_) / wall_100ns;
                s.host_cpu_pct = 100.0 * double(host - prev_host_) / wall_100ns;
            }
        }
        prev_idle_ = i;
        prev_kernel_ = k;
        prev_user_ = u;
        prev_game_ = game;
        prev_host_ = host;
        prev_wall_ = wall;
        have_prev_ = true;
    }
    return s;
}

bool ResourceMonitor::evaluate(const ResourceSample& s) {
    last_ = s;
    ++evaluated_;
    if (s.free_gb >= 0) {
        if (s.free_gb < limits_.critical_space_gb) {
            if (!critical_) {
                critical_ = true;
                logging::event(Ev::CriticalDiskSpace, "{:.2f} GB free on the output drive (limit {} GB): stopping and closing the file", s.free_gb,
                               limits_.critical_space_gb);
            }
        } else if (s.free_gb < limits_.low_space_gb && !low_reported_) {
            low_reported_ = true;
            logging::event(Ev::LowDiskSpace, "{:.1f} GB free on the output drive (limit {} GB)", s.free_gb, limits_.low_space_gb);
        }
    }
    if (s.on_ac >= 0) {
        if (on_ac_ >= 0 && (s.on_ac != on_ac_ || s.battery_saver != battery_saver_))
            logging::event(Ev::PowerStateChange, "{} -> {}{}", on_ac_ ? "AC" : "battery", s.on_ac ? "AC" : "battery",
                           s.battery_saver ? " (battery saver on)" : "");
        on_ac_ = s.on_ac;
        battery_saver_ = s.battery_saver;
    }
    if (s.system_cpu_pct >= 0) {
        hot_seconds_ = s.system_cpu_pct > limits_.high_cpu_pct ? hot_seconds_ + 1 : 0;
        if (hot_seconds_ >= limits_.high_cpu_seconds && evaluated_ - last_cpu_log_s_ >= 10) {
            last_cpu_log_s_ = evaluated_;
            logging::event(Ev::HighSystemCpu, "{:.0f}% busy for {} s (game {:.0f}%, rec {:.0f}%)", s.system_cpu_pct, hot_seconds_,
                           s.game_cpu_pct < 0 ? 0.0 : s.game_cpu_pct, s.host_cpu_pct < 0 ? 0.0 : s.host_cpu_pct);
        }
    }
    return critical_;
}

}  // namespace rec
