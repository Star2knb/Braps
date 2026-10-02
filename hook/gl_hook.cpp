#include "gl_hook.h"

#include <MinHook.h>

#include <atomic>

#include "capture_gl.h"
#include "hlog.h"
#include "hook_state.h"
#include "locate.h"
#include "measure.h"

namespace rec::hook {
namespace {

using SwapFn = BOOL(WINAPI*)(HDC);

SwapFn g_orig_wgl = nullptr;  // opengl32!wglSwapBuffers
SwapFn g_orig_gdi = nullptr;  // gdi32!SwapBuffers (GLFW, SDL and LWJGL call this one)
void* g_target_wgl = nullptr;
void* g_target_gdi = nullptr;

// The window being measured: the first real one to swap (§4.3). Other windows (overlays, launchers) are
// counted as ignored. Handles are only compared, never used after the call that passed them.
std::atomic<HDC> g_main{nullptr};
HDC g_rejected = nullptr;  // last window refused for being tiny; only touched under measure_gl()

constexpr LONG kMinWindowSide = 160;

// Decides whether `hdc` is (now) the window to measure. If it is, publishes its description.
bool adopt_window(HDC hdc, uint64_t now) {
    if (hdc == g_rejected) return false;
    if (other_api_active(true, now)) return false;  // a Direct3D swap chain is being measured
    HDC current = g_main.load(std::memory_order_acquire);
    // Another window only takes over when the measured one has been silent for a second.
    if (current && g.ctl->backend.load(std::memory_order_relaxed) == proto::kApiOpenGL &&
        now - g.ctl->last_present_qpc.load(std::memory_order_relaxed) <= uint64_t(g.qpc_freq))
        return false;

    HWND window = WindowFromDC(hdc);
    RECT rc{};
    if (!window || !GetClientRect(window, &rc)) return false;  // a memory DC or similar: not a window
    if (rc.right - rc.left < kMinWindowSide || rc.bottom - rc.top < kMinWindowSide) {
        g_rejected = hdc;
        return false;
    }
    if (!g_main.compare_exchange_strong(current, hdc, std::memory_order_acq_rel)) return false;

    proto::ControlBlock* c = g.ctl;
    c->backbuffer_width.store(uint32_t(rc.right - rc.left));
    c->backbuffer_height.store(uint32_t(rc.bottom - rc.top));
    c->backbuffer_format.store(28);  // DXGI_FORMAT_R8G8B8A8_UNORM: what a Windows OpenGL window is
    c->last_present_qpc.store(0);    // no frame interval across a change of window
    const uint32_t hz = window_refresh_hz(window);
    c->display_refresh_hz.store(hz);
    c->backend.store(proto::kApiOpenGL, std::memory_order_release);
    if (hz) log_event(Ev::DisplayRefresh, "monitor refresh %u Hz", hz);
    log_event(Ev::BackendSelected, "OpenGL %ldx%ld", long(rc.right - rc.left), long(rc.bottom - rc.top));
    return true;
}

// The measuring itself. Runs inside the SEH guard of measure_gl().
uint64_t measure_inner(HDC hdc, uint64_t now) {
    proto::ControlBlock* c = g.ctl;
    const bool owner = hdc == g_main.load(std::memory_order_acquire) && c->backend.load(std::memory_order_relaxed) == proto::kApiOpenGL;
    if (!owner && !adopt_window(hdc, now)) {
        c->present_ignored.fetch_add(1, std::memory_order_relaxed);
        return 0;
    }
    const uint32_t frame_time_us = note_present(now);
    const uint64_t count = c->present_count.load(std::memory_order_relaxed);
    if ((count & 31) == 0) {  // the window's size changes: keep it current for the host
        RECT rc{};
        HWND window = WindowFromDC(hdc);
        if (window && GetClientRect(window, &rc)) {
            c->backbuffer_width.store(uint32_t(rc.right - rc.left), std::memory_order_relaxed);
            c->backbuffer_height.store(uint32_t(rc.bottom - rc.top), std::memory_order_relaxed);
        }
    }
    return capture_gl_on_present(hdc, now, frame_time_us);  // one atomic load unless the host asked for a recording
}

void measure_gl(HDC hdc) {
    const uint64_t t0 = qpc();
    uint64_t held = 0;  // the deliberate wait of lock mode
    __try {
        held = measure_inner(hdc, t0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        note_exception(GetExceptionCode());
        return;
    }
    note_cost(t0, held);
}

// Entry and exit of both detours. The depth counter makes nested calls (one swap function calling the
// other) count once. The last error is preserved: TlsGetValue clears it.
void enter(HDC hdc) {
    const DWORD saved_error = GetLastError();
    g.inflight.fetch_add(1, std::memory_order_acq_rel);
    const auto depth = reinterpret_cast<uintptr_t>(TlsGetValue(g.tls));
    TlsSetValue(g.tls, reinterpret_cast<LPVOID>(depth + 1));
    if (depth == 0 && g.enabled.load(std::memory_order_acquire)) measure_gl(hdc);
    SetLastError(saved_error);
}

void leave() {
    const DWORD error = GetLastError();
    const auto depth = reinterpret_cast<uintptr_t>(TlsGetValue(g.tls));
    TlsSetValue(g.tls, reinterpret_cast<LPVOID>(depth - 1));
    g.inflight.fetch_sub(1, std::memory_order_release);
    SetLastError(error);
}

BOOL WINAPI wgl_swap_detour(HDC hdc) {
    enter(hdc);
    const BOOL r = g_orig_wgl(hdc);
    leave();
    return r;
}

BOOL WINAPI gdi_swap_detour(HDC hdc) {
    enter(hdc);
    const BOOL r = g_orig_gdi(hdc);
    leave();
    return r;
}

bool create_hook(const char* name, void* target, void* detour, void** original) {
    const MH_STATUS s = MH_CreateHook(target, detour, original);
    if (s != MH_OK) log_text(Level::Error, "MH_CreateHook %s: %s", name, MH_StatusToString(s));
    return s == MH_OK;
}

}  // namespace

bool install_gl_hooks() {
    if (g.ctl->debug_kiero_fail & proto::kApiOpenGL) {  // --debug-kiero-fail opengl
        log_event(Ev::LocateFailed, "OpenGL lookup failed on purpose (--debug-kiero-fail)");
        return false;
    }
    HMODULE gl = GetModuleHandleW(L"opengl32.dll");
    HMODULE gdi = GetModuleHandleW(L"gdi32.dll");
    void* wgl = gl ? reinterpret_cast<void*>(GetProcAddress(gl, "wglSwapBuffers")) : nullptr;
    void* sb = gdi ? reinterpret_cast<void*>(GetProcAddress(gdi, "SwapBuffers")) : nullptr;  // follows gdi32's forwarder
    // A pointer outside the module it should be in means something else is there already (§4.1): don't hook it.
    if (wgl && !pointer_in_module(L"opengl32.dll", wgl)) {
        log_event(Ev::VtablePointerOutsideModule, "wglSwapBuffers outside opengl32.dll");
        wgl = nullptr;
    }
    if (sb && !pointer_in_module(L"gdi32.dll", sb) && !pointer_in_module(L"gdi32full.dll", sb) && !pointer_in_module(L"win32u.dll", sb)) {
        log_event(Ev::VtablePointerOutsideModule, "SwapBuffers outside gdi32");
        sb = nullptr;
    }
    if (!wgl && !sb) {
        log_event(Ev::LocateFailed, "wglSwapBuffers / SwapBuffers not found");
        return false;
    }
    int hooked = 0;
    if (wgl && create_hook("wglSwapBuffers", wgl, reinterpret_cast<void*>(&wgl_swap_detour), reinterpret_cast<void**>(&g_orig_wgl))) {
        g_target_wgl = wgl;
        ++hooked;
    }
    // One function behind both names would collide.
    if (sb && sb != wgl && create_hook("SwapBuffers", sb, reinterpret_cast<void*>(&gdi_swap_detour), reinterpret_cast<void**>(&g_orig_gdi))) {
        g_target_gdi = sb;
        ++hooked;
    }
    if (!hooked) return false;

    g.enabled.store(true, std::memory_order_release);
    bool ok = true;
    void* targets[2] = {g_target_wgl, g_target_gdi};
    for (void* target : targets) {
        if (!target) continue;
        const MH_STATUS s = MH_EnableHook(target);
        if (s != MH_OK) {
            log_text(Level::Error, "MH_EnableHook: %s", MH_StatusToString(s));
            ok = false;
        }
    }
    return ok;
}

void remove_gl_hooks() {
    g.enabled.store(false, std::memory_order_release);
    if (g_target_wgl) MH_DisableHook(g_target_wgl);
    if (g_target_gdi) MH_DisableHook(g_target_gdi);
}

}  // namespace rec::hook
