# rec — recorder M4 report (OpenGL backend, host BGRA → NV12)

**Date:** 2026-10-02 · **Plan:** `Recorder_Implementation_Planv2.md` §16 M4, §5.2, §8.2 · **Decisions:** D-089 – D-093

OpenGL games can be recorded now: the hook captures from `wglSwapBuffers` / `SwapBuffers`, the frames travel to
the host as BGRA, and the host converts them to NV12 for the encoder; everything after that (timeline, DUPs,
lock mode, AVI, `rec verify`, `rec convert`) is the M3 code unchanged. **Tested on the OpenGL mode of the
synthetic test app and on one real OpenGL game, Geometry Dash (64-bit).** The overhead comparison against FRAPS
was left out at the user's request.

## Acceptance

| Check (§16 M4) | Result | Status |
|---|---|---|
| OpenGL backend (PBO path) captures, state untouched | `rec_testapp --gl`: 15 s at 60 fps vsync, locked: 897–912 frames, `rec verify --testapp`: 0 missing, 0 repeated, 0 out of order, 0 unreadable; the colour bars come back within 1 level of BT.601 (so the row flip, the letterbox and the BGRA order are right); the game's GL state is exactly as it was after every capture (`--state-check`, ~3000 swaps over the runs) | **met** on the test app |
| Host BGRA → I420/NV12 conversion | AVX2 and scalar give identical bytes; 0.5 ms for 1280x720 in a benchmark, 1.1–1.9 ms median inside a recording on this machine (target 1.5 ms, "not measured" in the plan) | **met** (borderline in situ) |
| Minecraft recording works | Minecraft Bedrock is Direct3D 11 and was recorded in M3. An OpenGL Minecraft (Java) is installed on the dev laptop but has not been touched. **Geometry Dash** (64-bit, OpenGL) was recorded by the user instead: 65.3 s, 3918 frames, `rec verify` PASS, nothing dropped by the host, picture upright with the right colours (checked on three decoded frames) | **met** on a real OpenGL game (Java Minecraft still untried) |
| Overhead A/B against FRAPS | Skipped at the user's request | **not done** |

Tests: `rec_tests` 40/40 in release and debug (new: BGRA→NV12 equality, accuracy, clamping and speed; a
recording session fed BGRA frames through the real ring); `m4_opengl.ps1` ~75 checks pass; `m1_attach_detach`
(30 cycles, Direct3D) and the non-graphics-process path of `m1_robustness` still pass after the install logic
changed. Release, debug and x86 builds: 0 warnings (`rec_hook32.dll` still never run). `m2_capture.ps1` was not
run (it presses F9 and switches the display to fullscreen while the user's game is open); the Direct3D 11
capture was re-checked with a 10 s test-app recording after the refactor instead.

## What was built

- **Shared capture core** (`capture_core.cpp`): ring, pacer, copy worker and failure handling, moved out of the
  Direct3D 11 file so that both backends use the same code (D-089).
- **OpenGL backend** (`capture_gl.cpp`, `gl_hook.cpp`, `gl_api.h`): blit into an own FBO at the output size,
  PBO ring with fences, state saved and restored, no link to opengl32 (D-090, D-091).
- **Hook install** (`hook_main.cpp`): per API group, polled; the first API to present is the backend.
- **Host**: `Layout::Bgra` slots (`protocol.h`), `bgra_to_nv12` (`convert.cpp`, D-092), `convert_ms` in the
  telemetry and the summary, the backend named in the summary.
- **Test app**: `--gl`, `--wgl`, `--core`, `--msaa`, `--state-check`, `--offscreen` (D-093).

## Measurements (i5-7200U laptop, `rec_testapp --gl`, 1366x745 vsync → 1280x720 @ 60, lossless)

The user's Minecraft was running in the background during all of these, so the pacing numbers are noisier
than the Direct3D ones in M3 (D3D11 on the same machine at the same time: hook p50 0.18 ms, pacing p99 3.4 ms).

| | OpenGL (6 runs) |
|---|---|
| hook time per captured frame, p50 / p99 | 0.19–0.31 ms / 0.9–2.5 ms |
| first captured frame (makes the GL objects) | 13–22 ms (D3D11: 3–5 ms) |
| host conversion BGRA → NV12, p50 / p99 | 1.1–1.9 ms / 2.8–4.4 ms |
| encode average | 2.9–4.7 ms |
| compression (test app content) | 9.2:1 (11.5:1 with 4x MSAA) |
| pacing error, p50 / p99 | 0.2–1.4 ms / 4.4–8.3 ms (D3D11: 0.2 / 3.4) |
| frames lost | 0; DUPs only where the game itself fell behind |

## Geometry Dash (the user's recording, 2026-10-02, 1296x729 window -> 1280x720 @ 60, lossless, locked)

65.3 s, 3918 frames, 1170 MB (4.3:1), `rec verify` PASS. Hook 0.31 ms median, 0.86 ms p99, 1.13 ms max, first frame
4.4 ms (the 13-22 ms below is the test app's driver path, not a general figure); host conversion 1.33 ms median, 3.5 ms
p99; encode 4.5 ms average, 31.8 ms worst; disk 400 MB/s, no slow writes; ring and packet queue never full.
66 ticks (1.7%) were filled with DUPs and 2 late frames were dropped (W2301): the game averaged 59.0 fps with a
1% low of 46 fps, so it missed ticks on its own; the pacing error p99 of 15.6 ms (W1301) is the same effect, a game
frame arriving a whole tick late. The same pattern as in the Minecraft Bedrock recording (59.65 fps against a 60 grid).

## Found while testing

- **The first OpenGL capture was right the first time** (orientation, colours, state), judged by checks written
  before the capture existed (the barcode, the colour bars, the state sentinel).
- **Launch status text** said the hook installs "when the game loads Direct3D 11" while it was waiting for a GL
  game too: now "Direct3D 11 or OpenGL".
- **Test thresholds**: the multisampled run once had 6 filled DUPs (1.2%) because the test app itself ran at
  59.2 fps with the background game using the same GPU; the script now judges DUPs against the game's own
  frame rate.

## Known limits

- **First-frame hitch**: the first captured frame costs 13–22 ms (driver work for the renderbuffer and six
  pixel-pack buffers): one dropped game frame, about, at the start of a recording.
- **Pacing p99** is 4–8 ms for OpenGL against 3 ms for Direct3D 11 here: the swap call's timing is more jittery;
  the video is not affected (ticks are assigned correctly, the pacing error is reported, W1301 warns).
- **GL contexts**: below GL 3.0 without ARB_framebuffer_object, E1206 and no capture. A game that swaps with
  a context other than the one it renders in, or recreates its context, loses the frames in flight and the
  objects are made again (I1207); the old ones leak until the process ends. Conditional rendering, a
  single-buffered pixel format and stereo are not handled.
- **One real OpenGL game tried** (Geometry Dash). Java Minecraft through LWJGL is untried; other games may use
  contexts or pixel formats the test app does not.
- **`rec convert` is slow**: 248 s for the 65 s recording on this laptop (decoding and x264 compete for the same cores).
- `--format rgb` (lossless RGB, codec GBR mode) is still not implemented.
- Direct3D 11 and OpenGL only, 64-bit only, as before.

## Next

M5: rate controller (controlled DUP dropping when the CPU is overloaded, near-lossless when the disk is), `rec
bench-disk`, the fault-injection flags. Before that, an OpenGL game recording by the user (Minecraft Java) would
close M4.
