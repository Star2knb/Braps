#include "dxgi_hook.h"

#include <MinHook.h>
#include <d3d10_1.h>
#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_2.h>

#include <atomic>

#include "hlog.h"
#include "hook_state.h"
#include "locate.h"

namespace rec::hook {
namespace {

using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using Present1Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);

PresentFn g_orig_present = nullptr;
Present1Fn g_orig_present1 = nullptr;
uint64_t g_locate_us = 0;

// The swap chain being measured: the first real one to present (§4.3). Overlays (Steam, Discord, ...)
// present their own small chains; those are counted as ignored. Pointers are only compared, never used
// after the Present call that passed them.
std::atomic<IDXGISwapChain*> g_main{nullptr};
IDXGISwapChain* g_rejected = nullptr;  // last chain refused for being tiny; only touched under measure()

constexpr uint32_t kMinChainSide = 160;

// Which D3D API owns this swap chain. COM calls: only on the first Present of a chain.
uint32_t api_of(IDXGISwapChain* sc, const char** name) {
    void* device = nullptr;
    if (SUCCEEDED(sc->GetDevice(__uuidof(ID3D11Device), &device))) {
        static_cast<IUnknown*>(device)->Release();
        *name = "D3D11";
        return proto::kApiD3D11;
    }
    if (SUCCEEDED(sc->GetDevice(__uuidof(ID3D12CommandQueue), &device))) {
        static_cast<IUnknown*>(device)->Release();
        *name = "D3D12";
        return proto::kApiD3D12;
    }
    if (SUCCEEDED(sc->GetDevice(__uuidof(ID3D10Device), &device))) {
        static_cast<IUnknown*>(device)->Release();
        *name = "D3D10";
        return proto::kApiD3D10;
    }
    *name = "DXGI";
    return proto::kApiDXGI;
}

// Decides whether `sc` is (now) the swap chain to measure. If it is, publishes its description.
bool adopt_swapchain(IDXGISwapChain* sc, uint64_t now) {
    if (sc == g_rejected) return false;
    IDXGISwapChain* current = g_main.load(std::memory_order_acquire);
    // Another chain only takes over when the measured one has been silent for a second (the game
    // recreated its swap chain, e.g. when switching between windowed and fullscreen).
    if (current && now - g.ctl->last_present_qpc.load(std::memory_order_relaxed) <= uint64_t(g.qpc_freq)) return false;

    DXGI_SWAP_CHAIN_DESC desc{};
    if (FAILED(sc->GetDesc(&desc))) return false;
    if (desc.BufferDesc.Width < kMinChainSide || desc.BufferDesc.Height < kMinChainSide) {
        g_rejected = sc;
        return false;
    }
    if (!g_main.compare_exchange_strong(current, sc, std::memory_order_acq_rel)) return false;

    const char* name = "DXGI";
    const uint32_t api = api_of(sc, &name);
    proto::ControlBlock* c = g.ctl;
    c->backbuffer_width.store(desc.BufferDesc.Width);
    c->backbuffer_height.store(desc.BufferDesc.Height);
    c->backbuffer_format.store(uint32_t(desc.BufferDesc.Format));
    c->last_present_qpc.store(0);  // no frame interval across a change of chain
    c->backend.store(api, std::memory_order_release);
    log_event(Ev::BackendSelected, "%s %ux%u fmt=%u locate=%llums", name, desc.BufferDesc.Width,
              desc.BufferDesc.Height, uint32_t(desc.BufferDesc.Format), g_locate_us / 1000);
    return true;
}

// The measuring itself. Runs inside the SEH guard of measure().
void measure_inner(IDXGISwapChain* sc, uint64_t now) {
    proto::ControlBlock* c = g.ctl;
    if (sc != g_main.load(std::memory_order_acquire) && !adopt_swapchain(sc, now)) {
        c->present_ignored.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const uint64_t previous = c->last_present_qpc.exchange(now, std::memory_order_relaxed);
    if (previous) c->frame_time_us.store(uint32_t((now - previous) * 1000000ull / uint64_t(g.qpc_freq)), std::memory_order_relaxed);
    c->present_count.fetch_add(1, std::memory_order_relaxed);
}

// An exception in our code disables measuring for good and the game carries on (§13.2).
void on_exception(DWORD code) {
    g.enabled.store(false, std::memory_order_release);
    g.ctl->error_code.store(event_number(Ev::HookException), std::memory_order_relaxed);
    log_event(Ev::HookException, "exception 0x%08lX in Present", code);
}

void measure(IDXGISwapChain* sc) {
    const uint64_t t0 = qpc();
    __try {
        measure_inner(sc, t0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        on_exception(GetExceptionCode());
        return;
    }
    // Our own cost: entry to just before the original Present. (The part after it is a decrement.)
    const uint32_t cost_ns = uint32_t((qpc() - t0) * 1000000000ull / uint64_t(g.qpc_freq));
    proto::ControlBlock* c = g.ctl;
    c->hook_cost_ns.store(cost_ns, std::memory_order_relaxed);
    c->hook_cost_total_ns.fetch_add(cost_ns, std::memory_order_relaxed);
    uint32_t seen = c->hook_cost_max_ns.load(std::memory_order_relaxed);
    while (cost_ns > seen && !c->hook_cost_max_ns.compare_exchange_weak(seen, cost_ns, std::memory_order_relaxed)) {
    }
}

// Entry and exit of every detour. The depth counter makes nested calls (Present calling Present1
// through the vtable) count once. The last error is preserved: TlsGetValue clears it.
struct Entry {
    DWORD saved_error;
};

Entry enter(IDXGISwapChain* sc, UINT flags) {
    Entry e{GetLastError()};
    g.inflight.fetch_add(1, std::memory_order_acq_rel);
    const auto depth = reinterpret_cast<uintptr_t>(TlsGetValue(g.tls));
    TlsSetValue(g.tls, reinterpret_cast<LPVOID>(depth + 1));
    // DXGI_PRESENT_TEST presents nothing (games use it while occluded).
    if (depth == 0 && g.enabled.load(std::memory_order_acquire) && !(flags & DXGI_PRESENT_TEST)) measure(sc);
    SetLastError(e.saved_error);
    return e;
}

void leave(const Entry& e) {
    const DWORD error = GetLastError();
    const auto depth = reinterpret_cast<uintptr_t>(TlsGetValue(g.tls));
    TlsSetValue(g.tls, reinterpret_cast<LPVOID>(depth - 1));
    g.inflight.fetch_sub(1, std::memory_order_release);
    SetLastError(error);
    (void)e;
}

HRESULT STDMETHODCALLTYPE present_detour(IDXGISwapChain* sc, UINT sync_interval, UINT flags) {
    const Entry e = enter(sc, flags);
    const HRESULT hr = g_orig_present(sc, sync_interval, flags);
    leave(e);
    return hr;
}

HRESULT STDMETHODCALLTYPE present1_detour(IDXGISwapChain1* sc, UINT sync_interval, UINT flags,
                                          const DXGI_PRESENT_PARAMETERS* parameters) {
    const Entry e = enter(sc, flags);
    const HRESULT hr = g_orig_present1(sc, sync_interval, flags, parameters);
    leave(e);
    return hr;
}

bool create_hook(const char* name, void* target, void* detour, void** original) {
    const MH_STATUS s = MH_CreateHook(target, detour, original);
    if (s != MH_OK) log_text(Level::Error, "MH_CreateHook %s: %s", name, MH_StatusToString(s));
    return s == MH_OK;
}

}  // namespace

bool install_dxgi_hooks() {
    SwapChainAddrs addrs;
    if (!locate_dxgi(&addrs, &g_locate_us)) return false;

    int hooked = 0;
    if (create_hook("Present", addrs.present, reinterpret_cast<void*>(&present_detour),
                    reinterpret_cast<void**>(&g_orig_present)))
        ++hooked;
    // Present1 shares the vtable; a different entry that points at the same code would collide.
    if (addrs.present1 && addrs.present1 != addrs.present &&
        create_hook("Present1", addrs.present1, reinterpret_cast<void*>(&present1_detour),
                    reinterpret_cast<void**>(&g_orig_present1)))
        ++hooked;
    if (!hooked) return false;

    g.enabled.store(true, std::memory_order_release);
    const MH_STATUS s = MH_EnableHook(MH_ALL_HOOKS);
    if (s != MH_OK) {
        g.enabled.store(false, std::memory_order_release);
        log_text(Level::Error, "MH_EnableHook: %s", MH_StatusToString(s));
        return false;
    }
    return true;
}

void remove_dxgi_hooks() {
    g.enabled.store(false, std::memory_order_release);
    MH_DisableHook(MH_ALL_HOOKS);
}

}  // namespace rec::hook
