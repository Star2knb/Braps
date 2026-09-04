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

// Two staging textures per API, used as a ring rather than one texture
// re-used every frame. A single staging texture forces Map() to stall the
// CPU until the GPU finishes the CopyResource just issued a moment ago —
// measured at ~0.7-1.9ms/frame on real hardware (see PresentTimingAccum
// below), directly on the game's own render thread. With two textures,
// each Present() call issues CopyResource into THIS frame's slot, then
// Maps LAST frame's slot instead — by then the GPU has had a full frame's
// worth of time (usually >10ms) to finish that earlier copy, so the Map
// call typically returns immediately rather than blocking. This trades
// one frame of latency (the pushed frame is always one Present() behind)
// for removing the GPU-wait stall from the hot path — an acceptable
// tradeoff for a recording tool, which has no reason to need zero-latency
// capture. A process could (in principle, if unusually) have swapchains
// of either D3D10 or D3D11, so each API gets its own independent ring.
constexpr int kStagingRingSize = 2;

ComPtr<ID3D11Texture2D> g_stagingTexture11[kStagingRingSize];
UINT g_stagingWidth11 = 0;
UINT g_stagingHeight11 = 0;
int g_stagingWriteIndex11 = 0; // slot CopyResource targets THIS Present call
bool g_stagingHasPending11 = false; // false until the ring has been through one full cycle

ComPtr<ID3D10Texture2D> g_stagingTexture10[kStagingRingSize];
UINT g_stagingWidth10 = 0;
UINT g_stagingHeight10 = 0;
int g_stagingWriteIndex10 = 0;
bool g_stagingHasPending10 = false;

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
    if (g_stagingTexture11[0] && width == g_stagingWidth11 && height == g_stagingHeight11) {
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

    for (auto& tex : g_stagingTexture11) {
        tex.Reset();
        HRESULT hr = device->CreateTexture2D(&desc, nullptr, tex.GetAddressOf());
        if (FAILED(hr)) return false;
    }

    g_stagingWidth11 = width;
    g_stagingHeight11 = height;
    // A resize invalidates any in-flight copy queued against the old-sized
    // textures — restart the ring from empty rather than risk Mapping a
    // slot whose CopyResource targeted a texture that no longer exists.
    g_stagingWriteIndex11 = 0;
    g_stagingHasPending11 = false;
    return true;
}

// D3D10 equivalent of EnsureStagingTexture11. D3D10 has no separate device
// context — the device itself issues CopyResource/Map calls directly.
bool EnsureStagingTexture10(ID3D10Device* device, UINT width, UINT height, DXGI_FORMAT format) {
    if (g_stagingTexture10[0] && width == g_stagingWidth10 && height == g_stagingHeight10) {
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

    for (auto& tex : g_stagingTexture10) {
        tex.Reset();
        HRESULT hr = device->CreateTexture2D(&desc, nullptr, tex.GetAddressOf());
        if (FAILED(hr)) return false;
    }

    g_stagingWidth10 = width;
    g_stagingHeight10 = height;
    g_stagingWriteIndex10 = 0;
    g_stagingHasPending10 = false;
    return true;
}

// Per-second accumulators for HandlePresent11's timing breakdown, flushed
// to hook_log.txt roughly once a second — used to find where real time
// goes on the game's own render thread during readback, since this call
// executes inline inside the game's Present() and any cost here is cost
// the game itself pays. Not thread-safe by design: Present is only ever
// called from the game's single render thread, same as the rest of this
// file's globals.
struct PresentTimingAccum {
    double getBufferMs = 0, copyResourceMs = 0, mapMs = 0, rowCopyMs = 0, unmapMs = 0, pushMs = 0, totalMs = 0;
    int frames = 0;
    // Counts DXGI_ERROR_WAS_STILL_DRAWING from the non-blocking Map — the
    // ring's one-frame margin wasn't enough and this frame's capture was
    // skipped rather than stalling the game. Expected to stay at/near 0;
    // a consistently nonzero rate would mean the GPU is more than a full
    // frame behind on this hardware and the ring may need a 3rd slot.
    int mapStillDrawing = 0;
    std::chrono::steady_clock::time_point windowStart = std::chrono::steady_clock::now();
};
PresentTimingAccum g_presentTiming;

void FlushPresentTimingIfDue() {
    auto now = std::chrono::steady_clock::now();
    if (now - g_presentTiming.windowStart < std::chrono::seconds(1) || g_presentTiming.frames == 0) return;

    auto& t = g_presentTiming;
    LogHook("[PresentTiming] frames=" + std::to_string(t.frames) +
            " getBuffer=" + std::to_string(t.getBufferMs / t.frames) +
            "ms copyResource=" + std::to_string(t.copyResourceMs / t.frames) +
            "ms map=" + std::to_string(t.mapMs / t.frames) +
            "ms rowCopy=" + std::to_string(t.rowCopyMs / t.frames) +
            "ms unmap=" + std::to_string(t.unmapMs / t.frames) +
            "ms push=" + std::to_string(t.pushMs / t.frames) +
            "ms total=" + std::to_string(t.totalMs / t.frames) +
            "ms/frame stillDrawing=" + std::to_string(t.mapStillDrawing));

    t = PresentTimingAccum{};
}

void HandlePresent11(IDXGISwapChain* swapChain, ID3D11Device* device) {
    auto tStart = std::chrono::steady_clock::now();

    ComPtr<ID3D11Texture2D> backBuffer;
    if (FAILED(swapChain->GetBuffer(0, IID_PPV_ARGS(backBuffer.GetAddressOf())))) return;
    auto tGetBuffer = std::chrono::steady_clock::now();

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

    // Issue this frame's copy into the write slot — the GPU queues this
    // and returns essentially immediately; it does NOT wait for the copy
    // to finish. The result won't be read back until NEXT Present(), by
    // which point the GPU has had a full frame's worth of time to finish it.
    const int writeIndex = g_stagingWriteIndex11;
    const int readIndex = (writeIndex + 1) % kStagingRingSize; // slot copied last Present()
    context->CopyResource(g_stagingTexture11[writeIndex].Get(), backBuffer.Get());
    auto tCopyResource = std::chrono::steady_clock::now();

    g_stagingWriteIndex11 = readIndex;

    // First frame after (re)creating the ring: only one copy has ever been
    // issued, so readIndex's texture has never been written — skip capture
    // for this one frame rather than reading undefined staging memory.
    if (!g_stagingHasPending11) {
        g_stagingHasPending11 = true;
        return;
    }

    // Try the non-blocking Map first — the expected/fast path once the
    // ring has warmed up, since the GPU has had a full frame's time to
    // finish the copy. Measured on real hardware: this alone dropped the
    // vast majority of frames on one test (Minecraft, GPU evidently still
    // behind by more than one frame under that load) — DXGI_ERROR_WAS_
    // STILL_DRAWING is not the rare edge case a "whole extra frame of
    // margin" was expected to make it; it can be the common case under
    // real load. So this is a fast-path attempt, not a skip-on-failure:
    // falling back to a blocking Map (identical cost to the pre-rework
    // synchronous path) on WAS_STILL_DRAWING still guarantees every frame
    // gets delivered, same as before this rework, while frames that DO
    // win the race pay near-zero Map cost instead of the old ~1-2ms stall.
    D3D11_MAPPED_SUBRESOURCE mapped{};
    HRESULT mapHr = context->Map(g_stagingTexture11[readIndex].Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
    if (mapHr == DXGI_ERROR_WAS_STILL_DRAWING) {
        g_presentTiming.mapStillDrawing++;
        mapHr = context->Map(g_stagingTexture11[readIndex].Get(), 0, D3D11_MAP_READ, 0, &mapped);
    }
    auto tMap = std::chrono::steady_clock::now();
    if (FAILED(mapHr)) return;

    uint8_t* dst = RowContiguousScratch();
    CopyRowsWithOptionalSwizzle(dst, static_cast<const uint8_t*>(mapped.pData),
                                 bbDesc.Width, bbDesc.Height, mapped.RowPitch, NeedsSwizzle(bbDesc.Format));
    auto tRowCopy = std::chrono::steady_clock::now();

    context->Unmap(g_stagingTexture11[readIndex].Get(), 0);
    auto tUnmap = std::chrono::steady_clock::now();

    const size_t rowBytes = static_cast<size_t>(bbDesc.Width) * 4;
    g_channel->TryPush(dst, rowBytes * bbDesc.Height, bbDesc.Width, bbDesc.Height, NowMs());
    auto tPush = std::chrono::steady_clock::now();

    auto ms = [](std::chrono::steady_clock::time_point a, std::chrono::steady_clock::time_point b) {
        return std::chrono::duration<double, std::milli>(b - a).count();
    };
    g_presentTiming.getBufferMs += ms(tStart, tGetBuffer);
    g_presentTiming.copyResourceMs += ms(tGetBuffer, tCopyResource);
    g_presentTiming.mapMs += ms(tCopyResource, tMap);
    g_presentTiming.rowCopyMs += ms(tMap, tRowCopy);
    g_presentTiming.unmapMs += ms(tRowCopy, tUnmap);
    g_presentTiming.pushMs += ms(tUnmap, tPush);
    g_presentTiming.totalMs += ms(tStart, tPush);
    ++g_presentTiming.frames;
    FlushPresentTimingIfDue();
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

    // Same double-buffered ring as HandlePresent11 — see the comment on
    // g_stagingTexture11's declaration for why. D3D10 has no separate
    // D3D10_MAP_FLAG_DO_NOT_WAIT-style async check on this SDK's Map, but
    // D3D10_MAP_FLAG_DO_NOT_WAIT itself exists (device10.h), so the same
    // non-blocking safety net applies.
    const int writeIndex = g_stagingWriteIndex10;
    const int readIndex = (writeIndex + 1) % kStagingRingSize;
    device->CopyResource(g_stagingTexture10[writeIndex].Get(), backBuffer.Get());
    g_stagingWriteIndex10 = readIndex;

    if (!g_stagingHasPending10) {
        g_stagingHasPending10 = true;
        return;
    }

    // Fast-path attempt, falls back to blocking Map on WAS_STILL_DRAWING —
    // see the matching comment in HandlePresent11 for why this isn't a
    // skip-on-failure (dropped nearly every frame in real testing).
    D3D10_MAPPED_TEXTURE2D mapped{};
    HRESULT mapHr = g_stagingTexture10[readIndex]->Map(0, D3D10_MAP_READ, D3D10_MAP_FLAG_DO_NOT_WAIT, &mapped);
    if (mapHr == DXGI_ERROR_WAS_STILL_DRAWING) {
        mapHr = g_stagingTexture10[readIndex]->Map(0, D3D10_MAP_READ, 0, &mapped);
    }
    if (FAILED(mapHr)) return;

    uint8_t* dst = RowContiguousScratch();
    CopyRowsWithOptionalSwizzle(dst, static_cast<const uint8_t*>(mapped.pData),
                                 bbDesc.Width, bbDesc.Height, mapped.RowPitch, NeedsSwizzle(bbDesc.Format));
    g_stagingTexture10[readIndex]->Unmap(0);

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

    for (auto& tex : g_stagingTexture11) tex.Reset();
    for (auto& tex : g_stagingTexture10) tex.Reset();
}

} // namespace braps
