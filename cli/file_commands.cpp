#include "file_commands.h"

#include <windows.h>

#include <cstdio>

#include "rec/avi_tools.h"
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
