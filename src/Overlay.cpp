#include "Overlay.h"
#include "Recorder.h"

#include <string>

namespace {
constexpr wchar_t kWndClassName[] = L"BrapsOverlayWindow";
constexpr int kWindowWidth = 160;
constexpr int kWindowHeight = 36;
constexpr int kScreenMargin = 12;
constexpr UINT_PTR kRedrawTimerId = 1;
constexpr UINT kRedrawIntervalMs = 250;
}

Overlay::Overlay(Recorder& recorder) : recorder_(recorder) {}

Overlay::~Overlay() {
    Stop();
}

void Overlay::Start() {
    running_ = true;
    thread_ = std::thread(&Overlay::ThreadMain, this);
}

void Overlay::Stop() {
    if (running_.exchange(false) && thread_.joinable()) {
        if (hwnd_) {
            PostMessageW(hwnd_, WM_CLOSE, 0, 0);
        }
        thread_.join();
    }
}

LRESULT CALLBACK Overlay::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    Overlay* self = reinterpret_cast<Overlay*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (msg) {
        case WM_CREATE: {
            auto* createStruct = reinterpret_cast<CREATESTRUCTW*>(lParam);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(createStruct->lpCreateParams));
            SetTimer(hwnd, kRedrawTimerId, kRedrawIntervalMs, nullptr);
            return 0;
        }
        case WM_TIMER:
            if (self) InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_PAINT:
            if (self) self->Paint(hwnd);
            return 0;
        case WM_DESTROY:
            KillTimer(hwnd, kRedrawTimerId);
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

void Overlay::ThreadMain() {
    HINSTANCE instance = GetModuleHandleW(nullptr);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &Overlay::WndProc;
    wc.hInstance = instance;
    wc.lpszClassName = kWndClassName;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr; // painted manually every frame
    RegisterClassExW(&wc);

    int screenWidth = GetSystemMetrics(SM_CXSCREEN);
    int x = screenWidth - kWindowWidth - kScreenMargin;
    int y = kScreenMargin;

    hwnd_ = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        kWndClassName, L"Braps Overlay", WS_POPUP,
        x, y, kWindowWidth, kWindowHeight,
        nullptr, nullptr, instance, this);

    if (!hwnd_) {
        running_ = false;
        return;
    }

    // Excludes this window from any screen/desktop capture (DXGI Desktop
    // Duplication, GDI BitBlt, and most third-party recorders) while still
    // rendering it normally on the physical display — this is what keeps
    // the HUD visible to the user only, not baked into recordings. Needs
    // Windows 10 2004+; if unavailable, leave the window as a normal
    // capturable window rather than forcing WDA_MONITOR, which would
    // paint a black rectangle into every recording instead of just
    // showing the overlay — the pre-existing (visible-in-recording)
    // behavior is the better fallback of the two.
    SetWindowDisplayAffinity(hwnd_, WDA_EXCLUDEFROMCAPTURE);

    SetLayeredWindowAttributes(hwnd_, 0, 235, LWA_ALPHA);
    ShowWindow(hwnd_, SW_SHOWNOACTIVATE);

    MSG msg;
    while (running_ && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    hwnd_ = nullptr;
}

void Overlay::Paint(HWND hwnd) {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);

    RECT rect;
    GetClientRect(hwnd, &rect);

    bool isRecording = recorder_.IsRecording();
    int fps = recorder_.CurrentFps();

    // Solid, slightly rounded dark background so the text stays readable
    // over any content behind it, regardless of desktop colors.
    HBRUSH bgBrush = CreateSolidBrush(RGB(20, 20, 20));
    FillRect(hdc, &rect, bgBrush);
    DeleteObject(bgBrush);

    SetBkMode(hdc, TRANSPARENT);
    HFONT font = CreateFontW(16, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                              DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                              CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    HFONT oldFont = static_cast<HFONT>(SelectObject(hdc, font));

    // Recording state as both color and a filled/hollow dot glyph, so
    // it's distinguishable at a glance without reading the text.
    COLORREF stateColor = isRecording ? RGB(255, 60, 60) : RGB(140, 140, 140);
    SetTextColor(hdc, stateColor);
    std::wstring dotAndLabel = isRecording ? L"● REC" : L"○ IDLE";
    TextOutW(hdc, 8, 4, dotAndLabel.c_str(), static_cast<int>(dotAndLabel.size()));

    SetTextColor(hdc, RGB(230, 230, 230));
    std::wstring fpsText = std::to_wstring(fps) + L" fps";
    TextOutW(hdc, 8, 19, fpsText.c_str(), static_cast<int>(fpsText.size()));

    SelectObject(hdc, oldFont);
    DeleteObject(font);

    EndPaint(hwnd, &ps);
}
