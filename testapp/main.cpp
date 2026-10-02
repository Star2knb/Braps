// rec_testapp: a small D3D11 or OpenGL window app standing in for a game (recorder plan §14.1, X10).
//
// Every frame it draws, with no shaders (ClearView on rectangles; glScissor + glClear with --gl):
//   - a frame counter as a 32-bit barcode of large black/white blocks along the top (it survives
//     scaling and near-lossless coding, so a recording can be checked frame by frame later),
//   - colour bars whose hue moves with the frame number and a square travelling across the window.
// The frame-time pattern is configurable, so freezes, spikes and jitter can be provoked on demand.
// Once a second it prints its own frame rate; recorder measurements are compared against that.
//
//   rec_testapp [--width W] [--height H] [--fps-cap N] [--vsync] [--present1] [--fullscreen]
//               [--pattern steady|jitter|spikes|freeze] [--freeze-ms N] [--seconds N] [--title T]
//               [--noise N]          (N random rectangles per frame: hard-to-compress content)
//               [--late-load-ms N]   (wait before the first Direct3D call; d3d11.dll is delay-loaded)
//               [--gl]               (OpenGL instead of Direct3D 11; d3d11.dll and dxgi.dll are never loaded)
//               [--wgl]              (with --gl: swap through opengl32's wglSwapBuffers instead of gdi32's SwapBuffers)
//               [--core]             (with --gl: a 3.3 core-profile context)
//               [--msaa N]           (with --gl: an N-sample default framebuffer)
//               [--state-check]      (with --gl: leave unusual GL state set at every swap and verify it afterwards)
//               [--offscreen]        (a real, visible window, but parked off the desktop and never activated: tests that
//                                     must not disturb whatever the user is doing; a minimised window has no client area)
#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <windows.h>
#include <wrl/client.h>

#include <GL/gl.h>

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
    bool fullscreen = false;  // exclusive fullscreen (SetFullscreenState)
    int noise = 0;            // random small rectangles per frame: video that compresses like a game, not like flat colour
    std::string pattern = "steady";
    int freeze_ms = 3000;
    int seconds = 0;          // 0: until the window is closed
    int late_load_ms = 0;     // wait this long before first touching Direct3D (it is delay-loaded)
    bool gl = false;          // OpenGL instead of Direct3D 11
    bool wgl_swap = false;    // opengl32's wglSwapBuffers instead of gdi32's SwapBuffers
    bool core = false;        // 3.3 core profile
    int msaa = 0;             // samples of the default framebuffer
    bool state_check = false; // set unusual GL state at every swap, verify it is unchanged after
    bool offscreen = false;   // parked at (-20000, -20000), shown without activation
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

// ---- OpenGL ------------------------------------------------------------------------------------
// Only what the test needs. Functions newer than GL 1.1 come from wglGetProcAddress.
constexpr GLenum kPackBuffer = 0x88EB, kPackBufferBinding = 0x88ED, kFramebufferSrgb = 0x8DB9, kReadFramebufferBinding = 0x8CAA,
                 kDrawFramebufferBinding = 0x8CA6, kColorWritemask = 0x0C23, kPackRowLength = 0x0D02, kPackAlignment = 0x0D05, kReadBuffer = 0x0C02,
                 kSamples = 0x80A9;
constexpr int WGL_DRAW_TO_WINDOW_ARB = 0x2001, WGL_SUPPORT_OPENGL_ARB = 0x2010, WGL_DOUBLE_BUFFER_ARB = 0x2011, WGL_PIXEL_TYPE_ARB = 0x2013,
              WGL_TYPE_RGBA_ARB = 0x202B, WGL_COLOR_BITS_ARB = 0x2014, WGL_DEPTH_BITS_ARB = 0x2022, WGL_SAMPLE_BUFFERS_ARB = 0x2041,
              WGL_SAMPLES_ARB = 0x2042, WGL_CONTEXT_MAJOR_VERSION_ARB = 0x2091, WGL_CONTEXT_MINOR_VERSION_ARB = 0x2092,
              WGL_CONTEXT_PROFILE_MASK_ARB = 0x9126, WGL_CONTEXT_CORE_PROFILE_BIT_ARB = 1;

extern "C" __declspec(dllimport) BOOL WINAPI wglSwapBuffers(HDC);  // opengl32.lib has it; the SDK header does not declare it

using ChoosePixelFormatArb = BOOL(WINAPI*)(HDC, const int*, const FLOAT*, UINT, int*, UINT*);
using CreateContextAttribsArb = HGLRC(WINAPI*)(HDC, HGLRC, const int*);
using SwapIntervalExt = BOOL(WINAPI*)(int);
using GenBuffersFn = void(APIENTRY*)(GLsizei, GLuint*);
using BindBufferFn = void(APIENTRY*)(GLenum, GLuint);

struct GlState {
    HDC dc = nullptr;
    HGLRC rc = nullptr;
    GenBuffersFn gen_buffers = nullptr;
    BindBufferFn bind_buffer = nullptr;
    GLuint sentinel_buffer = 0;
    bool wgl_swap = false;
    bool state_check = false;
    UINT width = 0, height = 0;
};
GlState g_gl;

bool gl_context_for(HWND hwnd, const Options& opt) {
    // A throwaway window and legacy context first: the extension functions can only be fetched with a context current.
    ChoosePixelFormatArb choose = nullptr;
    CreateContextAttribsArb create_attribs = nullptr;
    SwapIntervalExt swap_interval = nullptr;
    PIXELFORMATDESCRIPTOR pfd{};
    pfd.nSize = sizeof(pfd);
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.cDepthBits = 24;
    pfd.iLayerType = PFD_MAIN_PLANE;
    {
        HWND dummy = CreateWindowExW(0, L"STATIC", L"", WS_OVERLAPPED, 0, 0, 64, 64, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        HDC dc = dummy ? GetDC(dummy) : nullptr;
        const int format = dc ? ChoosePixelFormat(dc, &pfd) : 0;
        if (dc && format && SetPixelFormat(dc, format, &pfd)) {
            HGLRC rc = wglCreateContext(dc);
            if (rc && wglMakeCurrent(dc, rc)) {
                choose = reinterpret_cast<ChoosePixelFormatArb>(wglGetProcAddress("wglChoosePixelFormatARB"));
                create_attribs = reinterpret_cast<CreateContextAttribsArb>(wglGetProcAddress("wglCreateContextAttribsARB"));
                swap_interval = reinterpret_cast<SwapIntervalExt>(wglGetProcAddress("wglSwapIntervalEXT"));
                wglMakeCurrent(nullptr, nullptr);
            }
            if (rc) wglDeleteContext(rc);
        }
        if (dc) ReleaseDC(dummy, dc);
        if (dummy) DestroyWindow(dummy);
    }

    HDC dc = GetDC(hwnd);
    int format = 0;
    if (opt.msaa > 0 && choose) {
        const int attribs[] = {WGL_DRAW_TO_WINDOW_ARB, 1, WGL_SUPPORT_OPENGL_ARB, 1, WGL_DOUBLE_BUFFER_ARB, 1, WGL_PIXEL_TYPE_ARB, WGL_TYPE_RGBA_ARB,
                               WGL_COLOR_BITS_ARB, 32, WGL_DEPTH_BITS_ARB, 24, WGL_SAMPLE_BUFFERS_ARB, 1, WGL_SAMPLES_ARB, opt.msaa, 0};
        UINT count = 0;
        if (!choose(dc, attribs, nullptr, 1, &format, &count) || count == 0) format = 0;
    }
    if (!format) format = ChoosePixelFormat(dc, &pfd);
    if (!format || !SetPixelFormat(dc, format, &pfd)) return false;
    HGLRC rc = nullptr;
    if (opt.core && create_attribs) {
        const int attribs[] = {WGL_CONTEXT_MAJOR_VERSION_ARB, 3, WGL_CONTEXT_MINOR_VERSION_ARB, 3, WGL_CONTEXT_PROFILE_MASK_ARB, WGL_CONTEXT_CORE_PROFILE_BIT_ARB, 0};
        rc = create_attribs(dc, nullptr, attribs);
    }
    if (!rc) rc = wglCreateContext(dc);
    if (!rc || !wglMakeCurrent(dc, rc)) return false;
    if (swap_interval) swap_interval(opt.vsync ? 1 : 0);
    g_gl.dc = dc;
    g_gl.rc = rc;
    g_gl.wgl_swap = opt.wgl_swap;
    g_gl.state_check = opt.state_check;
    g_gl.gen_buffers = reinterpret_cast<GenBuffersFn>(wglGetProcAddress("glGenBuffers"));
    g_gl.bind_buffer = reinterpret_cast<BindBufferFn>(wglGetProcAddress("glBindBuffer"));
    RECT rect;
    GetClientRect(hwnd, &rect);
    g_gl.width = UINT(rect.right);
    g_gl.height = UINT(rect.bottom);
    return true;
}

void gl_clear_rect(const float color[4], LONG l, LONG t, LONG r, LONG b) {
    glEnable(GL_SCISSOR_TEST);
    glScissor(l, GLint(g_gl.height) - b, r - l, b - t);  // GL's origin is the bottom left
    glClearColor(color[0], color[1], color[2], color[3]);
    glClear(GL_COLOR_BUFFER_BIT);
}

// The state the capture has to leave as it found it, set to unusual values just before the swap.
void gl_set_sentinel() {
    if (g_gl.gen_buffers && !g_gl.sentinel_buffer) g_gl.gen_buffers(1, &g_gl.sentinel_buffer);
    glEnable(GL_SCISSOR_TEST);
    glScissor(3, 5, 17, 19);
    glColorMask(GL_TRUE, GL_FALSE, GL_TRUE, GL_TRUE);
    glPixelStorei(GL_PACK_ROW_LENGTH, 7);
    glPixelStorei(GL_PACK_ALIGNMENT, 2);
    glEnable(kFramebufferSrgb);
    glReadBuffer(GL_FRONT);
    if (g_gl.bind_buffer) g_gl.bind_buffer(kPackBuffer, g_gl.sentinel_buffer);
}

// Compares after the swap and puts the defaults back. Returns the first difference, or null.
const char* gl_check_sentinel() {
    const char* problem = nullptr;
    GLint v[4] = {0, 0, 0, 0};
    GLboolean mask[4] = {1, 1, 1, 1};
    if (!glIsEnabled(GL_SCISSOR_TEST)) problem = "scissor test was switched off";
    glGetIntegerv(GL_SCISSOR_BOX, v);
    if (!problem && (v[0] != 3 || v[1] != 5 || v[2] != 17 || v[3] != 19)) problem = "scissor box changed";
    glGetBooleanv(kColorWritemask, mask);
    if (!problem && (!mask[0] || mask[1] || !mask[2] || !mask[3])) problem = "colour mask changed";
    glGetIntegerv(kPackRowLength, v);
    if (!problem && v[0] != 7) problem = "GL_PACK_ROW_LENGTH changed";
    glGetIntegerv(kPackAlignment, v);
    if (!problem && v[0] != 2) problem = "GL_PACK_ALIGNMENT changed";
    if (!problem && !glIsEnabled(kFramebufferSrgb)) problem = "GL_FRAMEBUFFER_SRGB was switched off";
    glGetIntegerv(kReadBuffer, v);
    if (!problem && v[0] != GL_FRONT) problem = "the default framebuffer's read buffer changed";
    glGetIntegerv(kPackBufferBinding, v);
    if (!problem && GLuint(v[0]) != g_gl.sentinel_buffer) problem = "the pixel-pack buffer binding changed";
    glGetIntegerv(kReadFramebufferBinding, v);
    if (!problem && v[0] != 0) problem = "the read framebuffer binding changed";
    glGetIntegerv(kDrawFramebufferBinding, v);
    if (!problem && v[0] != 0) problem = "the draw framebuffer binding changed";
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glPixelStorei(GL_PACK_ROW_LENGTH, 0);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glDisable(kFramebufferSrgb);
    glReadBuffer(GL_BACK);
    if (g_gl.bind_buffer) g_gl.bind_buffer(kPackBuffer, 0);
    return problem;
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
    if (g_gl.dc) return gl_clear_rect(color, l, t, r, b);
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

void draw_frame(Gfx* gfx, uint32_t frame, int noise) {
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

    // Noise: many small rectangles of random colour, different every frame (below the barcode).
    uint32_t rng = frame * 2654435761u + 977u;
    for (int i = 0; i < noise; ++i) {
        rng = rng * 1664525u + 1013904223u;
        const float color[4] = {float((rng >> 8) & 255) / 255.0f, float((rng >> 16) & 255) / 255.0f, float((rng >> 24) & 255) / 255.0f, 1.0f};
        rng = rng * 1664525u + 1013904223u;
        const LONG rx = LONG((rng >> 4) % uint32_t(w - 24)), ry = bar_top + LONG((rng >> 14) % uint32_t(h - bar_top - 24));
        const LONG side2 = 4 + LONG((rng >> 28));
        clear_rect(gfx, color, rx, ry, rx + side2, ry + side2);
    }

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
        if (a == "--width" || a == "--height" || a == "--fps-cap" || a == "--freeze-ms" || a == "--seconds" || a == "--late-load-ms" || a == "--noise") {
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
            else if (a == "--noise") o->noise = n;
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
        } else if (a == "--fullscreen") {
            o->fullscreen = true;
        } else if (a == "--offscreen") {
            o->offscreen = true;
        } else if (a == "--gl") {
            o->gl = true;
        } else if (a == "--wgl") {
            o->wgl_swap = true;
        } else if (a == "--core") {
            o->core = true;
        } else if (a == "--state-check") {
            o->state_check = true;
        } else if (a == "--msaa") {
            const wchar_t* v = next("--msaa");
            if (!v) return false;
            o->msaa = _wtoi(v);
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
    const int pos = opt.offscreen ? -20000 : 100;
    HWND hwnd = CreateWindowExW(opt.offscreen ? WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW : 0, wc.lpszClassName, opt.title.c_str(),
                                WS_OVERLAPPEDWINDOW | (opt.offscreen ? 0 : WS_VISIBLE), pos, pos, rect.right - rect.left, rect.bottom - rect.top,
                                nullptr, nullptr, wc.hInstance, nullptr);
    if (hwnd && opt.offscreen) ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    if (!hwnd) {
        std::fprintf(stderr, "rec_testapp: cannot create the window (%lu)\n", GetLastError());
        return 1;
    }
    if (opt.late_load_ms) Sleep(DWORD(opt.late_load_ms));
    Gfx gfx;
    if (opt.gl) {
        if (!gl_context_for(hwnd, opt)) {
            std::fprintf(stderr, "rec_testapp: cannot create the OpenGL context\n");
            return 1;
        }
        if (opt.fullscreen) {  // borderless, over the whole monitor (not exclusive: OpenGL has no such thing)
            MONITORINFO mi{sizeof(mi)};
            GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi);
            SetWindowLongPtrW(hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
            SetWindowPos(hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top,
                         SWP_FRAMECHANGED | SWP_SHOWWINDOW);
            RECT client;
            GetClientRect(hwnd, &client);
            g_gl.width = UINT(client.right);
            g_gl.height = UINT(client.bottom);
        }
        gfx.width = g_gl.width;
        gfx.height = g_gl.height;
        GLint samples = 0;
        glGetIntegerv(kSamples, &samples);
        std::printf("rec_testapp pid %lu: OpenGL %s %ux%u, %s, pattern %s%s%s%s%s%s samples %d\n", GetCurrentProcessId(), reinterpret_cast<const char*>(glGetString(GL_VERSION)),
                    g_gl.width, g_gl.height, opt.wgl_swap ? "wglSwapBuffers" : "SwapBuffers", opt.pattern.c_str(), opt.vsync ? ", vsync" : "",
                    opt.fps_cap ? ", capped" : "", opt.core ? ", core" : "", opt.state_check ? ", state-check" : "", opt.fullscreen ? ", borderless" : "", samples);
    } else {
        if (!init_gfx(hwnd, opt.width, opt.height, &gfx)) {
            std::fprintf(stderr, "rec_testapp: cannot create the D3D11 device and swap chain\n");
            return 1;
        }
        if (opt.fullscreen && FAILED(gfx.swapchain->SetFullscreenState(TRUE, nullptr)))
            std::fprintf(stderr, "rec_testapp: could not switch to fullscreen\n");
        std::printf("rec_testapp pid %lu: D3D11 %dx%d, pattern %s%s%s%s%s\n", GetCurrentProcessId(), opt.width, opt.height,
                    opt.pattern.c_str(), opt.vsync ? ", vsync" : "", opt.fps_cap ? ", capped" : "",
                    opt.present1 ? ", Present1" : "", opt.fullscreen ? ", fullscreen" : "");
    }
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
            if (opt.gl) {
                g_gl.width = g_new_width;
                g_gl.height = g_new_height;
                gfx.width = g_new_width;
                gfx.height = g_new_height;
            } else if (!resize_gfx(&gfx, g_new_width, g_new_height)) {
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

        if (!opt.gl) gfx.context->OMSetRenderTargets(1, gfx.rtv.GetAddressOf(), nullptr);
        draw_frame(&gfx, frame, opt.noise);

        const UINT sync = opt.vsync ? 1 : 0;
        if (opt.gl) {
            glDisable(GL_SCISSOR_TEST);
            if (g_gl.state_check) gl_set_sentinel();
            if (g_gl.wgl_swap) wglSwapBuffers(g_gl.dc);
            else SwapBuffers(g_gl.dc);
            if (g_gl.state_check) {
                if (const char* problem = gl_check_sentinel()) {
                    std::printf("STATE CHANGED at frame %u: %s\n", frame, problem);
                    std::fflush(stdout);
                    return 3;
                }
            }
        } else if (opt.present1) {
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
    if (opt.fullscreen && !opt.gl) gfx.swapchain->SetFullscreenState(FALSE, nullptr);  // give the display mode back
    std::printf("rec_testapp: exit after %u frames\n", frame);
    if (timer) CloseHandle(timer);
    return 0;
}
