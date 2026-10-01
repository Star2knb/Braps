// rec_hook64.dll / rec_hook32.dll: loaded into the game by rec.exe (recorder plan §4.1, §4.5, §13.2).
//
// DllMain does nothing but start one thread. That control thread opens the host's shared memory,
// installs the hooks (immediately, or once the graphics DLLs have loaded), then serves the host:
// heartbeat, commands, host-lost detection, detach.
#include <windows.h>
#include <psapi.h>

#include <MinHook.h>

#include <cstdio>
#include <cwchar>

#include "dxgi_hook.h"
#include "hlog.h"
#include "hook_state.h"

namespace rec::hook {

Globals g;

namespace {

constexpr uint64_t kHostLostSeconds = 3;   // host heartbeat older than this: host lost (E1401)
constexpr DWORD kControlTickMs = 100;
constexpr DWORD kDetachGraceMs = 250;      // after the last thread left a detour (see do_detach)

constexpr const char* kArch = sizeof(void*) == 8 ? "x64" : "x86";

// ---- Loader notification (deferred install, §4.1) --------------------------------------------
// The callback runs under the loader lock: it may only set an event. Everything else happens on
// the control thread.
struct UnicodeString {
    USHORT length, maximum_length;
    PWSTR buffer;
};
struct DllLoadedData {
    ULONG flags;
    const UnicodeString* full_name;
    const UnicodeString* base_name;
    PVOID base;
    ULONG size;
};
using NotificationFn = VOID(CALLBACK*)(ULONG reason, const DllLoadedData* data, PVOID context);
using RegisterFn = LONG(NTAPI*)(ULONG flags, NotificationFn fn, PVOID context, PVOID* cookie);
using UnregisterFn = LONG(NTAPI*)(PVOID cookie);
constexpr ULONG kReasonLoaded = 1;

PVOID g_notification_cookie = nullptr;

bool name_is(const UnicodeString* s, const wchar_t* name) {
    const size_t n = wcslen(name);
    return s && s->buffer && s->length == n * sizeof(wchar_t) && _wcsnicmp(s->buffer, name, n) == 0;
}

VOID CALLBACK on_dll_loaded(ULONG reason, const DllLoadedData* data, PVOID) {
    if (reason == kReasonLoaded && data && (name_is(data->base_name, L"d3d11.dll") || name_is(data->base_name, L"dxgi.dll")))
        SetEvent(g.dll_event);
}

void register_notification() {
    if (g_notification_cookie) return;
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    auto fn = ntdll ? reinterpret_cast<RegisterFn>(GetProcAddress(ntdll, "LdrRegisterDllNotification")) : nullptr;
    if (!fn || fn(0, &on_dll_loaded, nullptr, &g_notification_cookie) != 0) {
        g_notification_cookie = nullptr;
        log_text(Level::Warn, "LdrRegisterDllNotification unavailable");
    }
}

void unregister_notification() {
    if (!g_notification_cookie) return;
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    auto fn = ntdll ? reinterpret_cast<UnregisterFn>(GetProcAddress(ntdll, "LdrUnregisterDllNotification")) : nullptr;
    if (fn) fn(g_notification_cookie);
    g_notification_cookie = nullptr;
}

// ---- Shared memory ---------------------------------------------------------------------------
template <class T>
T* map_object(uint32_t pid, const wchar_t* suffix, size_t size, HANDLE* mapping_out) {
    wchar_t name[64];
    proto::object_name(name, 64, pid, suffix);
    HANDLE mapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, name);
    if (!mapping) return nullptr;
    void* view = MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, size);
    if (!view) {
        CloseHandle(mapping);
        return nullptr;
    }
    *mapping_out = mapping;  // kept open: see Globals::ctl_map
    return static_cast<T*>(view);
}

bool open_shared_memory() {
    const uint32_t pid = GetCurrentProcessId();
    g.ctl = map_object<proto::ControlBlock>(pid, L"ctl", sizeof(proto::ControlBlock), &g.ctl_map);
    g.log = map_object<proto::LogRing>(pid, L"log", sizeof(proto::LogRing), &g.log_map);
    wchar_t name[64];
    proto::object_name(name, 64, pid, L"cmd");
    g.cmd_event = OpenEventW(EVENT_MODIFY_STATE | SYNCHRONIZE, FALSE, name);
    g.dll_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!g.ctl || !g.log || !g.cmd_event || !g.dll_event) return false;
    if (g.ctl->magic != proto::kMagic || g.ctl->version != proto::kVersion || g.ctl->size != sizeof(proto::ControlBlock) ||
        g.ctl->game_pid != pid || g.log->magic != proto::kLogMagic || g.log->capacity != proto::kLogCapacity) {
        g.ctl->error_code.store(proto::kErrProtocolMismatch);
        set_state(proto::HookState::Failed);
        return false;
    }
    QueryPerformanceFrequency(reinterpret_cast<LARGE_INTEGER*>(&g.qpc_freq));
    log_attach(g.log);
    return true;
}

void close_shared_memory() {
    log_attach(nullptr);
    if (g.ctl) UnmapViewOfFile(g.ctl);
    if (g.log) UnmapViewOfFile(g.log);
    if (g.ctl_map) CloseHandle(g.ctl_map);
    if (g.log_map) CloseHandle(g.log_map);
    if (g.cmd_event) CloseHandle(g.cmd_event);
    if (g.dll_event) CloseHandle(g.dll_event);
    g.ctl_map = g.log_map = g.cmd_event = g.dll_event = nullptr;
    g.ctl = nullptr;
    g.log = nullptr;
}

// ---- Overlays (I1102) --------------------------------------------------------------------------
// Modules that hook Present themselves: first suspects when capture misbehaves (§4.4).
void log_overlays() {
    static const wchar_t* const kOverlays[] = {
        L"gameoverlayrenderer64.dll", L"gameoverlayrenderer.dll",  // Steam
        L"DiscordHook64.dll",         L"DiscordHook.dll",          // Discord
        L"RTSSHooks64.dll",           L"RTSSHooks.dll",            // RivaTuner / Afterburner
        L"nvspcap64.dll",             L"nvspcap.dll",              // GeForce Experience
        L"graphics-hook64.dll",       L"graphics-hook32.dll",      // OBS game capture
        L"fraps64.dll",               L"fraps.dll",                // FRAPS
        L"EOSOVH-Win64-Shipping.dll",                              // Epic overlay
    };
    HMODULE modules[1024];
    DWORD needed = 0;
    if (!EnumProcessModules(GetCurrentProcess(), modules, sizeof(modules), &needed)) return;
    const size_t count = (needed < sizeof(modules) ? needed : sizeof(modules)) / sizeof(HMODULE);
    for (size_t i = 0; i < count; ++i) {
        wchar_t base[MAX_PATH];
        if (!GetModuleBaseNameW(GetCurrentProcess(), modules[i], base, MAX_PATH)) continue;
        for (const wchar_t* overlay : kOverlays)
            if (_wcsicmp(base, overlay) == 0) log_event(Ev::OverlaysDetected, "%ls", base);
    }
}

// ---- Installing --------------------------------------------------------------------------------
bool graphics_dlls_loaded() { return GetModuleHandleW(L"dxgi.dll") && GetModuleHandleW(L"d3d11.dll"); }

proto::HookState g_installed_state = proto::HookState::Loading;  // what to show when the host is back

void try_install(bool* installed, bool* waiting_logged) {
    if (*installed) return;
    if (!graphics_dlls_loaded()) {
        if (!*waiting_logged) {
            *waiting_logged = true;
            const bool dxgi = GetModuleHandleW(L"dxgi.dll") != nullptr;
            log_event(Ev::HookInstallDeferred, "waiting for %s", dxgi ? "d3d11.dll" : "dxgi.dll/d3d11.dll");
            register_notification();
            g_installed_state = proto::HookState::Waiting;
            set_state(proto::HookState::Waiting);
        }
        return;
    }
    // Launch mode: let the loader finish and the game's own device creation settle before we create a
    // throwaway device of our own (§4.1). With attach the DLLs were already there, no wait needed.
    if (*waiting_logged) Sleep(50);

    unregister_notification();
    *installed = true;
    if (install_dxgi_hooks()) {
        g.ctl->hooked_apis.fetch_or(proto::kApiDXGI);
        g_installed_state = proto::HookState::Hooked;
        log_text(Level::Info, "hooks installed (Present, Present1)");
    } else {
        g_installed_state = proto::HookState::Failed;
        g.ctl->error_code.store(event_number(Ev::LocateFailed));
    }
    set_state(g_installed_state);
}

// ---- Detach (§4.5) -----------------------------------------------------------------------------
[[noreturn]] void do_detach() {
    set_state(proto::HookState::Detaching);
    remove_dxgi_hooks();
    // No thread may be inside a detour, and none may be on its way out of one: after the counter
    // reaches zero a thread still has to execute the detour's final instructions, which are in this
    // DLL. Hence the grace period and a second look.
    const uint64_t deadline = qpc() + 2 * uint64_t(g.qpc_freq);
    for (int pass = 0; pass < 2; ++pass) {
        while (g.inflight.load(std::memory_order_acquire) != 0 && qpc() < deadline) Sleep(1);
        Sleep(kDetachGraceMs);
    }
    if (g.inflight.load(std::memory_order_acquire) != 0) {
        // A thread is stuck inside Present (a hung game). Unloading now could crash it: stay loaded.
        log_text(Level::Warn, "detach: a thread is still in Present; hooks off, DLL stays loaded");
        set_state(proto::HookState::Detached);
        ExitThread(0);
    }
    MH_Uninitialize();
    unregister_notification();
    log_text(Level::Info, "detached");
    set_state(proto::HookState::Detached);
    if (g.tls != TLS_OUT_OF_INDEXES) TlsFree(g.tls);
    HMODULE self = g.module;
    close_shared_memory();
    FreeLibraryAndExitThread(self, 0);
}

DWORD WINAPI control_thread(LPVOID) {
    if (!open_shared_memory()) {
        OutputDebugStringW(L"rec hook: shared memory missing or incompatible; hook inactive\n");
        return 0;
    }
    proto::ControlBlock* c = g.ctl;
    g.tls = TlsAlloc();
    set_state(proto::HookState::Loading);
    log_text(Level::Info, "hook loaded (%s, pid %lu)", kArch, GetCurrentProcessId());
    log_overlays();

    const MH_STATUS mh = MH_Initialize();
    if (mh != MH_OK || g.tls == TLS_OUT_OF_INDEXES) {
        log_text(Level::Error, "MH_Initialize: %s", MH_StatusToString(mh));
        c->error_code.store(event_number(Ev::LocateFailed));
        set_state(proto::HookState::Failed);
    }

    bool installed = mh != MH_OK || g.tls == TLS_OUT_OF_INDEXES;  // nothing to install if init failed
    bool waiting_logged = false;
    bool host_lost = false;
    const HANDLE wait_handles[2] = {g.cmd_event, g.dll_event};

    for (;;) {
        try_install(&installed, &waiting_logged);

        const uint64_t now = qpc();
        c->hook_heartbeat_qpc.store(now, std::memory_order_relaxed);
        const uint64_t host_beat = c->host_heartbeat_qpc.load(std::memory_order_relaxed);
        const bool stale = host_beat != 0 && now > host_beat && (now - host_beat) > kHostLostSeconds * uint64_t(g.qpc_freq);
        if (stale && !host_lost) {
            host_lost = true;
            log_event(Ev::HostLost, "no heartbeat for %llu ms", (now - host_beat) * 1000 / uint64_t(g.qpc_freq));
            set_state(proto::HookState::Idle);
        } else if (!stale && host_lost) {
            host_lost = false;
            log_text(Level::Info, "host is back");
            set_state(g_installed_state);
        }

        const uint32_t command = c->command.exchange(uint32_t(proto::Command::None));
        if (command == uint32_t(proto::Command::Detach)) do_detach();

        WaitForMultipleObjects(2, wait_handles, FALSE, kControlTickMs);
    }
}

}  // namespace
}  // namespace rec::hook

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        rec::hook::g.module = module;
        // Never install from DllMain (loader lock): hand over to our own thread.
        if (HANDLE t = CreateThread(nullptr, 0, rec::hook::control_thread, nullptr, 0, nullptr)) CloseHandle(t);
    }
    return TRUE;
}
