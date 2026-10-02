#include "rec/diskbench.h"

#include <windows.h>

#include <toml.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <vector>

#include "rec/disk_file.h"
#include "rec/paths.h"

namespace rec {
namespace {

int64_t qpc_now() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

double seconds_between(int64_t a, int64_t b) {
    static const double freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return double(f.QuadPart);
    }();
    return double(b - a) / freq;
}

std::string fmt(const char* f, ...) {
    char buf[512];
    va_list args;
    va_start(args, f);
    std::vsnprintf(buf, sizeof(buf), f, args);
    va_end(args);
    return buf;
}

// Serial number, file system and root of the volume `folder` is on.
bool volume_of(const std::filesystem::path& folder, std::string* key, std::string* description) {
    std::error_code ec;
    std::filesystem::path full = std::filesystem::absolute(folder, ec);
    wchar_t root[MAX_PATH] = {};
    if (!GetVolumePathNameW(full.c_str(), root, MAX_PATH)) return false;
    DWORD serial = 0;
    wchar_t fs[MAX_PATH] = {};
    if (!GetVolumeInformationW(root, nullptr, 0, &serial, nullptr, nullptr, fs, MAX_PATH)) return false;
    if (key) *key = fmt("%08lX", static_cast<unsigned long>(serial));
    if (description) *description = to_utf8(root) + " (" + to_utf8(fs) + ")";
    return true;
}

std::string local_time_text() {
    SYSTEMTIME t;
    GetLocalTime(&t);
    return fmt("%04d-%02d-%02d %02d:%02d", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute);
}

}  // namespace

bool parse_bench_size(const std::string& text, uint64_t* bytes) {
    std::string t;
    for (char c : text)
        if (!std::isspace(static_cast<unsigned char>(c))) t += char(std::toupper(static_cast<unsigned char>(c)));
    if (t.empty()) return false;
    char* end = nullptr;
    const double value = std::strtod(t.c_str(), &end);
    if (end == t.c_str() || !(value > 0)) return false;
    const std::string unit = end;
    double scale = 1048576.0;  // a bare number is MB
    if (unit == "GB" || unit == "G" || unit == "GIB") scale = 1073741824.0;
    else if (unit == "MB" || unit == "M" || unit == "MIB" || unit.empty()) scale = 1048576.0;
    else if (unit == "KB" || unit == "K" || unit == "KIB") scale = 1024.0;
    else return false;
    const double total = value * scale;
    if (total < 16.0 * 1048576.0 || total > 1024.0 * 1073741824.0) return false;
    *bytes = uint64_t(total);
    return true;
}

bool run_disk_bench(const std::filesystem::path& folder, uint64_t bytes, DiskBenchResult* out, const std::function<void(double)>& progress) {
    DiskBenchResult r;
    std::error_code ec;
    std::filesystem::create_directories(folder, ec);
    if (ec) {
        r.error = "can't create " + to_utf8(folder.wstring()) + ": " + ec.message();
        *out = r;
        return false;
    }
    volume_of(folder, nullptr, &r.volume);
    const std::filesystem::path file = folder / (L"rec_bench_" + std::to_wstring(GetCurrentProcessId()) + L".tmp");

    constexpr size_t kBuffer = 8u << 20;
    DiskFile f;
    std::vector<double> latencies;
    f.on_write = [&](uint64_t, uint32_t, double ms) { latencies.push_back(ms); };
    f.set_thresholds(1e9, 1e9);
    std::string error;
    if (!f.open(file, kBuffer, &error)) {
        r.error = error;
        *out = r;
        return false;
    }
    // Data a disk or a file system cannot squeeze: the recorder's data is compressed already.
    std::vector<uint8_t> block(kBuffer);
    uint64_t x = 0x9E3779B97F4A7C15ull ^ uint64_t(qpc_now());
    for (size_t i = 0; i < block.size(); i += 8) {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        std::memcpy(block.data() + i, &x, 8);
    }

    const uint64_t blocks = (std::max)(uint64_t(2), (bytes + kBuffer - 1) / kBuffer);
    const int64_t t0 = qpc_now();
    bool ok = true;
    for (uint64_t i = 0; i < blocks && ok; ++i) {
        ok = f.append(block.data(), block.size());
        if (progress && (i % 8 == 0 || i + 1 == blocks)) progress(double(i + 1) / double(blocks));
    }
    ok = f.close(&error) && ok;  // waits for the last write
    const int64_t t1 = qpc_now();
    f.on_write = nullptr;
    std::filesystem::remove(file, ec);
    if (!ok) {
        r.error = error.empty() ? f.error() : error;
        *out = r;
        return false;
    }
    r.bytes = blocks * kBuffer;
    r.seconds = seconds_between(t0, t1);
    r.sustained_mb_s = r.seconds > 0 ? double(r.bytes) / 1048576.0 / r.seconds : 0;
    r.writes = uint32_t(latencies.size());
    if (!latencies.empty()) {
        std::sort(latencies.begin(), latencies.end());
        r.p99_latency_ms = latencies[(std::min)(latencies.size() - 1, size_t(double(latencies.size()) * 0.99))];
        r.max_latency_ms = latencies.back();
        r.slowest_write_mb_s = r.max_latency_ms > 0 ? double(kBuffer) / 1048576.0 / (r.max_latency_ms / 1000.0) : 0;
    }
    r.when = local_time_text();
    r.ok = true;
    *out = r;
    return true;
}

std::filesystem::path disk_bench_cache_path() { return app_data_dir() / L"diskbench.toml"; }

bool load_cached_bench(const std::filesystem::path& folder, DiskBenchResult* out) {
    std::string key;
    if (!volume_of(folder, &key, nullptr)) return false;
    try {
        const toml::table t = toml::parse_file(disk_bench_cache_path().wstring());
        const toml::table* v = t[key].as_table();
        if (!v) return false;
        DiskBenchResult r;
        r.sustained_mb_s = v->at("sustained_mb_s").value_or(0.0);
        r.p99_latency_ms = v->at("p99_latency_ms").value_or(0.0);
        r.max_latency_ms = v->at("max_latency_ms").value_or(0.0);
        r.volume = v->at("volume").value_or(std::string());
        r.when = v->at("when").value_or(std::string());
        r.ok = r.sustained_mb_s > 0;
        if (!r.ok) return false;
        *out = r;
        return true;
    } catch (const toml::parse_error&) {
        return false;
    }
}

bool store_bench(const std::filesystem::path& folder, const DiskBenchResult& result, std::string* error) {
    std::string key, description;
    if (!volume_of(folder, &key, &description)) {
        *error = "can't identify the volume of " + to_utf8(folder.wstring());
        return false;
    }
    // Keep the other volumes' entries.
    toml::table all;
    try {
        all = toml::parse_file(disk_bench_cache_path().wstring());
    } catch (const toml::parse_error&) {
    }
    toml::table entry;
    entry.insert("sustained_mb_s", std::round(result.sustained_mb_s * 10.0) / 10.0);
    entry.insert("p99_latency_ms", std::round(result.p99_latency_ms * 10.0) / 10.0);
    entry.insert("max_latency_ms", std::round(result.max_latency_ms * 10.0) / 10.0);
    entry.insert("volume", description);
    entry.insert("when", result.when);
    all.insert_or_assign(key, std::move(entry));

    std::error_code ec;
    std::filesystem::create_directories(disk_bench_cache_path().parent_path(), ec);
    FILE* f = nullptr;
    if (_wfopen_s(&f, disk_bench_cache_path().c_str(), L"wb") != 0 || !f) {
        *error = "can't write " + to_utf8(disk_bench_cache_path().wstring());
        return false;
    }
    std::ostringstream text;
    text << "# Cached `rec bench-disk` results, one table per volume serial number.\n" << all;
    const std::string s = text.str();
    std::fwrite(s.data(), 1, s.size(), f);
    std::fclose(f);
    return true;
}

std::string disk_may_be_too_slow(const std::filesystem::path& folder, double required_mb_s) {
    DiskBenchResult r;
    if (!load_cached_bench(folder, &r)) return {};
    if (r.sustained_mb_s >= kBenchHeadroom * required_mb_s) return {};
    return fmt("%s measured %.0f MB/s on %s; this recording needs about %.0f MB/s (%.1fx headroom wanted): try --encoder hw, a smaller size or a lower frame rate",
               r.volume.empty() ? "the drive" : r.volume.c_str(), r.sustained_mb_s, r.when.c_str(), required_mb_s, kBenchHeadroom);
}

}  // namespace rec
