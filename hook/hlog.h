// Logging from inside the game (recorder plan §10.2): the hook never touches files or the console.
// It writes 64-byte records into a lock-free ring in shared memory; the host drains and formats them.
// Nothing here allocates; every call is safe from any thread.
#pragma once

#include <cstdint>

#include "rec/events.h"
#include "rec/protocol.h"

namespace rec::hook {

void log_attach(proto::LogRing* ring);

// An event of the catalogue with up to 47 characters of detail text (the record's argument and tag
// fields hold it as text). The level comes from the event's code.
void log_event(Ev e, const char* fmt, ...);

// Free text at a level, with no event code.
void log_text(Level level, const char* fmt, ...);

// kiero2's debug output and assertions (hook/kiero_config.h routes them here).
void kiero_debug(const char* fmt, ...);
void kiero_assert_failed(const char* expression, int line);

}  // namespace rec::hook
