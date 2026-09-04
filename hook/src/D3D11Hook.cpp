#include "D3D11Hook.h"
#include "SharedFrameChannel.h"

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
// GetParent, GetDevice, Present). Present is index 8, 0-based.
constexpr int kPresentVTableIndex = 8;

using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);

PresentFn g_originalPresent = nullptr;
void** g_swapChainVTable = nullptr;
DWORD g_originalProtect = 0;

ComPtr<ID3D11Device> g_capturedDevice;
ComPtr<ID3D11DeviceContext> g_capturedContext;
ComPtr<ID3D11Texture2D> g_stagingTexture;
UINT g_stagingWidth = 0;
UINT g_stagingHeight = 0;

SharedFrameChannel* g_channel = nullptr;

uint64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

// Lazily (re)creates the staging texture when the swapchain's backbuffer
// size changes (window resize, fullscreen toggle) so CopyResource always
// targets a correctly-sized destination.
bool EnsureStagingTexture(ID3D11Device* device, UINT width, UINT height, DXGI_FORMAT format) {
    if (g_stagingTexture && width == g_stagingWidth && height == g_stagingHeight) {
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

    g_stagingTexture.Reset();
    HRESULT hr = device->CreateTexture2D(&desc, nullptr, g_stagingTexture.GetAddressOf());
    if (FAILED(hr)) return false;

    g_stagingWidth = width;
    g_stagingHeight = height;
    return true;
}

// The detour: runs on every real Present() call from the game, before
// handing off to the original implementation. Keep this path as cheap as
// possible — it executes on the game's own render thread, so any added
// latency here is latency the game itself feels.
HRESULT STDMETHODCALLTYPE HookedPresent(IDXGISwapChain* swapChain, UINT syncInterval, UINT flags) {
    if (g_channel && g_channel->IsValid()) {
        ComPtr<ID3D11Texture2D> backBuffer;
        if (SUCCEEDED(swapChain->GetBuffer(0, IID_PPV_ARGS(backBuffer.GetAddressOf())))) {
            ComPtr<ID3D11Device> device;
            backBuffer->GetDevice(device.GetAddressOf());

            D3D11_TEXTURE2D_DESC bbDesc{};
            backBuffer->GetDesc(&bbDesc);

            // The shared memory contract (and everything downstream:
            // Encoder, the DXGI/GDI capture paths) assumes BGRA byte
            // order, matching what DXGI Desktop Duplication and GDI both
            // produce natively. A game's actual swapchain format is
            // commonly RGBA (DXGI_FORMAT_R8G8B8A8_UNORM) instead, which is
            // byte-identical except red and blue are swapped — left
            // unhandled, this reads as "colors are off" while everything
            // else (shapes, motion, brightness) looks correct.
            const bool needsSwizzle =
                bbDesc.Format == DXGI_FORMAT_R8G8B8A8_UNORM ||
                bbDesc.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
            const bool isSupportedFormat = needsSwizzle ||
                bbDesc.Format == DXGI_FORMAT_B8G8R8A8_UNORM ||
                bbDesc.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;

            static bool loggedFormat = false;
            if (!loggedFormat) {
                loggedFormat = true;
                LogHook("[D3D11Hook] Backbuffer format=" + std::to_string(bbDesc.Format) +
                        " size=" + std::to_string(bbDesc.Width) + "x" + std::to_string(bbDesc.Height) +
                        " swizzle=" + (needsSwizzle ? "yes" : "no") +
                        " supported=" + (isSupportedFormat ? "yes" : "no"));
            }

            if (isSupportedFormat && bbDesc.Width <= kMaxWidth && bbDesc.Height <= kMaxHeight &&
                EnsureStagingTexture(device.Get(), bbDesc.Width, bbDesc.Height, bbDesc.Format)) {
                ComPtr<ID3D11DeviceContext> context;
                device->GetImmediateContext(context.GetAddressOf());

                auto copyStart = std::chrono::steady_clock::now();
                context->CopyResource(g_stagingTexture.Get(), backBuffer.Get());
                double copyMs = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - copyStart).count();

                auto mapStart = std::chrono::steady_clock::now();
                D3D11_MAPPED_SUBRESOURCE mapped{};
                HRESULT mapHr = context->Map(g_stagingTexture.Get(), 0, D3D11_MAP_READ, 0, &mapped);
                double mapMs = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - mapStart).count();

                if (SUCCEEDED(mapHr)) {
                    // Row-major scratch buffer to de-stride the mapped GPU
                    // memory before handing it to shared memory. Heap
                    // allocated once (not thread_local/stack) since ~8MB
                    // is too large for either safely, and Present() always
                    // runs on the game's own render thread anyway.
                    static std::vector<uint8_t> rowContiguous(kMaxFrameBytes);
                    const size_t rowBytes = static_cast<size_t>(bbDesc.Width) * 4;
                    uint8_t* dst = rowContiguous.data();
                    const uint8_t* src = static_cast<const uint8_t*>(mapped.pData);

                    auto copyLoopStart = std::chrono::steady_clock::now();
                    for (UINT y = 0; y < bbDesc.Height; ++y) {
                        uint8_t* dstRow = dst + static_cast<size_t>(y) * rowBytes;
                        const uint8_t* srcRow = src + static_cast<size_t>(y) * mapped.RowPitch;
                        if (needsSwizzle) {
                            for (UINT x = 0; x < bbDesc.Width; ++x) {
                                dstRow[x * 4 + 0] = srcRow[x * 4 + 2]; // B <- R
                                dstRow[x * 4 + 1] = srcRow[x * 4 + 1]; // G
                                dstRow[x * 4 + 2] = srcRow[x * 4 + 0]; // R <- B
                                dstRow[x * 4 + 3] = srcRow[x * 4 + 3]; // A
                            }
                        } else {
                            memcpy(dstRow, srcRow, rowBytes);
                        }
                    }
                    double copyLoopMs = std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - copyLoopStart).count();
                    context->Unmap(g_stagingTexture.Get(), 0);

                    g_channel->TryPush(rowContiguous.data(), rowBytes * bbDesc.Height, bbDesc.Width, bbDesc.Height, NowMs());

                    static int frameCounter = 0;
                    if (++frameCounter % 60 == 0) {
                        LogHook("[D3D11Hook] copy=" + std::to_string(copyMs) + "ms map=" + std::to_string(mapMs) +
                                "ms copyLoop=" + std::to_string(copyLoopMs) + "ms (frame " + std::to_string(frameCounter) + ")");
                    }
                }
            }
        }
    }

    return g_originalPresent(swapChain, syncInterval, flags);
}

// Creates a throwaway device+swapchain solely to read the real Present
// function pointer out of its vtable, then destroys it. This is the
// standard technique: you can't get a swapchain's vtable without first
// creating one, but any swapchain instance (even a dummy hidden window's)
// shares the same vtable as the game's real one, since it's determined by
// the D3D11/DXGI runtime implementation, not the specific instance.
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

    g_capturedDevice.Reset();
    g_capturedContext.Reset();
    g_stagingTexture.Reset();
}

} // namespace braps
