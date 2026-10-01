// Host logging (recorder plan §10.1): spdlog, asynchronous, one logger per subsystem sharing sinks:
//   - rolling application log %LOCALAPPDATA%\rec\logs\rec.log (5 x 10 MB),
//   - the console (WARN and above, so the status line stays readable),
//   - the session log <recording>.log while a recording is open.
// Line format: 2026-09-25 11:56:49.123 [WARN ] [writer ] W4101 slow_write latency=312ms ...
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>

#include <spdlog/spdlog.h>

#include "rec/events.h"

namespace rec::logging {

struct Options {
    std::filesystem::path app_log_dir;  // empty: no application log file
    bool verbose = false;               // DEBUG in the log files (console stays at WARN)
    bool console = true;
};

// Starts logging. Until then (and after stop) events are only counted.
bool start(const Options& options, std::string* error);
void stop();  // flushes and closes every file

// Adds/removes the session log file (INFO and above, DEBUG when verbose).
bool open_session(const std::filesystem::path& log_file, std::string* error);
void close_session();
void flush();

// Logger for free-text messages of a subsystem; a do-nothing logger before start().
spdlog::logger& get(Subsystem subsystem);

// Logs an event at its level and subsystem as "<code> <name> <details>", and counts it.
void event(Ev e, std::string_view details = {});
template <class... Args>
void event(Ev e, spdlog::format_string_t<Args...> fmt, Args&&... args) {
    event(e, std::string_view(spdlog::fmt_lib::format(fmt, std::forward<Args>(args)...)));
}
uint64_t event_count(Ev e);  // since process start (for the session summary)

}  // namespace rec::logging
