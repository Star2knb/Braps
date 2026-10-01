// State shared by the hook DLL's files. One instance per process (the DLL is loaded once).
#pragma once

#include <windows.h>

#include <atomic>
#include <cstdint>

#include "rec/protocol.h"

namespace rec::hook {

struct Globals {
    HMODULE module = nullptr;
    proto::ControlBlock* ctl = nullptr;
    proto::LogRing* log = nullptr;
    // The hook keeps handles to the shared objects, not just views: a named object loses its name when
    // the last handle closes, and then a new host could not find the hook's objects (takeover).
    HANDLE ctl_map = nullptr;
    HANDLE log_map = nullptr;
    HANDLE cmd_event = nullptr;  // host -> hook: a command is waiting
    HANDLE dll_event = nullptr;  // set by the loader-notification callback: a graphics DLL was loaded
    int64_t qpc_freq = 0;
    DWORD tls = TLS_OUT_OF_INDEXES;  // per-thread depth inside our Present detours
    std::atomic<int> inflight{0};    // threads currently inside a detour (detach waits for zero)
    std::atomic<bool> enabled{false};  // measuring (cleared on exception and on detach)
};

extern Globals g;

inline uint64_t qpc() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return uint64_t(t.QuadPart);
}

inline void set_state(proto::HookState s) { g.ctl->hook_state.store(uint32_t(s), std::memory_order_release); }

}  // namespace rec::hook
