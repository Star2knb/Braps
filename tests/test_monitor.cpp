// Resource monitoring (recorder plan §10.4: W4103, E4104, W6101, W6102) and the disk benchmark with its cache (§9, §11.4).
#include <windows.h>

#include "rec/diskbench.h"
#include "rec/log.h"
#include "rec/monitor.h"
#include "test_util.h"
#include "testfw.h"

using namespace rec;

namespace {

uint64_t count(Ev e) { return logging::event_count(e); }

ResourceSample space(double gb) {
    ResourceSample s;
    s.free_gb = gb;
    return s;
}

}  // namespace

TEST_CASE("monitor: low space warns once, critical space stops the recording") {
    ResourceMonitor m;
    m.start(GetCurrentProcessId(), std::filesystem::temp_directory_path(), ResourceLimits{}, false);
    const uint64_t low0 = count(Ev::LowDiskSpace), crit0 = count(Ev::CriticalDiskSpace);
    CHECK(!m.evaluate(space(50)));
    CHECK(count(Ev::LowDiskSpace) == low0);
    CHECK(!m.evaluate(space(4.9)));
    CHECK(!m.evaluate(space(4.5)));
    CHECK(count(Ev::LowDiskSpace) == low0 + 1);  // once, not every second
    CHECK(!m.critical_space());
    CHECK(m.evaluate(space(0.9)));
    CHECK(m.evaluate(space(0.8)));
    CHECK(count(Ev::CriticalDiskSpace) == crit0 + 1);
    CHECK(m.critical_space());
}

TEST_CASE("monitor: a change of power source is reported, the first reading is not") {
    ResourceMonitor m;
    m.start(GetCurrentProcessId(), std::filesystem::temp_directory_path(), ResourceLimits{}, false);
    const uint64_t p0 = count(Ev::PowerStateChange);
    ResourceSample s;
    s.on_ac = 1;
    m.evaluate(s);
    m.evaluate(s);
    CHECK(count(Ev::PowerStateChange) == p0);
    s.on_ac = 0;
    m.evaluate(s);
    CHECK(count(Ev::PowerStateChange) == p0 + 1);
    s.battery_saver = true;
    m.evaluate(s);
    CHECK(count(Ev::PowerStateChange) == p0 + 2);
    m.evaluate(s);
    CHECK(count(Ev::PowerStateChange) == p0 + 2);
}

TEST_CASE("monitor: the machine at over 95% CPU for 3 s is reported, once per 10 s") {
    ResourceMonitor m;
    m.start(GetCurrentProcessId(), std::filesystem::temp_directory_path(), ResourceLimits{}, false);
    const uint64_t c0 = count(Ev::HighSystemCpu);
    ResourceSample s;
    s.system_cpu_pct = 99;
    s.game_cpu_pct = 80;
    s.host_cpu_pct = 12;
    m.evaluate(s);
    m.evaluate(s);
    CHECK(count(Ev::HighSystemCpu) == c0);  // 2 s
    m.evaluate(s);
    CHECK(count(Ev::HighSystemCpu) == c0 + 1);
    for (int i = 0; i < 5; ++i) m.evaluate(s);
    CHECK(count(Ev::HighSystemCpu) == c0 + 1);  // still the same stretch
    s.system_cpu_pct = 50;
    m.evaluate(s);
    s.system_cpu_pct = 99;
    for (int i = 0; i < 3; ++i) m.evaluate(s);  // a new stretch, but under 10 s since the last message
    CHECK(count(Ev::HighSystemCpu) == c0 + 1);
}

TEST_CASE("monitor: --debug-fill-disk shrinks the free space by a gigabyte a second") {
    ResourceMonitor m;
    m.start(GetCurrentProcessId(), std::filesystem::temp_directory_path(), ResourceLimits{}, true);
    const ResourceSample a = m.sample();
    CHECK(a.free_gb <= 8.0 && a.free_gb > 7.0);
    Sleep(1500);
    const ResourceSample b = m.sample();
    CHECK(b.free_gb < a.free_gb - 1.0);
    // The real figures are there too: a sample has CPU shares after the second reading.
    CHECK(b.system_cpu_pct >= 0 && b.system_cpu_pct <= 100.5);
    CHECK(b.host_cpu_pct >= 0);
}

TEST_CASE("disk bench: sizes, a small run, the cache and the startup check") {
    uint64_t bytes = 0;
    CHECK(parse_bench_size("2GB", &bytes) && bytes == 2ull << 30);
    CHECK(parse_bench_size("500 MB", &bytes) && bytes == 500ull << 20);
    CHECK(parse_bench_size("1.5gb", &bytes) && bytes == uint64_t(1.5 * 1073741824.0));
    CHECK(parse_bench_size("64", &bytes) && bytes == 64ull << 20);
    CHECK(!parse_bench_size("", &bytes));
    CHECK(!parse_bench_size("fast", &bytes));
    CHECK(!parse_bench_size("1MB", &bytes));  // under the 16 MB floor
    CHECK(!parse_bench_size("5XB", &bytes));

    // The cache lives under REC_DATA_DIR in tests: the user's own cache is not touched.
    const rt::TempDir data("bench_data");
    const rt::TempDir folder("bench_folder");
    SetEnvironmentVariableW(L"REC_DATA_DIR", data.path().c_str());
    CHECK(disk_bench_cache_path().parent_path() == data.path());

    DiskBenchResult none;
    CHECK(!load_cached_bench(folder.path(), &none));
    CHECK(disk_may_be_too_slow(folder.path(), 1000.0).empty());  // nothing cached: nothing to say

    DiskBenchResult r;
    double last_progress = 0;
    REQUIRE(run_disk_bench(folder.path(), 32ull << 20, &r, [&](double f) { last_progress = f; }));
    CHECK(r.ok && r.bytes == 32ull << 20 && r.writes == 4);
    CHECK(r.sustained_mb_s > 5.0);
    CHECK(r.p99_latency_ms > 0 && r.max_latency_ms >= r.p99_latency_ms);
    CHECK(last_progress == 1.0);
    CHECK(!r.volume.empty() && !r.when.empty());
    const bool left_over = std::filesystem::directory_iterator(folder.path()) != std::filesystem::directory_iterator();
    CHECK(!left_over);  // the temporary file is gone

    std::string error;
    REQUIRE(store_bench(folder.path(), r, &error));
    DiskBenchResult back;
    REQUIRE(load_cached_bench(folder.path(), &back));
    CHECK(std::abs(back.sustained_mb_s - r.sustained_mb_s) < 0.06);
    CHECK(back.volume == r.volume);

    CHECK(disk_may_be_too_slow(folder.path(), r.sustained_mb_s / 2).empty());       // 2x the need: fine
    CHECK(!disk_may_be_too_slow(folder.path(), r.sustained_mb_s / 1.1).empty());    // only 1.1x: W4001
    CHECK(!disk_may_be_too_slow(folder.path(), r.sustained_mb_s * 3).empty());

    SetEnvironmentVariableW(L"REC_DATA_DIR", nullptr);
}
