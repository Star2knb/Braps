#include "rec/log.h"

#include <windows.h>

#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

#include <spdlog/async.h>
#include <spdlog/pattern_formatter.h>
#include <spdlog/sinks/base_sink.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/dist_sink.h>
#include <spdlog/sinks/null_sink.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include "rec/paths.h"

namespace rec::logging {
namespace {

constexpr size_t kQueueSize = 32768;  // messages; the queue blocks when full rather than losing lines
constexpr size_t kAppLogSize = 10 * 1024 * 1024;
constexpr size_t kAppLogFiles = 5;
constexpr int kSubsystems = 7;

// [%*] = level padded to five characters, upper case (spdlog's own names are lower case).
class LevelFlag final : public spdlog::custom_flag_formatter {
public:
    void format(const spdlog::details::log_msg& msg, const std::tm&, spdlog::memory_buf_t& dest) override {
        static constexpr std::string_view kNames[] = {"TRACE", "DEBUG", "INFO ", "WARN ", "ERROR", "FATAL", "OFF  "};
        const std::string_view n = kNames[size_t(msg.level)];
        dest.append(n.data(), n.data() + n.size());
    }
    std::unique_ptr<custom_flag_formatter> clone() const override { return std::make_unique<LevelFlag>(); }
};

std::unique_ptr<spdlog::formatter> make_formatter(bool with_date) {
    auto f = std::make_unique<spdlog::pattern_formatter>();
    f->add_flag<LevelFlag>('*').set_pattern(with_date ? "%Y-%m-%d %H:%M:%S.%e [%*] [%-7n] %v" : "[%*] [%-7n] %v");
    return f;
}

spdlog::level::level_enum to_spdlog(Level l) {
    switch (l) {
    case Level::Debug: return spdlog::level::debug;
    case Level::Info: return spdlog::level::info;
    case Level::Warn: return spdlog::level::warn;
    case Level::Error: return spdlog::level::err;
    case Level::Fatal: return spdlog::level::critical;
    }
    return spdlog::level::info;
}

// Signals when the logger thread has processed a flush request. All loggers share one queue and
// one thread, so once a flush posted to the barrier logger is done, every earlier message is
// written (spdlog's async flush() itself only posts the request).
class BarrierSink final : public spdlog::sinks::base_sink<std::mutex> {
public:
    uint64_t count() const { return flushed_.load(); }
    void wait_past(uint64_t old) const {
        for (uint64_t c; (c = flushed_.load()) == old;) flushed_.wait(c);
    }

protected:
    void sink_it_(const spdlog::details::log_msg&) override {}
    void flush_() override {
        flushed_.fetch_add(1);
        flushed_.notify_all();
    }

private:
    std::atomic<uint64_t> flushed_{0};
};

struct State {
    std::mutex mutex;  // start/stop/session changes
    bool started = false;
    bool verbose = false;
    std::array<std::shared_ptr<spdlog::logger>, kSubsystems> loggers;
    std::shared_ptr<spdlog::sinks::dist_sink_mt> session;  // part of every logger; empty unless a session is open
    std::shared_ptr<spdlog::sinks::sink> session_file;
    std::shared_ptr<BarrierSink> barrier_sink;
    std::shared_ptr<spdlog::async_logger> barrier;  // not registered, so the periodic flush leaves it alone
    std::shared_ptr<spdlog::logger> null_logger =
        std::make_shared<spdlog::logger>("null", std::make_shared<spdlog::sinks::null_sink_mt>());
};
State& state() {
    static State s;
    return s;
}

std::array<std::atomic<uint64_t>, kEventCount> g_counts{};

// Waits until everything logged so far has been written (caller holds the state mutex).
void drain(State& s) {
    const uint64_t c = s.barrier_sink->count();
    s.barrier->flush();
    s.barrier_sink->wait_past(c);
}

}  // namespace

bool start(const Options& options, std::string* error) {
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (s.started) return true;
    const auto level = options.verbose ? spdlog::level::debug : spdlog::level::info;
    std::vector<spdlog::sink_ptr> sinks;
    try {
        if (!options.app_log_dir.empty()) {
            std::filesystem::create_directories(options.app_log_dir);
            auto file = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
                (options.app_log_dir / L"rec.log").wstring(), kAppLogSize, kAppLogFiles);
            file->set_level(level);
            file->set_formatter(make_formatter(true));
            sinks.push_back(file);
        }
        if (options.console) {
            auto con = std::make_shared<spdlog::sinks::stderr_color_sink_mt>();
            con->set_level(spdlog::level::warn);
            con->set_formatter(make_formatter(false));
            sinks.push_back(con);
        }
        s.session = std::make_shared<spdlog::sinks::dist_sink_mt>();
        sinks.push_back(s.session);
        // The logger thread runs below normal priority (plan §3.2).
        spdlog::init_thread_pool(kQueueSize, 1, [] { SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL); });
        for (int i = 0; i < kSubsystems; ++i) {
            const char* name = subsystem_name(Subsystem(i + 1));
            auto lg = std::make_shared<spdlog::async_logger>(name, sinks.begin(), sinks.end(), spdlog::thread_pool(),
                                                             spdlog::async_overflow_policy::block);
            lg->set_level(level);
            lg->flush_on(spdlog::level::warn);
            spdlog::register_logger(lg);
            s.loggers[size_t(i)] = lg;
        }
        s.barrier_sink = std::make_shared<BarrierSink>();
        s.barrier = std::make_shared<spdlog::async_logger>("barrier", s.barrier_sink, spdlog::thread_pool(),
                                                           spdlog::async_overflow_policy::block);
        spdlog::flush_every(std::chrono::seconds(1));
    } catch (const std::exception& e) {
        if (error) *error = std::string("can't start logging: ") + e.what();
        s.loggers = {};
        s.barrier.reset();
        s.barrier_sink.reset();
        s.session.reset();
        spdlog::shutdown();
        return false;
    }
    s.verbose = options.verbose;
    s.started = true;
    return true;
}

void stop() {
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (!s.started) return;
    for (auto& lg : s.loggers) lg->flush();
    drain(s);
    s.loggers = {};
    s.barrier.reset();
    s.barrier_sink.reset();
    s.session.reset();
    s.session_file.reset();
    spdlog::shutdown();  // joins the logger thread
    s.started = false;
}

bool open_session(const std::filesystem::path& log_file, std::string* error) {
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (!s.started) {
        if (error) *error = "logging not started";
        return false;
    }
    try {
        auto file = std::make_shared<spdlog::sinks::basic_file_sink_mt>(log_file.wstring(), true);
        file->set_level(s.verbose ? spdlog::level::debug : spdlog::level::info);
        file->set_formatter(make_formatter(true));
        drain(s);  // lines logged before the session opened stay out of its file
        s.session->set_sinks({file});
        s.session_file = file;
    } catch (const std::exception& e) {
        if (error) *error = std::string("can't open the session log: ") + e.what();
        return false;
    }
    return true;
}

void close_session() {
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (!s.started) return;
    if (!s.session_file) return;
    drain(s);  // queued session lines reach the file first
    s.session->set_sinks({});
    s.session_file->flush();
    s.session_file.reset();  // closes the file
}

void flush() {
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (!s.started) return;
    for (auto& lg : s.loggers) lg->flush();
    drain(s);  // the flushes above were queued before the barrier, so they are done too
}

spdlog::logger& get(Subsystem subsystem) {
    State& s = state();
    const size_t i = size_t(subsystem) - 1;
    if (s.started && i < s.loggers.size() && s.loggers[i]) return *s.loggers[i];
    return *s.null_logger;
}

void event(Ev e, std::string_view details) {
    g_counts[size_t(e)].fetch_add(1, std::memory_order_relaxed);
    const EventInfo& info = event_info(e);
    spdlog::logger& lg = get(event_subsystem(e));
    if (details.empty())
        lg.log(to_spdlog(event_level(e)), "{} {}", info.code, info.name);
    else
        lg.log(to_spdlog(event_level(e)), "{} {} {}", info.code, info.name, details);
}

uint64_t event_count(Ev e) { return g_counts[size_t(e)].load(std::memory_order_relaxed); }

}  // namespace rec::logging
