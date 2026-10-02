#include "file_commands.h"

#include <windows.h>

#include <cstdio>

#include "rec/avi_tools.h"
#include "rec/diskbench.h"
#include "rec/doctor.h"
#include "rec/options.h"
#include "rec/log.h"
#include "rec/paths.h"

namespace rec_cli {
namespace {

int fail(const std::string& message) {
    std::fprintf(stderr, "%s\n", message.c_str());
    rec::logging::get(rec::Subsystem::Cli).warn("{}", message);
    return 1;
}

void show_progress(uint64_t done, uint64_t total) {
    if (total) std::fprintf(stderr, "\r  %llu / %llu frames", (unsigned long long)done, (unsigned long long)total);
}

}  // namespace

int cmd_bench_disk(const rec::Config& cfg, const std::string& path, const std::string& size) {
    uint64_t bytes = 0;
    if (!rec::parse_bench_size(size, &bytes)) return fail("rec bench-disk: --size must look like 2GB or 500MB (16 MB to 1 TB)");
    const std::filesystem::path folder = path.empty() ? rec::expand_env(cfg.record.out_dir) : std::filesystem::path(rec::from_utf8(path));
    std::printf("rec bench-disk: writing %.1f GB to %s with the recorder's writer (unbuffered, 8 MiB writes)\n", double(bytes) / 1073741824.0,
                rec::to_utf8(folder.wstring()).c_str());
    int last = -1;
    rec::DiskBenchResult r;
    const bool ok = rec::run_disk_bench(folder, bytes, &r, [&](double f) {
        const int pct = int(f * 100);
        if (pct / 10 != last / 10) {
            std::fprintf(stderr, "\r  %d%%", pct);
            last = pct;
        }
    });
    std::fprintf(stderr, "\r      \r");
    if (!ok) return fail("rec bench-disk: " + r.error);
    std::printf("  %s: wrote %.1f GB in %.1f s (%u writes)\n", r.volume.c_str(), double(r.bytes) / 1073741824.0, r.seconds, r.writes);
    std::printf("  sustained %.0f MB/s; one 8 MiB write took %.0f ms at p99 and %.0f ms at worst (%.0f MB/s)\n", r.sustained_mb_s, r.p99_latency_ms,
                r.max_latency_ms, r.slowest_write_mb_s);

    rec::OutputSize os;
    rec::parse_size(cfg.record.size, &os);
    if (os.native || os.width <= 0) {
        os.width = 1920;  // a typical desktop; the estimate only needs the order of magnitude
        os.height = 1080;
    }
    const rec::RateEstimate need = rec::estimate_write_rate(os.width, os.height, cfg.record.fps, cfg.record.format == "rgb");
    std::printf("  a lossless %dx%d@%d recording needs about %.0f MB/s at worst (plan estimate) and %.0f MB/s typically: %.1fx headroom over the worst case\n",
                os.width, os.height, cfg.record.fps, need.required_mbps, need.typical_mbps, r.sustained_mb_s / (need.required_mbps > 0 ? need.required_mbps : 1));
    if (r.sustained_mb_s < rec::kBenchHeadroom * need.required_mbps)
        std::printf("  WARNING: that is under %.1fx the worst case; busy scenes may fill the packet queue. Use a faster drive, --encoder hw, or a smaller size / lower frame rate.\n",
                    rec::kBenchHeadroom);
    std::string error;
    if (rec::store_bench(folder, r, &error))
        std::printf("  saved for the startup check (%s)\n", rec::to_utf8(rec::disk_bench_cache_path().wstring()).c_str());
    else
        std::fprintf(stderr, "  could not save the result: %s\n", error.c_str());
    rec::logging::get(rec::Subsystem::Disk).info("bench-disk {}: {:.0f} MB/s sustained, p99 {:.0f} ms, worst {:.0f} ms", r.volume, r.sustained_mb_s,
                                                 r.p99_latency_ms, r.max_latency_ms);
    return 0;
}

int cmd_verify(const std::string& file, bool testapp, const std::string& source) {
    const std::filesystem::path path = rec::from_utf8(file);
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) return fail("rec verify: can't find " + file);
    rec::VerifyOptions options;
    options.testapp = testapp;
    if (!source.empty()) {
        unsigned w = 0, h = 0;
        if (sscanf_s(source.c_str(), "%ux%u", &w, &h) != 2 || !w || !h) return fail("rec verify: --source must look like 1366x745");
        options.source_w = w;
        options.source_h = h;
    }
    const rec::VerifyReport report = rec::verify_avi(path, options, show_progress);
    std::fprintf(stderr, "\r%40s\r", "");
    std::printf("%s", report.text.c_str());
    rec::logging::get(rec::Subsystem::Cli).info("rec verify {}: {}", file, report.ok ? "PASS" : "FAIL");
    return report.ok ? 0 : 1;
}

int cmd_convert(const std::string& file, const std::string& container, int crf, const std::string& out) {
    const std::filesystem::path path = rec::from_utf8(file);
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) return fail("rec convert: can't find " + file);
    rec::ConvertOptions options;
    options.container = container;
    options.crf = crf;
    if (!out.empty()) options.output = rec::from_utf8(out);
    std::string message;
    const bool ok = rec::convert_avi(path, options, &message, show_progress);
    std::fprintf(stderr, "\r%40s\r", "");
    if (!ok) return fail("rec convert: " + message);
    std::printf("%s\n", message.c_str());
    rec::logging::get(rec::Subsystem::Cli).info("rec convert: {}", message);
    return 0;
}

}  // namespace rec_cli
