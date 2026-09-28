# Braps

A lightweight screen/gameplay recorder built to avoid the encoding-lag
problem that OBS and Xbox Game Bar hit on low-end/dual-core laptops (see
[Braps.pdf](Braps.pdf) for the original design discussion this project
came from).

Two capture paths are available:

- **DXGI Desktop Duplication** (with a GDI BitBlt fallback) — works with
  any application, no setup beyond running the app. This is the default.
- **Direct3D Present hook** (`--inject <pid>`) — a Fraps-style DLL injected
  directly into a running game's process, hooking `IDXGISwapChain::Present`
  to grab frames straight from the game's own D3D10/D3D11 pipeline before
  Windows composites anything else. Much lower capture overhead under
  load, but only works for non-anti-cheat-protected D3D10/D3D11 titles.

## Architecture

- **Capture (default path)**: DXGI Desktop Duplication (`src/CaptureDXGI.cpp`),
  with a GDI BitBlt fallback (`src/CaptureGDI.cpp`) for hardware/drivers
  that don't support DXGI 1.2.
- **Capture (hook path)**: `hook/src/D3D11Hook.cpp` patches
  `IDXGISwapChain::Present`'s vtable slot inside the target process
  (detecting at runtime whether the swapchain is backed by D3D10 or D3D11
  and reading back accordingly), pushing frames to `Braps.exe` over a
  named shared-memory ring buffer (`hook/include/SharedFrameChannel.h`).
  `hook/src/Injector.cpp` performs the `CreateRemoteThread` + `LoadLibraryW`
  injection; `hook/src/CaptureHook.cpp` is the `ICapture` implementation
  that consumes from the shared channel on the `Braps.exe` side.
- **Ring buffer**: lock-free SPSC queue (`include/RingBuffer.h`) decouples
  capture from disk I/O so a slow write never stalls the capture thread.
- **Disk writer**: background thread drains the ring buffer to BMP frame
  files while recording (chosen over headerless raw so FFmpeg's concat
  demuxer can probe each frame's format).
- **Encoder**: on stop, spawns a bundled `ffmpeg.exe` to stitch the frame
  sequence into a variable-frame-rate MP4 (`src/Encoder.cpp`), using each
  frame's real capture timestamp so playback matches the actual recording
  pacing rather than an averaged flat frame rate.
- **Hotkeys**: native `RegisterHotKey` (`src/Hotkeys.cpp`) — F9 toggles
  recording, F10 takes a screenshot.
- **Overlay**: an always-on-top HUD (`src/Overlay.cpp`) showing live FPS
  and recording state, excluded from any screen capture via
  `SetWindowDisplayAffinity(WDA_EXCLUDEFROMCAPTURE)` — visible to you,
  never baked into the recording.

## Requirements

- Visual Studio 2022+ (Desktop development with C++ workload) with the
  CMake tools component, or any MSVC toolchain with CMake 3.20+
- Windows 10/11 SDK
- A bundled `ffmpeg.exe`

## Setup

### 1. Vendor FFmpeg

Already vendored at `third_party/ffmpeg/bin/ffmpeg.exe` (extracted from
`ffmpeg-2026-09-02-git-9fc8c785e2-full_build.7z`). To update it, extract a
new build to that same path, or point CMake elsewhere by editing
`FFMPEG_BIN` in `CMakeLists.txt`.

### 2. Build the main (64-bit) app

```bash
cmake -B build -S .
cmake --build build --config Release
```

This produces `build/Release/Braps.exe`, `BrapsHook.dll` (the 64-bit
Present hook), and copies `ffmpeg.exe` alongside automatically.

### 3. (Optional) Build 32-bit hook support

`CreateRemoteThread` + `LoadLibraryW` injection requires the injecting
process to match the target's pointer size — a 64-bit `Braps.exe` cannot
inject into a 32-bit game directly. To support 32-bit games, build a
second, separate tree configured for Win32; this produces only the two
small helper binaries the main app needs (no separate `Braps32.exe`):

```bash
cmake -B build32 -S . -A Win32
cmake --build build32 --config Release
```

Then copy the two artifacts it produces next to the main build:

```bash
cp build32/Release/BrapsHook32.dll build/Release/
cp build32/Release/BrapsInjector32.exe build/Release/
```

`Braps.exe` detects the target's bitness automatically at inject time and
picks `BrapsHook.dll` or `BrapsHook32.dll` accordingly, delegating the
actual injection to `BrapsInjector32.exe` (a same-bitness helper) when the
target is 32-bit. If these two files aren't present next to `Braps.exe`,
`--inject` against a 32-bit target fails with a clear error rather than
silently doing the wrong thing.

## Usage

**Desktop/window capture (default, works with anything):**

```bash
build/Release/Braps.exe
```

**Direct game capture via the Present hook (D3D10/D3D11 games only):**

```bash
build/Release/Braps.exe --list-processes   # find the target's PID
build/Release/Braps.exe --inject <PID>
```

**Other flags:**
- `--fps <n>` — target capture rate (default 30)

## Controls

- `F9` — start/stop recording (encodes to `C:\SavedRecordings` on stop)
- `F10` — take a screenshot (saved as raw BGRA to `C:\SavedRecordings`)
- `Ctrl+C` — exit

## Diagnostics

`tools/frame_timing_analyzer.html` — open directly in a browser (no
server needed), drop in a `frame_timing_*.csv` from `C:\SavedRecordings`
(written alongside every recording) to see per-frame interval/jitter
statistics and spot pacing problems.

## Known limitations

- Present hook covers D3D10 and D3D11 only — D3D9, D3D12, OpenGL, and
  Vulkan games aren't supported by `--inject` yet (each needs its own
  hook module targeting that API's present/swap call).
- D3D10 support is implemented but not yet verified against a real D3D10
  title (no D3D10-only test game was available when it was added).
- No anti-cheat bypass or evasion of any kind — anti-cheat-protected games
  will refuse the injection outright (this is intentional; use the
  DXGI/GDI path for those).
- Screenshots are dumped as raw `.raw` BGRA, not `.png`.
- No GUI; console-only.
