// `rec bench-disk` (recorder plan §11.4) and the startup check that uses its result (§9).
//
// The benchmark writes with the recorder's own writer (DiskFile: unbuffered, overlapped, 8 MB buffers), so it
// measures what a recording will get. Results are cached per volume, because a drive's speed does not change
// between recordings: the cache lives in %LOCALAPPDATA%\rec\diskbench.toml, not in rec.toml (D-094).
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

namespace rec {

struct DiskBenchResult {
    bool ok = false;
    std::string error;
    uint64_t bytes = 0;
    double seconds = 0;
    double sustained_mb_s = 0;      // bytes / wall time, first write issued to last one finished
    double p99_latency_ms = 0;      // of one 8 MiB write
    double max_latency_ms = 0;
    double slowest_write_mb_s = 0;  // the same slowest write, as a speed
    uint32_t writes = 0;
    std::string volume;             // "C:\ (NTFS)"
    std::string when;               // local time, "2026-10-02 04:31"
};

// Writes `bytes` to a temporary file in `folder` (created if needed; the file is deleted afterwards).
// progress(fraction) is called now and then, from the calling thread.
bool run_disk_bench(const std::filesystem::path& folder, uint64_t bytes, DiskBenchResult* out, const std::function<void(double)>& progress = {});

// "2GB", "500MB", "1.5 GB", "4096" (MB) -> bytes. False if not understood.
bool parse_bench_size(const std::string& text, uint64_t* bytes);

// The cache, keyed by the volume's serial number.
std::filesystem::path disk_bench_cache_path();
bool load_cached_bench(const std::filesystem::path& folder, DiskBenchResult* out);
bool store_bench(const std::filesystem::path& folder, const DiskBenchResult& result, std::string* error);

// §9: the benchmark must be at least this many times the needed rate, or the startup check warns (W4001).
constexpr double kBenchHeadroom = 1.2;
// Empty if fine or unknown (no cached result); otherwise the text for W4001.
std::string disk_may_be_too_slow(const std::filesystem::path& folder, double required_mb_s);

}  // namespace rec
