// Logging (recorder plan §10.1): line format, rolling app log, session files, event counts.
#include <regex>
#include <set>
#include <sstream>

#include "rec/doctor.h"
#include "rec/log.h"
#include "rec/paths.h"
#include "test_util.h"
#include "testfw.h"

using namespace rec;

namespace {

int count_lines(const std::string& text, const std::string& part) {
    int n = 0;
    std::istringstream in(text);
    for (std::string line; std::getline(in, line);)
        if (line.find(part) != std::string::npos) ++n;
    return n;
}

}  // namespace

TEST_CASE("logging: events before start are counted, not written") {
    const uint64_t before = logging::event_count(Ev::GpuBacklog);
    logging::event(Ev::GpuBacklog, "slot={}", 2);
    logging::get(Subsystem::Hook).warn("goes nowhere");
    CHECK(logging::event_count(Ev::GpuBacklog) == before + 1);
}

TEST_CASE("logging: line format, app log, session file") {
    const rt::TempDir dir("logging");
    logging::Options o;
    o.app_log_dir = dir.path() / "logs";
    o.console = false;
    std::string err;
    REQUIRE(logging::start(o, &err));

    const uint64_t slow_before = logging::event_count(Ev::SlowWrite);
    logging::event(Ev::SessionStart, "before the session");
    const auto session_log = dir.path() / "Game 2026-10-01 12-00-00-00.log";
    REQUIRE(logging::open_session(session_log, &err));
    logging::event(Ev::SlowWrite, "latency={}ms size={}MiB", 312, 8);
    logging::get(Subsystem::Encoder).info("free text");
    logging::get(Subsystem::Encoder).debug("hidden without --verbose");
    for (int i = 0; i < 2000; ++i) logging::get(Subsystem::Audio).info("burst {}", i);  // all must arrive
    logging::close_session();
    logging::event(Ev::SessionStats, "after the session");
    logging::stop();

    CHECK(logging::event_count(Ev::SlowWrite) == slow_before + 1);
    const std::string app = rt::read_file(o.app_log_dir / "rec.log");
    const std::string session = rt::read_file(session_log);
    const std::regex line(R"(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\.\d{3} \[WARN \] \[writer \] W4101 slow_write latency=312ms size=8MiB\r?)");
    bool found = false;
    std::istringstream in(app);
    for (std::string l; std::getline(in, l);) found = found || std::regex_match(l, line);
    CHECK(found);
    CHECK(count_lines(app, "[INFO ] [encoder] free text") == 1);
    CHECK(count_lines(app, "hidden without") == 0);
    CHECK(count_lines(app, "I6001 session_start before the session") == 1);
    CHECK(count_lines(app, "I6002 session_stats after the session") == 1);

    // The session file has exactly what was logged while it was open.
    CHECK(count_lines(session, "W4101 slow_write") == 1);
    CHECK(count_lines(session, "burst ") == 2000);
    CHECK(count_lines(session, "burst 1999") == 1);
    CHECK(count_lines(session, "before the session") == 0);
    CHECK(count_lines(session, "after the session") == 0);
}

TEST_CASE("logging: restart after stop, verbose writes DEBUG") {
    const rt::TempDir dir("logging2");
    logging::Options o;
    o.app_log_dir = dir.path();
    o.console = false;
    o.verbose = true;
    std::string err;
    REQUIRE(logging::start(o, &err));
    logging::get(Subsystem::System).debug("debug line {}", 7);
    logging::flush();
    CHECK(count_lines(rt::read_file(dir.path() / "rec.log"), "[DEBUG] [system ] debug line 7") == 1);
    logging::stop();
}

TEST_CASE("doctor: runs every check; output folder in a temp dir passes") {
    const rt::TempDir dir("doctor");
    Config c;
    c.record.out_dir = rec::to_utf8((dir.path() / "not yet").wstring());
    const std::vector<CheckResult> r = run_doctor(c);
    std::set<std::string> names;
    for (const CheckResult& x : r) {
        names.insert(x.name);
        CHECK(!x.detail.empty());
        if (x.status != CheckStatus::Pass) CHECK(!x.fix.empty() || x.name == "Power");
    }
    for (const char* n : {"Windows", "CPU", "CPU threads", "Memory", "GPU", "Hotkey", "Output folder", "Filesystem",
                          "Free space", "Disk speed", "Power", "Game audio", "HW encoder", "FFmpeg"})
        CHECK(names.count(n) == 1);
    for (const CheckResult& x : r)
        if (x.name == "Output folder") {
            CHECK(x.status == CheckStatus::Pass);
            CHECK(x.detail.find("will be created") != std::string::npos);
        }
    CHECK(!std::filesystem::exists(dir.path() / "not yet"));  // doctor changes nothing

    const RateEstimate e = estimate_write_rate(1280, 720, 60, false);  // plan §9: ~33 MB/s
    CHECK(e.required_mbps > 33 && e.required_mbps < 34);
    CHECK(e.typical_mbps > 14 && e.typical_mbps < 15);
    CHECK(estimate_write_rate(1920, 1080, 60, false).required_mbps > 74);  // plan §9: ~75 MB/s
}
