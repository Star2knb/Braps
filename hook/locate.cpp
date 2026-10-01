#include "locate.h"

#include <windows.h>
#include <psapi.h>

#include "hlog.h"
#include "hook_state.h"
#include "kiero_wrap.hpp"
#include "vtable_indices.h"

namespace rec::hook {
namespace {

using LocateFn = kiero::Error (*)(void*, void*);

// kiero2 calls into the graphics driver; a crash there must disable this API, not the game (§4.6.2).
// No C++ objects with destructors may live in a function with __try, hence the separate function.
bool locate_guarded(LocateFn fn, void* out, kiero::Error* result, DWORD* exception) {
    __try {
        *result = fn(nullptr, out);
        return true;
    } __except (*exception = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Reads vtable entry `index` after the checks of §4.6.3: in range, non-null, inside the module.
void* checked_entry(const std::vector<void*>& methods, int index, const wchar_t* module_name, const char* api) {
    if (index < 0 || size_t(index) >= methods.size() || !methods[size_t(index)]) {
        log_event(Ev::VtablePointerOutsideModule, "%s[%d] missing (%zu entries)", api, index, methods.size());
        return nullptr;
    }
    void* p = methods[size_t(index)];
    if (!pointer_in_module(module_name, p)) {
        log_event(Ev::VtablePointerOutsideModule, "%s[%d] %p not in module", api, index, p);
        return nullptr;
    }
    return p;
}

}  // namespace

bool pointer_in_module(const wchar_t* module_name, const void* p) {
    HMODULE m = GetModuleHandleW(module_name);
    MODULEINFO info{};
    if (!m || !GetModuleInformation(GetCurrentProcess(), m, &info, sizeof(info))) return false;
    const auto base = reinterpret_cast<uintptr_t>(info.lpBaseOfDll);
    const auto addr = reinterpret_cast<uintptr_t>(p);
    return addr >= base && addr < base + info.SizeOfImage;
}

bool locate_dxgi(SwapChainAddrs* out, uint64_t* locate_us) {
    const uint64_t t0 = qpc();
    kiero::D3D11Output output;
    kiero::Error error = kiero::Error_Unknown;
    DWORD exception = 0;
    const bool ran = locate_guarded(&kiero::locate<kiero::Implementation_D3D11>, &output, &error, &exception);
    *locate_us = (qpc() - t0) * 1000000ull / uint64_t(g.qpc_freq);

    if (!ran) {
        log_event(Ev::LocateFailed, "D3D11 exception 0x%08lX after %llu us", exception, *locate_us);
        return false;
    }
    if (error != kiero::Error_Nil) {
        log_event(Ev::LocateFailed, "D3D11 %s (%d) after %llu us", kiero_error_text(error), error, *locate_us);
        return false;
    }

    // Never trust .size(): kiero2 reads until a null entry, so the vector can end with junk (§4.6.3).
    out->present = checked_entry(output.swapchain_methods, vt::kSwapChainPresent, L"dxgi.dll", "DXGI");
    out->present1 = checked_entry(output.swapchain_methods, vt::kSwapChain1Present1, L"dxgi.dll", "DXGI");
    return out->present != nullptr;
}

}  // namespace rec::hook
