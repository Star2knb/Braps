# Braps

A lightweight, universal screen recorder built to avoid the encoding-lag
problem that OBS and Xbox Game Bar hit on low-end/dual-core laptops (see
[Braps.pdf](Braps.pdf) for the full design discussion this project came from).

## Architecture

- **Capture**: DXGI Desktop Duplication (`src/CaptureDXGI.cpp`), with a GDI
  BitBlt fallback (`src/CaptureGDI.cpp`) for hardware/drivers that don't
  support DXGI 1.2.
- **Ring buffer**: lock-free SPSC queue (`include/RingBuffer.h`) decouples
  capture from disk I/O so a slow write never stalls the capture thread.
- **Disk writer**: background thread drains the ring buffer to raw `.raw`
  BGRA frame files while recording.
- **Encoder**: on stop, spawns a bundled `ffmpeg.exe` to stitch the raw
  frame sequence into a compressed MP4, then deletes the temp frames
  (`src/Encoder.cpp`). This heavy compression work never runs while you're
  actively recording.
- **Hotkeys**: native `RegisterHotKey` (`src/Hotkeys.cpp`) — F9 toggles
  recording, F10 takes a screenshot. No external hotkey library needed.

## Requirements

- Visual Studio 2022 (Desktop development with C++ workload) or any
  MSVC/Clang toolchain with CMake 3.20+
- Windows 10/11 SDK
- A bundled `ffmpeg.exe`

## Setup

1. FFmpeg is already vendored at `third_party/ffmpeg/bin/ffmpeg.exe`
   (extracted from `ffmpeg-2026-09-02-git-9fc8c785e2-full_build.7z`). If
   you ever need to update it, extract a new build so `ffmpeg.exe` ends up
   at that same path, or point CMake elsewhere by editing `FFMPEG_BIN` in
   `CMakeLists.txt`.

2. Configure and build:

   ```bash
   cmake -B build -S .
   cmake --build build --config Release
   ```

3. Run `build/Release/Braps.exe`. `ffmpeg.exe` is copied next to it
   automatically as a post-build step (if found at the path above).

## Controls

- `F9` — start/stop recording (encodes to `C:\SavedRecordings` on stop)
- `F10` — take a screenshot (saved as raw BGRA; see Known limitations)
- `Ctrl+C` — exit

## Known limitations / next steps

- Screenshots are currently dumped as raw `.raw` BGRA, not `.png` — wire up
  WIC or stb_image_write to encode PNG directly.
- No GUI yet; console-only per the original blueprint. `CustomTkinter`-style
  live preview was discussed as a future step but intentionally deferred.
- No overlay/FPS counter thread yet (mentioned in the original design doc as
  a "next steps to turn this into a full app" item).
- Raw frame dumps are `.raw`, not `.jpg` as in the very first Python
  prototype in the doc — this avoids JPEG encode overhead on the disk
  writer thread, matching the "defer all compression" philosophy from the
  final C++ design.
