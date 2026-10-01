// The only place kiero2 headers are included (recorder plan §4.6.1). Its Implementation_* numbers
// depend on include order, so the order is fixed here and must match the slots given to the kiero2
// sources in third_party/CMakeLists.txt: D3D9, D3D11, D3D12 (only the enabled ones). Today: D3D11.
#pragma once

#include "kiero_config.h"  // before kiero.hpp: defines KIERO_DBG_MSG / KIERO_ASSERT

#include <kiero.hpp>
#include <kiero_d3d11.hpp>

// kiero2's error numbers as the log text uses them (checked against the pinned commit, 8f57dd9).
static_assert(kiero::Error_Nil == 0 && kiero::Error_ModuleNotFound == 2 && kiero::Error_MethodNotFound == 3);

namespace rec::hook {

inline const char* kiero_error_text(int error) {
    switch (error) {
    case kiero::Error_Nil: return "no error";
    case kiero::Error_Unknown: return "unknown";
    case kiero::Error_ModuleNotFound: return "module not loaded";
    case kiero::Error_MethodNotFound: return "export not found";
    case kiero::Error_D3D11_CreateDXGIFactoryFailed: return "CreateDXGIFactory failed";
    case kiero::Error_D3D11_EnumAdaptersFailed: return "EnumAdapters failed";
    case kiero::Error_D3D11_CreateDeviceAndSwapChainFailed: return "dummy device creation failed";
    default: return "exception while locating";
    }
}

}  // namespace rec::hook
