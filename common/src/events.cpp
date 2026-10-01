#include "rec/events.h"

namespace rec {

const EventInfo* find_event(std::string_view code_or_name) {
    for (const EventInfo& e : kEvents)
        if (code_or_name == e.code || code_or_name == e.name) return &e;
    return nullptr;
}

const char* level_name(Level level) {
    switch (level) {
    case Level::Debug: return "DEBUG";
    case Level::Info: return "INFO";
    case Level::Warn: return "WARN";
    case Level::Error: return "ERROR";
    case Level::Fatal: return "FATAL";
    }
    return "?";
}

// Names as they appear in the log's subsystem column (plan §10.1: "[writer ]").
const char* subsystem_name(Subsystem subsystem) {
    switch (subsystem) {
    case Subsystem::Hook: return "hook";
    case Subsystem::Transport: return "ipc";
    case Subsystem::Encoder: return "encoder";
    case Subsystem::Disk: return "writer";
    case Subsystem::Audio: return "audio";
    case Subsystem::System: return "system";
    case Subsystem::Cli: return "cli";
    }
    return "?";
}

}  // namespace rec
