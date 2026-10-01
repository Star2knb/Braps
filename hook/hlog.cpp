#include "hlog.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>

namespace rec::hook {
namespace {

proto::LogRing* g_ring = nullptr;

void push(Level level, uint16_t code, const char* fmt, va_list args) {
    proto::LogRing* ring = g_ring;
    if (!ring) return;
    proto::LogRecord r;
    std::memset(&r, 0, sizeof(r));
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    r.qpc = uint64_t(now.QuadPart);
    r.level = uint8_t(level);
    r.flags = proto::kLogText;
    r.code = code;
    r.thread_id = GetCurrentThreadId();
    // arg[] and tag are contiguous (48 bytes) and hold the text.
    char text[proto::kLogTextMax];
    _vsnprintf_s(text, sizeof(text), _TRUNCATE, fmt, args);
    std::memcpy(r.arg, text, sizeof(text));
    ring->push(r);
}

}  // namespace

void log_attach(proto::LogRing* ring) { g_ring = ring; }

void log_event(Ev e, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    push(event_level(e), event_number(e), fmt, args);
    va_end(args);
}

void log_text(Level level, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    push(level, 0, fmt, args);
    va_end(args);
}

void kiero_debug(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    push(Level::Debug, 0, fmt, args);
    va_end(args);
}

void kiero_assert_failed(const char* expression, int line) {
    // Never abort inside the game (plan §4.6.1): record it and carry on.
    log_text(Level::Error, "kiero assert line %d: %.30s", line, expression);
}

}  // namespace rec::hook
