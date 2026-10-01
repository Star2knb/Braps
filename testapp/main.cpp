// rec_testapp: a small D3D11 window app standing in for a game (recorder plan §14.1, X10).
//
// Every frame it draws, with no shaders (ClearView on rectangles):
//   - a frame counter as a 32-bit barcode of large black/white blocks along the top (it survives
//     scaling and near-lossless coding, so a recording can be checked frame by frame later),
//   - colour bars whose hue moves with the frame number and a square travelling across the window.
// The frame-time pattern is configurable, so freezes, spikes and jitter can be provoked on demand.
// Once a second it prints its own frame rate; recorder measurements are compared against that.
//
//   rec_testapp [--width W] [--height H] [--fps-cap N] [--vsync] [--present1]
//               [--pattern steady|jitter|spikes|freeze] [--freeze-ms N] [--seconds N] [--title T]
//               [--late-load-ms N]   (wait before the first Direct3D call; d3d11.dll is delay-loaded)
#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <windows.h>
#include <wrl/client.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

using Microsoft::WRL::ComPtr;

namespace {

struct Options {
    int width = 1280, height = 720;
    int fps_cap = 0;          // 0: no cap (vsync, if asked, still applies)
    bool vsync = false;
    bool present1 = false;    // call IDXGISwapChain1::Present1 instead of Present
    std::string pattern = "steady";
    int freeze_ms = 3000;
    int seconds = 0;          // 0: until the window is closed
    int late_load_ms = 0;     // wait this long before first touching Direct3D (it is delay-loaded)
    std::wstring title = L"rec_testapp";
};

int64_t g_freq = 0;
int64_t now_ticks() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}
double ms_between(int64_t a, int64_t b) { return double(b - a) * 1000.0 / double(g_freq); }

// Sleeps to an absolute QPC time: a high-resolution timer for the bulk, a spin for the last bit.
void sleep_until(HANDLE timer, int64_t target) {
    const double remaining_ms = ms_between(now_ticks(), target);
    if (remaining_ms > 2.0 && timer) {
        LARGE_INTEGER due;
        due.QuadPart = -LONGLONG((remaining_ms - 1.5) * 10000.0);  // 100 ns units, relative
        SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
        WaitForSingleObject(timer, INFINITE);
    }
    while (now_ticks() < target) YieldProcessor();
}

bool g_resized = false;
bool g_closed = false;
UINT g_new_width = 0, g_new_height = 0;

LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_SIZE:
        if (wp != SIZE_MINIMIZED) {
            g_new_width = LOWORD(lp);
            g_new_height = HIWORD(lp);
            g_resized = true;
        }
        return 0;
    case WM_CLOSE:
    case WM_DESTROY:
        g_closed = true;
        return 0;
    default: return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

struct Gfx {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext1> context;
    ComPtr<IDXGISwapChain1> swapchain;
    ComPtr<ID3D11RenderTargetView> rtv;
    UINT width = 0, height = 0;
};

bool make_rtv(Gfx* gfx) {
    ComPtr<ID3D11Texture2D> back;
    if (FAILED(gfx->swapchain->GetBuffer(0, IID_PPV_ARGS(&back)))) return false;
    return SUCCEEDED(gfx->device->CreateRenderTargetView(back.Get(), nullptr, &gfx->rtv));
}

bool init_gfx(HWND hwnd, int width, int height, Gfx* gfx) {
    ComPtr<ID3D11DeviceContext> context;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, ARRAYSIZE(levels),
                                 D3D11_SDK_VERSION, &gfx->device, nullptr, &context)))
        return false;
    if (FAILED(context.As(&gfx->context))) return false;

    ComPtr<IDXGIDevice> dxgi_device;
    ComPtr<IDXGIAdapter> adapter;
    ComPtr<IDXGIFactory2> factory;
    if (FAILED(gfx->device.As(&dxgi_device)) || FAILED(dxgi_device->GetAdapter(&adapter)) ||
        FAILED(adapter->GetParent(IID_PPV_ARGS(&factory))))
        return false;

    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = UINT(width);
    desc.Height = UINT(height);
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    if (FAILED(factory->CreateSwapChainForHwnd(gfx->device.Get(), hwnd, &desc, nullptr, nullptr, &gfx->swapchain)))
        return false;
    factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
    gfx->width = UINT(width);
    gfx->height = UINT(height);
    return make_rtv(gfx);
}

bool resize_gfx(Gfx* gfx, UINT width, UINT height) {
    if (!width || !height) return true;
    gfx->context->OMSetRenderTargets(0, nullptr, nullptr);
    gfx->rtv.Reset();
    if (FAILED(gfx->swapchain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0))) return false;
    gfx->width = width;
    gfx->height = height;
    return make_rtv(gfx);
}

void clear_rect(Gfx* gfx, const float color[4], LONG l, LONG t, LONG r, LONG b) {
    const D3D11_RECT rect{l, t, r, b};
    gfx->context->ClearView(gfx->rtv.Get(), color, &rect, 1);
}

void hue_to_rgb(float hue, float* rgba) {  // hue in [0, 1)
    const float h = hue * 6.0f;
    const float x = 1.0f - std::fabs(std::fmod(h, 2.0f) - 1.0f);
    float r = 0, g = 0, b = 0;
    if (h < 1) { r = 1; g = x; }
    else if (h < 2) { r = x; g = 1; }
    else if (h < 3) { g = 1; b = x; }
    else if (h < 4) { g = x; b = 1; }
    else if (h < 5) { r = x; b = 1; }
    else { r = 1; b = x; }
    rgba[0] = r * 0.8f + 0.1f;
    rgba[1] = g * 0.8f + 0.1f;
    rgba[2] = b * 0.8f + 0.1f;
    rgba[3] = 1.0f;
}

void draw_frame(Gfx* gfx, uint32_t frame) {
    const LONG w = LONG(gfx->width), h = LONG(gfx->height);
    const float black[4] = {0, 0, 0, 1}, white[4] = {1, 1, 1, 1};

    // Colour bars below the barcode, hue moving with the frame number.
    const LONG bar_top = h / 6;
    const int bars = 8;
    for (int i = 0; i < bars; ++i) {
        float color[4];
        hue_to_rgb(std::fmod(float(frame) * 0.004f + float(i) / float(bars), 1.0f), color);
        clear_rect(gfx, color, w * i / bars, bar_top, w * (i + 1) / bars, h);
    }
    // A square crossing the window (something that moves every frame).
    const LONG side = h / 8;
    const LONG x = LONG((frame * 7u) % uint32_t(w - side));
    const LONG y = bar_top + (h - bar_top - side) / 2 + LONG(std::sin(double(frame) * 0.05) * double(h) * 0.15);
    const float square[4] = {0.05f, 0.05f, 0.05f, 1};
    clear_rect(gfx, square, x, y, x + side, y + side);

    // Barcode: 32 blocks, most significant bit on the left, 1 = white.
    const LONG bar_h = h / 6;
    for (int bit = 0; bit < 32; ++bit) {
        const bool one = (frame >> (31 - bit)) & 1u;
        clear_rect(gfx, one ? white : black, w * bit / 32, 0, w * (bit + 1) / 32, bar_h);
    }
}

bool parse(int argc, wchar_t** argv, Options* o) {
    auto narrow = [](const wchar_t* s) {
        std::string r;
        for (; *s; ++s) r += char(*s);
        return r;
    };
    for (int i = 1; i < argc; ++i) {
        const std::string a = narrow(argv[i]);
        auto next = [&](const char* what) -> const wchar_t* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "rec_testapp: %s needs a value\n", what);
                return nullptr;
            }
            return argv[++i];
        };
        if (a == "--width" || a == "--height" || a == "--fps-cap" || a == "--freeze-ms" || a == "--seconds" || a == "--late-load-ms") {
            const wchar_t* v = next(a.c_str());
            if (!v) return false;
            const int n = _wtoi(v);
            if (n < 0 || ((a == "--width" || a == "--height") && n < 64)) {
                std::fprintf(stderr, "rec_testapp: bad value for %s\n", a.c_str());
                return false;
            }
            if (a == "--width") o->width = n;
            else if (a == "--height") o->height = n;
            else if (a == "--fps-cap") o->fps_cap = n;
            else if (a == "--freeze-ms") o->freeze_ms = n;
            else if (a == "--late-load-ms") o->late_load_ms = n;
            else o->seconds = n;
        } else if (a == "--pattern") {
            const wchar_t* v = next("--pattern");
            if (!v) return false;
            o->pattern = narrow(v);
            if (o->pattern != "steady" && o->pattern != "jitter" && o->pattern != "spikes" && o->pattern != "freeze") {
                std::fprintf(stderr, "rec_testapp: pattern must be steady, jitter, spikes or freeze\n");
                return false;
            }
        } else if (a == "--title") {
            const wchar_t* v = next("--title");
            if (!v) return false;
            o->title = v;
        } else if (a == "--vsync") {
            o->vsync = true;
        } else if (a == "--present1") {
            o->present1 = true;
        } else {
            std::fprintf(stderr, "rec_testapp: unknown option %s\n", a.c_str());
            return false;
        }
    }
    return true;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    Options opt;
    if (!parse(argc, argv, &opt)) return 2;
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    g_freq = f.QuadPart;

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"rec_testapp_window";
    RegisterClassExW(&wc);
    RECT rect{0, 0, opt.width, opt.height};
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, opt.title.c_str(), WS_OVERLAPPEDWINDOW | WS_VISIBLE, 100, 100,
                                rect.right - rect.left, rect.bottom - rect.top, nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd) {
        std::fprintf(stderr, "rec_testapp: cannot create the window (%lu)\n", GetLastError());
        return 1;
    }
    if (opt.late_load_ms) Sleep(DWORD(opt.late_load_ms));
    Gfx gfx;
    if (!init_gfx(hwnd, opt.width, opt.height, &gfx)) {
        std::fprintf(stderr, "rec_testapp: cannot create the D3D11 device and swap chain\n");
        return 1;
    }
    std::printf("rec_testapp pid %lu: D3D11 %dx%d, pattern %s%s%s%s\n", GetCurrentProcessId(), opt.width, opt.height,
                opt.pattern.c_str(), opt.vsync ? ", vsync" : "", opt.fps_cap ? ", capped" : "",
                opt.present1 ? ", Present1" : "");
    std::fflush(stdout);

    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    uint32_t rng = 12345;
    uint32_t frame = 0;
    const int64_t start = now_ticks();
    int64_t next_frame = start;
    int64_t last_report = start;
    uint32_t report_frames = 0;
    const int64_t cap_ticks = opt.fps_cap ? g_freq / opt.fps_cap : 0;
    bool frozen = false;

    while (!g_closed) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (g_closed) break;
        if (g_resized) {
            g_resized = false;
            if (!resize_gfx(&gfx, g_new_width, g_new_height)) {
                std::fprintf(stderr, "rec_testapp: ResizeBuffers failed\n");
                return 1;
            }
        }

        // The frame-time pattern.
        if (opt.pattern == "jitter") {
            rng = rng * 1664525u + 1013904223u;
            Sleep((rng >> 24) % 9);
        } else if (opt.pattern == "spikes" && frame && frame % 120 == 0) {
            Sleep(100);
        } else if (opt.pattern == "freeze" && frame == 300 && !frozen) {
            frozen = true;
            Sleep(DWORD(opt.freeze_ms));
        }

        gfx.context->OMSetRenderTargets(1, gfx.rtv.GetAddressOf(), nullptr);
        draw_frame(&gfx, frame);

        const UINT sync = opt.vsync ? 1 : 0;
        if (opt.present1) {
            DXGI_PRESENT_PARAMETERS params{};
            gfx.swapchain->Present1(sync, 0, &params);
        } else {
            gfx.swapchain->Present(sync, 0);
        }
        ++frame;
        ++report_frames;

        if (cap_ticks) {
            next_frame += cap_ticks;
            const int64_t t = now_ticks();
            if (next_frame < t - cap_ticks) next_frame = t;  // far behind: re-anchor
            sleep_until(timer, next_frame);
        }

        const int64_t t = now_ticks();
        if (t - last_report >= g_freq) {
            std::printf("frames=%u fps=%.1f\n", frame, double(report_frames) * double(g_freq) / double(t - last_report));
            std::fflush(stdout);
            report_frames = 0;
            last_report = t;
        }
        if (opt.seconds && ms_between(start, t) >= opt.seconds * 1000.0) break;
    }
    std::printf("rec_testapp: exit after %u frames\n", frame);
    if (timer) CloseHandle(timer);
    return 0;
}
