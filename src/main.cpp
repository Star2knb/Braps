#include "Recorder.h"
#include "Hotkeys.h"
#include "Overlay.h"
#include "Injector.h"
#include "CaptureHook.h"

#include <iostream>
#include <chrono>
#include <thread>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <windows.h>
#include <timeapi.h>

namespace {

int ParseFpsArg(int argc, char** argv) {
    constexpr int kDefaultFps = 30;
    constexpr int kMinFps = 1;
    constexpr int kMaxFps = 240;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--fps") == 0 && i + 1 < argc) {
            int value = std::atoi(argv[i + 1]);
            if (value <= 0) {
                std::cerr << "[Braps] Invalid --fps value, using default of " << kDefaultFps << ".\n";
                return kDefaultFps;
            }
            return std::clamp(value, kMinFps, kMaxFps);
        }
    }
    return kDefaultFps;
}

// Finds --inject <pid>. Returns 0 if not present.
DWORD ParseInjectArg(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--inject") == 0 && i + 1 < argc) {
            return static_cast<DWORD>(std::atoi(argv[i + 1]));
        }
    }
    return 0;
}

bool HasFlag(int argc, char** argv, const char* flag) {
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], flag) == 0) return true;
    }
    return false;
}

std::wstring GetHookDllPath(DWORD targetPid) {
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::wstring path(exePath);
    size_t lastSlash = path.find_last_of(L"\\/");
    std::wstring dir = path.substr(0, lastSlash + 1);

    // Must match the target's bitness: BrapsHook.dll is 64-bit (built
    // alongside this exe), BrapsHook32.dll is the separate 32-bit build
    // (see README.md / CMakeLists.txt's BRAPS_BUILDING_32BIT_HELPERS path).
    // InjectDll() itself transparently delegates to BrapsInjector32.exe
    // for the actual cross-bitness injection call, but it still needs to
    // be handed a DLL of the right bitness to load into the target.
    return dir + (braps::Is32BitProcess(targetPid) ? L"BrapsHook32.dll" : L"BrapsHook.dll");
}

void PrintCandidateProcesses() {
    auto processes = braps::ListCandidateProcesses();
    std::wcout << L"PID\tProcess Name\n";
    for (const auto& p : processes) {
        std::wcout << p.pid << L"\t" << p.name << L"\n";
    }
}

}

int main(int argc, char** argv) {
    // Windows' default timer resolution (~15.6ms) makes sleep_for() round
    // up to that granularity regardless of the duration requested — at
    // 60fps our per-frame budget is only 16.7ms, so a sleep for even a few
    // remaining ms was costing a full extra tick and capping capture around
    // 30fps no matter how fast the capture work itself ran. Raising the
    // process's timer resolution to 1ms fixes that; Windows reverts it
    // automatically when the process exits, even via Ctrl+C.
    timeBeginPeriod(1);

    if (HasFlag(argc, argv, "--list-processes")) {
        PrintCandidateProcesses();
        return 0;
    }

    int fpsTarget = ParseFpsArg(argc, argv);
    DWORD injectPid = ParseInjectArg(argc, argv);
    bool micEnabled = HasFlag(argc, argv, "--mic");

    Recorder recorder(fpsTarget, micEnabled);
    bool initOk;

    if (injectPid != 0) {
        std::wstring dllPath = GetHookDllPath(injectPid);
        std::wstring error;
        std::wcout << L"[Braps] Injecting " << dllPath << L" into PID " << injectPid << L"...\n";
        if (!braps::InjectDll(injectPid, dllPath, error)) {
            std::wcerr << L"[Braps] Injection failed: " << error << L"\n";
            return 1;
        }
        std::cout << "[Braps] Injection succeeded. Waiting for hook to attach...\n";
        // Give the injected DllMain's install thread (see dllmain.cpp) a
        // moment to create the D3D11 device and the shared memory section
        // before we try to open it as a consumer.
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
        initOk = recorder.InitializeWithCapture(std::make_unique<CaptureHook>(fpsTarget));
        if (!initOk) {
            std::cerr << "[Braps] Failed to attach to hook's shared memory channel. "
                         "Is the target process still running?\n";
            return 1;
        }
        std::cout << "[Braps] Using hooked D3D11 Present capture (direct from game process).\n";
    } else {
        initOk = recorder.Initialize();
        if (!initOk) {
            std::cerr << "[Braps] Failed to initialize any capture backend.\n";
            return 1;
        }
    }

    recorder.Run();

    Hotkeys hotkeys(
        [&recorder]() { recorder.ToggleRecording(); },
        [&recorder]() { recorder.TakeScreenshot(); });
    hotkeys.Start();

    Overlay overlay(recorder);
    overlay.Start();

    std::cout << "================================\n";
    std::cout << " BRAPS RECORDER RUNNING (target " << fpsTarget << " fps, mic: "
               << (micEnabled ? "on" : "off") << ")\n";
    std::cout << " Press [F9]  to Start/Stop Recording\n";
    std::cout << " Press [F10] to Take a Screenshot\n";
    std::cout << " Press [Ctrl+C] to Exit\n";
    std::cout << "================================\n";

    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}
