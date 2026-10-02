# rec — recorder M2 report (D3D11 capture, frame ring, F9)

**Date:** 2026-10-02 · **Plan:** `Recorder_Implementation_Planv2.md` §16 M2 · **Decisions:** D-065 – D-075

M2 captures frames from a hooked D3D11 game and delivers them to `rec.exe`, which receives and checks
them. **Nothing is encoded or written to disk yet:** "recording" in M2 is a capture test whose summary
is printed and logged. Encoding, the timeline, pacing and the AVI writer are M3.

## Acceptance

| Check (§16 M2) | Result | Status |
|---|---|---|
| P1 measured: hook cost when idle < 5 µs | 1.3 – 2.0 µs per Present on a vsync-locked 60 fps game (the per-second average, median of 7 seconds); 0.2 – 0.4 µs on an uncapped one (cache-warm) | **met** |
| P2 measured: median ≤ 0.5 ms, p99 ≤ 1.0 ms (1366×745 → 1280×720) | median **0.30 – 0.40 ms** over 10 runs: **met**. p99 **0.85 – 2.0 ms**, median of runs ~1.3 ms: **not reliably met** (see below) | median met, p99 not |
| F9 start/stop works in fullscreen | real F9 key presses (`keybd_event`) start and stop a capture in a window and in **exclusive fullscreen**; every check of the capture passes in both | **met** |
| No GPU stalls (`W1201` rare) | 0 in most runs, ≤ 5 in any; 111 skips in 8 s with the plan's 3 staging slots, which is why the default is now 6 (D-068) | **met** (with 6 slots) |
| Host receives frames and checksums them | every frame is hashed, timed and checked; nothing lost between hook and host in any run | **met** |

Other plan items checked in M2: stop (F9) → summary in **77 – 336 ms** (P6: ≤ 2 s; that is the capture
only, there is no file to close yet); **E7003** keyboard-hook fallback when F9 is taken (two `rec`
instances); the audible cue plays (`PlaySound` returns success; I can't listen from here, so please tell
me if you don't hear a short rising tone on start and a falling one on stop).

Tests: `rec_tests` 25/25 in release and debug (new: output planning, frame tools, a **simulated hook**
filling the real shared-memory ring through a 20-frame recording with a missing tick, stop, a second
recording, a reported capture failure); `tests\m2_capture.ps1` 34 checks; the M1 scripts still pass on
protocol v2 (`m1_attach_detach.ps1` 100/100, `m1_robustness.ps1` 21/21). Release, debug and x86 builds:
0 warnings (`rec_hook32.dll` compiles with the capture code and the protocol layout checks hold on x86;
the 32-bit hook has still not been run).

## What was built

- **Hook (`hook/capture_d3d11.cpp`, `shaders/capture.hlsl`):** per captured frame, copy the back buffer,
  one draw that scales, letterboxes and converts to NV12 (BT.601 full range, chroma = 2×2 average),
  copy into a ring of staging textures, read the oldest finished one back without waiting. Runs inside
  our own device-context state and under the device lock; failures disable capture, not the game.
  A worker thread does the memcpy out of mapped memory (D-069).
- **Protocol v2:** recording configuration and capture counters in the control block, a frame ring and
  semaphore per recording (D-065, D-066).
- **Host:** `RecordingSession` (state machine, ring, receiver thread, summary), hotkey thread with
  fallback and debounce, generated cue sounds, frame tools (hash, test-pattern decoder, NV12→RGB, PNG
  writer), the watch loop's recording status line.
- **Test app:** `rec_testapp --fullscreen`; test aids `--record-for`, `--save-frame`.

## Measurements (i5-7200U laptop, AC power, `rec_testapp` 1366×745 vsync 60 → 1280×720 @ 60)

A typical 10-second capture:

```
Capture finished: 568 frames over 10.2 s (55.5 fps), 1280x720 from a 1366x745 game
  hook time per captured frame: p50 0.295 ms, p99 0.852 ms, max 1.639 ms (first frame, which creates the GPU objects: 41.98 ms)
  Present to frame in the host: p50 83.8 ms, p99 86.1 ms; GPU read-back lag p50 5 Presents; game frame p50 16.67 ms, p99 17.82 ms
  ticks without a frame: 46, ticks with two frames: 0, frames lost between hook and host: 0
  GPU backlog skips (W1201): 0, ring-full drops (W1202): 0, capture errors: 0
  test pattern: 568 frames read, counters 151..764, 0 out of order, 46 game frames not captured, 0 unreadable
  colour check against BT.601 on 492 frames: largest difference Y 1, Cb 1, Cr 1 (of 255)   [a later run]
```

Where the render thread's time goes per captured frame (µs, from the DEBUG log with `-v`):
setup (device and back-buffer lookups) ~80, issuing the GPU work (copy, state swap, one draw, copy to
staging, query) ~280, map ~25, unmap and publish ~18. The state swap itself is ~4 µs.
How the number got there: two draws and an inline `memcpy` of the mapped staging texture first gave
p50 1.06 ms (the memcpy alone was ~600 µs on this Intel GPU, streaming loads did not help); one draw
and the copy worker brought it to 0.3 ms.

- **Image correctness:** a captured frame was inspected (colour bars, barcode bits, the moving square,
  the thin letterbox bars of 1366×745 → 1280×720), and on every captured frame of the test the middle of
  all 8 colour bars matches BT.601 full-range YUV to within 1/255.
- **Leaks:** over 20 start/stop cycles in one attach the game's handle count (197 → 197) and private
  memory (24 → 24 MB) and `rec.exe`'s (290 → 292 handles, 18 → 18 MB) are flat. One earlier run of the
  same script saw the game's handles go 197 → 232; I could not reproduce it in three later runs
  (including a hand-run of ten cycles that was exactly 197/205 at every step) and have no explanation, so
  it stays on the watch list.
- **First frame:** creating the GPU objects takes 5 – 10 ms when warm and 40 – 60 ms the first time in a
  process (shader creation in the driver): a hitch of up to ~3 frames when F9 is pressed.
- **Latency:** the frame is in the host 67 – 84 ms after the game's Present (4 – 5 Presents: the GPU
  finishes the copy that late behind the flip-model frame queue, plus one for the worker).
- **Copy worker:** ~0.6 ms of memcpy per frame on another thread in the game's process (≈ 4% of a core
  at 60 fps; estimated from the render-thread measurement, not measured separately).

## On a real game: Minecraft Bedrock

Run by the user on Minecraft Bedrock (local world, windowed 840×600, 60 Hz, FRAPS not loaded): `rec attach
--name Minecraft.Windows.exe --duration 60 --save-frame mc.png`, F9 once. The user reported no stutter.

```
Capture finished: 3066 frames over 51.3 s (59.7 fps), 840x472 from a 840x600 game
  hook time per captured frame: p50 0.185 ms, p99 0.299 ms, max 0.725 ms (first frame, which creates the GPU objects: 35.47 ms)
  Present to frame in the host: p50 33.4 ms, p99 38.2 ms; GPU read-back lag p50 2 Presents; game frame p50 16.69 ms, p99 20.21 ms
  ticks without a frame: 14, ticks with two frames: 0, frames lost between hook and host: 0
  GPU backlog skips (W1201): 0, ring-full drops (W1202): 0, capture errors: 0
```

Against the plan's targets this is P2 met on both median and p99 (0.185 / 0.299 ms), on one real game at
a small window size. The saved frame is a correct picture (checked by eye: colours, crosshair, hotbar).
The 840×472 output is the default `record.size` 1280x720 shrunk to fit inside the 840×600 back buffer
with the 16:9 aspect ratio, so the 4:3 picture is pillarboxed; `rec config set record.size native`
captures the window as it is. The test app's p99 tail (below) did not appear on the real game.

## What does not meet the plan, and known limits

- **P2 p99 (≤ 1.0 ms) is not reliably met.** Median is fine; the tail is 1 – 2 ms in most runs, coming from
  the driver: either issuing the copy/draw or `Map` occasionally takes 1 – 2 ms (the slow-frame log shows
  one phase or the other, clustered in time, so it depends on what else the laptop is doing). Ideas if
  it matters: record the GPU work on a deferred context on the worker thread, or avoid the back-buffer
  copy; both are bigger changes. The acceptance script guards against regressions (p99 ≤ 2.5 ms) and
  prints a note above 1.0.
- **Free mode drops ticks.** With the game running at the recording rate and no pacing, 0 – 8% of ticks
  get no frame depending on the phase between the game and the tick grid (jitter at the rounding
  boundary). Lock mode in M3 aligns the game to the grid; M3 fills the rest with DUPs.
- **Direct3D 11 only.** An OpenGL / D3D12 / Vulkan game is refused with a clear message (M4, M10).
- **Resizes** rescale into the fixed output but the test-pattern checks use the size at the start;
  alt-tab, minimise and device loss are M7. A device removed under a recording logs E1209 and disables
  capture (the path is written, not exercised).
- **Real game:** capture has not been run on Minecraft Bedrock yet; that is the next thing to try
  (instructions in the hand-over message). The 3-second anti-cheat rescan and the SEH guard around
  capture have no fault-injection test until M5.
- The copy worker and the state swap depend on D3D11.1 (Windows 8+); a device without it disables capture
  with E1211.

## Next: M3

Integrate RCV1 (encoder thread pool, the BGRA path is not needed for D3D11), the timeline with DUP
filling and the stall rule, lock/free pacing, the AVI OpenDML writer with unbuffered overlapped writes,
`rec verify`, `rec convert` (video only), telemetry CSV and summary JSON. Acceptance: the test app passes
P9 (no unexplained frame loss), the freeze test passes, files convert with FFmpeg.
