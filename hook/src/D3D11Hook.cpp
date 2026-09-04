#include "D3D11Hook.h"
#include "SharedFrameChannel.h"

#include <d3d10_1.h>
#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>
#include <cstdint>
#include <chrono>
#include <vector>
#include <fstream>

using Microsoft::WRL::ComPtr;

namespace braps {
namespace {

// The injected DLL has no console — this is the only practical way to see
// what happened inside the target process after the fact.
void LogHook(const std::string& message) {
    std::ofstream log(L"C:\\TempRecordings\\hook_log.txt", std::ios::app);
    log << message << "\n";
}

// IDXGISwapChain::Present is vtable slot 8 (after the 3 IUnknown +
// 5 IDXGIObject/IDXGIDeviceSubObject... actually: QueryInterface, AddRef,
// Release, SetPrivateData, SetPrivateDataInterface, GetPrivateData,
// GetParent, GetDevice, Present). Present is index 8, 0-based. This slot
// is identical for a swapchain regardless of whether the device behind it
// is D3D10 or D3D11 — both go through the same IDXGISwapChain interface,
// so one hook covers both APIs; only the backbuffer readback differs.
constexpr int kPresentVTableIndex = 8;

using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);

PresentFn g_originalPresent = nullptr;
void** g_swapChainVTable = nullptr;
DWORD g_originalProtect = 0;

// Separate staging texture + tracked size per API, since a process could
// (in principle, if unusually) have swapchains of either type, and the
// two APIs' texture types aren't interchangeable.
ComPtr<ID3D11Texture2D> g_stagingTexture11;
UINT g_stagingWidth11 = 0;
UINT g_stagingHeight11 = 0;

ComPtr<ID3D10Texture2D> g_stagingTexture10;
UINT g_stagingWidth10 = 0;
UINT g_stagingHeight10 = 0;

SharedFrameChannel* g_channel = nullptr;

uint64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

bool NeedsSwizzle(DXGI_FORMAT format) {
    return format == DXGI_FORMAT_R8G8B8A8_UNORM || format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
}

bool IsSupportedFormat(DXGI_FORMAT format) {
    return NeedsSwizzle(format) ||
           format == DXGI_FORMAT_B8G8R8A8_UNORM || format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
}

// Row-major scratch buffer used to de-stride mapped GPU memory before
// handing it to shared memory, and optionally swizzle R/B along the way.
// Shared between the D3D10 and D3D11 paths since only one is ever active
// for a given swapchain at a time.
uint8_t* RowContiguousScratch() {
    static std::vector<uint8_t> buffer(kMaxFrameBytes);
    return buffer.data();
}

// The shared memory contract (and everything downstream: Encoder, the
// DXGI/GDI capture paths) assumes BGRA byte order, matching what DXGI
// Desktop Duplication and GDI both produce natively. A game's actual
// swapchain format is commonly RGBA (DXGI_FORMAT_R8G8B8A8_UNORM) instead,
// which is byte-identical except red and blue are swapped — left
// unhandled, this reads as "colors are off" while everything else
// (shapes, motion, brightness) looks correct.
void CopyRowsWithOptionalSwizzle(uint8_t* dst, const uint8_t* src, UINT width, UINT height,
                                  UINT rowPitch, bool needsSwizzle) {
    const size_t rowBytes = static_cast<size_t>(width) * 4;
    for (UINT y = 0; y < height; ++y) {
        uint8_t* dstRow = dst + static_cast<size_t>(y) * rowBytes;
        const uint8_t* srcRow = src + static_cast<size_t>(y) * rowPitch;
        if (needsSwizzle) {
            for (UINT x = 0; x < width; ++x) {
                dstRow[x * 4 + 0] = srcRow[x * 4 + 2]; // B <- R
                dstRow[x * 4 + 1] = srcRow[x * 4 + 1]; // G
                dstRow[x * 4 + 2] = srcRow[x * 4 + 0]; // R <- B
                dstRow[x * 4 + 3] = srcRow[x * 4 + 3]; // A
            }
        } else {
            memcpy(dstRow, srcRow, rowBytes);
        }
    }
}

// Lazily (re)creates the D3D11 staging texture when the swapchain's
// backbuffer size changes (window resize, fullscreen toggle) so
// CopyResource always targets a correctly-sized destination.
bool EnsureStagingTexture11(ID3D11Device* device, UINT width, UINT height, DXGI_FORMAT format) {
    if (g_stagingTexture11 && width == g_stagingWidth11 && height == g_stagingHeight11) {
        return true;
    }

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

    g_stagingTexture11.Reset();
    HRESULT hr = device->CreateTexture2D(&desc, nullptr, g_stagingTexture11.GetAddressOf());
    if (FAILED(hr)) return false;

    g_stagingWidth11 = width;
    g_stagingHeight11 = height;
    return true;
}

// D3D10 equivalent of EnsureStagingTexture11. D3D10 has no separate device
// context — the device itself issues CopyResource/Map calls directly.
bool EnsureStagingTexture10(ID3D10Device* device, UINT width, UINT height, DXGI_FORMAT format) {
    if (g_stagingTexture10 && width == g_stagingWidth10 && height == g_stagingHeight10) {
        return true;
    }

    D3D10_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D10_USAGE_STAGING;
    desc.CPUAccessFlags = D3D10_CPU_ACCESS_READ;

    g_stagingTexture10.Reset();
    HRESULT hr = device->CreateTexture2D(&desc, nullptr, g_stagingTexture10.GetAddressOf());
    if (FAILED(hr)) return false;

    g_stagingWidth10 = width;
    g_stagingHeight10 = height;
    return true;
}

void HandlePresent11(IDXGISwapChain* swapChain, ID3D11Device* device) {
    ComPtr<ID3D11Texture2D> backBuffer;
    if (FAILED(swapChain->GetBuffer(0, IID_PPV_ARGS(backBuffer.GetAddressOf())))) return;

    D3D11_TEXTURE2D_DESC bbDesc{};
    backBuffer->GetDesc(&bbDesc);

    static bool loggedFormat = false;
    if (!loggedFormat) {
        loggedFormat = true;
        LogHook("[PresentHook/D3D11] Backbuffer format=" + std::to_string(bbDesc.Format) +
                " size=" + std::to_string(bbDesc.Width) + "x" + std::to_string(bbDesc.Height));
    }

    if (!IsSupportedFormat(bbDesc.Format) || bbDesc.Width > kMaxWidth || bbDesc.Height > kMaxHeight ||
        !EnsureStagingTexture11(device, bbDesc.Width, bbDesc.Height, bbDesc.Format)) {
        return;
    }

    ComPtr<ID3D11DeviceContext> context;
    device->GetImmediateContext(context.GetAddressOf());

    context->CopyResource(g_stagingTexture11.Get(), backBuffer.Get());

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(g_stagingTexture11.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return;

    uint8_t* dst = RowContiguousScratch();
    CopyRowsWithOptionalSwizzle(dst, static_cast<const uint8_t*>(mapped.pData),
                                 bbDesc.Width, bbDesc.Height, mapped.RowPitch, NeedsSwizzle(bbDesc.Format));
    context->Unmap(g_stagingTexture11.Get(), 0);

    const size_t rowBytes = static_cast<size_t>(bbDesc.Width) * 4;
    g_channel->TryPush(dst, rowBytes * bbDesc.Height, bbDesc.Width, bbDesc.Height, NowMs());
}

// D3D10 equivalent of HandlePresent11 — same shape, but every call goes
// straight to the device (no ID3D10DeviceContext exists; the device IS
// the immediate context in D3D10's model).
void HandlePresent10(IDXGISwapChain* swapChain, ID3D10Device* device) {
    ComPtr<ID3D10Texture2D> backBuffer;
    if (FAILED(swapChain->GetBuffer(0, IID_PPV_ARGS(backBuffer.GetAddressOf())))) return;

    D3D10_TEXTURE2D_DESC bbDesc{};
    backBuffer->GetDesc(&bbDesc);

    static bool loggedFormat = false;
    if (!loggedFormat) {
        loggedFormat = true;
        LogHook("[PresentHook/D3D10] Backbuffer format=" + std::to_string(bbDesc.Format) +
                " size=" + std::to_string(bbDesc.Width) + "x" + std::to_string(bbDesc.Height));
    }

    if (!IsSupportedFormat(bbDesc.Format) || bbDesc.Width > kMaxWidth || bbDesc.Height > kMaxHeight ||
        !EnsureStagingTexture10(device, bbDesc.Width, bbDesc.Height, bbDesc.Format)) {
        return;
    }

    device->CopyResource(g_stagingTexture10.Get(), backBuffer.Get());

    D3D10_MAPPED_TEXTURE2D mapped{};
    if (FAILED(g_stagingTexture10->Map(0, D3D10_MAP_READ, 0, &mapped))) return;

    uint8_t* dst = RowContiguousScratch();
    CopyRowsWithOptionalSwizzle(dst, static_cast<const uint8_t*>(mapped.pData),
                                 bbDesc.Width, bbDesc.Height, mapped.RowPitch, NeedsSwizzle(bbDesc.Format));
    g_stagingTexture10->Unmap(0);

    const size_t rowBytes = static_cast<size_t>(bbDesc.Width) * 4;
    g_channel->TryPush(dst, rowBytes * bbDesc.Height, bbDesc.Width, bbDesc.Height, NowMs());
}

// The detour: runs on every real Present() call from the game, before
// handing off to the original implementation. Keep this path as cheap as
// possible — it executes on the game's own render thread, so any added
// latency here is latency the game itself feels.
HRESULT STDMETHODCALLTYPE HookedPresent(IDXGISwapChain* swapChain, UINT syncInterval, UINT flags) {
    // Skip all GPU readback work (GetBuffer/CopyResource/Map/row-copy)
    // entirely unless Braps.exe is actually recording right now. Checking
    // this single atomic is essentially free; doing the full readback on
    // every Present call regardless — which is what happened before this
    // check existed — cost a measured ~10fps in the game just from being
    // injected, even while idle and not recording anything.
    if (g_channel && g_channel->IsValid() &&
        g_channel->Header().consumerWantsFrames.load(std::memory_order_acquire)) {
        // Detect which API actually backs this swapchain by asking for
        // each device interface in turn — a swapchain only succeeds
        // QueryInterface-ing its own real device type, so exactly one of
        // these two branches ever does real work for a given process.
        ComPtr<ID3D11Device> device11;
        if (SUCCEEDED(swapChain->GetDevice(IID_PPV_ARGS(device11.GetAddressOf())))) {
            HandlePresent11(swapChain, device11.Get());
        } else {
            ComPtr<ID3D10Device> device10;
            if (SUCCEEDED(swapChain->GetDevice(IID_PPV_ARGS(device10.GetAddressOf())))) {
                HandlePresent10(swapChain, device10.Get());
            }
        }
    }

    return g_originalPresent(swapChain, syncInterval, flags);
}

// Creates a throwaway D3D11 device+swapchain solely to read the real
// Present function pointer out of its vtable, then destroys it. This is
// the standard technique: you can't get a swapchain's vtable without
// first creating one, but any swapchain instance (even a dummy hidden
// window's) shares the same vtable as the game's real one, since it's
// determined by the DXGI runtime implementation, not the specific
// instance — and critically, IDXGISwapChain::Present's vtable slot is
// identical whether the backing device is D3D10 or D3D11, so a D3D11
// dummy device is sufficient to find the hook point for both APIs.
bool GetRealPresentAddress(void** outVTable, PresentFn* outOriginal) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"BrapsHookDummyWindow";
    RegisterClassExW(&wc);

    HWND dummyWindow = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW,
                                        0, 0, 100, 100, nullptr, nullptr, wc.hInstance, nullptr);
    if (!dummyWindow) return false;

    DXGI_SWAP_CHAIN_DESC scDesc{};
    scDesc.BufferCount = 1;
    scDesc.BufferDesc.Width = 100;
    scDesc.BufferDesc.Height = 100;
    scDesc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    scDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scDesc.OutputWindow = dummyWindow;
    scDesc.SampleDesc.Count = 1;
    scDesc.Windowed = TRUE;

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGISwapChain> swapChain;
    D3D_FEATURE_LEVEL featureLevel;

    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        nullptr, 0, D3D11_SDK_VERSION, &scDesc,
        swapChain.GetAddressOf(), device.GetAddressOf(), &featureLevel, context.GetAddressOf());

    bool ok = false;
    if (SUCCEEDED(hr)) {
        void** vtable = *reinterpret_cast<void***>(swapChain.Get());
        *outVTable = vtable;
        *outOriginal = reinterpret_cast<PresentFn>(vtable[kPresentVTableIndex]);
        ok = true;
    }

    DestroyWindow(dummyWindow);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return ok;
}

} // namespace

bool InstallD3D11Hook() {
    void* vtable = nullptr;
    PresentFn original = nullptr;
    if (!GetRealPresentAddress(&vtable, &original)) {
        return false;
    }

    g_channel = new SharedFrameChannel();
    if (!g_channel->CreateAsProducer()) {
        delete g_channel;
        g_channel = nullptr;
        return false;
    }
    g_channel->Header().hookProcessId.store(GetCurrentProcessId(), std::memory_order_relaxed);
    g_channel->Header().hookAttached.store(true, std::memory_order_release);

    g_originalPresent = original;
    g_swapChainVTable = static_cast<void**>(vtable);

    // Patch the vtable slot directly rather than an inline trampoline —
    // simpler and sufficient here since we only need to intercept calls
    // made through this vtable pointer, not raw calls to the function
    // address from elsewhere.
    if (!VirtualProtect(&g_swapChainVTable[kPresentVTableIndex], sizeof(void*),
                         PAGE_READWRITE, &g_originalProtect)) {
        return false;
    }
    g_swapChainVTable[kPresentVTableIndex] = reinterpret_cast<void*>(&HookedPresent);
    DWORD dummy;
    VirtualProtect(&g_swapChainVTable[kPresentVTableIndex], sizeof(void*), g_originalProtect, &dummy);

    return true;
}

void RemoveD3D11Hook() {
    if (g_swapChainVTable && g_originalPresent) {
        DWORD oldProtect;
        if (VirtualProtect(&g_swapChainVTable[kPresentVTableIndex], sizeof(void*), PAGE_READWRITE, &oldProtect)) {
            g_swapChainVTable[kPresentVTableIndex] = reinterpret_cast<void*>(g_originalPresent);
            DWORD dummy;
            VirtualProtect(&g_swapChainVTable[kPresentVTableIndex], sizeof(void*), oldProtect, &dummy);
        }
    }

    if (g_channel) {
        g_channel->Header().hookAttached.store(false, std::memory_order_release);
        delete g_channel;
        g_channel = nullptr;
    }

    g_stagingTexture11.Reset();
    g_stagingTexture10.Reset();
}

} // namespace braps
