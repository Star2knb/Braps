# rec — recorder M3 report (RCV1 in the pipeline, timeline, pacing, AVI, verify, convert)

**Date:** 2026-10-02 · **Plan:** `Recorder_Implementation_Planv2.md` §16 M3 · **Decisions:** D-076 – D-085

F9 now produces a lossless video file. Frames go from the hook through the shared ring to the RCV1
encoder, a timeline puts exactly one frame on every tick (repeating the last one where the game did not
present), and an AVI OpenDML writer saves them with unbuffered, overlapped writes. `rec verify` checks a
recording and `rec convert` turns it into H.264. **Tested on the synthetic test app only; Minecraft has
not been recorded yet.** No audio (M6).

## Acceptance

| Check (§16 M3) | Result | Status |
|---|---|---|
| Test-app end-to-end passes P9 (no unexplained frame loss) | 15 s at 60 fps vsync, locked: 898–914 frames, `rec verify --testapp`: 0 missing, 0 repeated, 0 out of order, 0 unreadable, DUPs never change the picture; 0–4 DUPs per 900 frames, only where the app's `Present` was a whole tick late | **met** |
| Freeze test passes | the app stops presenting for 3 s: 184 DUPs (180 expected) in one unbroken run, the video has one frame per tick over the whole 15 s, nothing missing or repeated; 5 frames in flight when the freeze began are lost to latency (D-078, below) | **met**, with that caveat |
| Files convert with FFmpeg | `rec convert` → H.264 MP4/MKV; `ffprobe` reads the same frame count as the AVI (615/615, 914/914, …), 60/1 fps, BT.601 limited range | **met** |

Other plan items checked: a game slower than the recording rate (45 fps into 60) gets 182 DUPs in 732
frames (24.9%, a quarter as expected); free mode (`--no-lock`) writes a valid constant-frame-rate file;
a 5-minute soak at 720p60 is exactly 18,000 frames, 2.5 GB, `verify --testapp` clean, game handles flat
(188 → 191), nothing dropped; 20 start/stop cycles and F9 in a window and in exclusive fullscreen (M2
checks) still pass with files being written.

Tests: `rec_tests` 34/34 in release and debug (new: disk writer with patches across buffers and
truncation, AVI writer/reader with one and several RIFF blocks and damaged files, the encode pipeline
with gaps, a late frame, a stall and a queue that wraps — every decoded frame compared with what was
sent — and a recording session that writes and re-reads a real file); codec `rcv_tests` 72/72;
`m3_record.ps1` ~50 checks; M1 scripts (`m1_attach_detach` 100/100, `m1_robustness` 21/21) and
`m2_capture.ps1` (34 checks) pass on the M3 build. Release, debug and x86 builds: 0 warnings
(`rec_hook32.dll` builds; still never run).

## What was built

- **Pipeline** (`pipeline.cpp`): timeline with DUP filling, RCV1 encoder fed straight from the ring slot,
  packet arena and queue, writer thread, telemetry CSV (D-076, D-077, D-082).
- **Disk writer** (`disk_file.cpp`): two 8 MB sector-aligned buffers, unbuffered and overlapped, exact
  per-write latency (a thread-pool callback timestamps each completion), patching of sectors already on
  disk, 1 GiB preallocation, truncation to the real length at close.
- **AVI** (`avi.cpp`): OpenDML writer and a reader that cross-checks every index (D-080).
- **Lock mode** in the hook: holds the game to the tick grid with a high-resolution timer (D-079).
- **Session**: file naming and reservation, JSON summary, richer status line; **`rec verify`** and
  **`rec convert`** (D-084).

## Measurements (i5-7200U laptop, AC power, `rec_testapp` 1366×745 vsync → 1280×720 @ 60, lossless)

A steady 15 s recording (typical):

```
Recording finished: 914 frames, 15.2 s (60 fps), 1280x720 from a 1366x745 game, locked
  video: I 8, P 906, DUP 0; ratio 9.20:1; encode avg 5.0 ms, p99 12.8 ms, max 15.9 ms (2 codec threads)
  disk: 119 MB/s average, 150 MB/s best; file 131 MB (8.6 MB/s of video)
  hook time per captured frame: p50 0.24–0.39 ms, p99 0.7–1.2 ms (as in M2)
  pacing: capture began 0.1–0.25 ms after its tick at the median; p99 0.6–1.7 ms (worst run 3.1 ms)
```

| Plan metric | Result |
|---|---|
| P4 pacing error p99 ≤ 1 ms (capture time vs tick) | **0.57 – 0.70 ms in 4 steady runs, 1.5 – 3.1 ms in 2 others: not reliably met.** The 3.1 ms run also had three very slow disk writes (E4102) and was the one right after the 5-minute soak, so the machine was busy; the cause of the other is not known. A game slower than the recording rate is late by design (p50 11 ms at 45 fps), and the hook says so with W1301 |
| P7 host CPU ≤ 2 logical CPUs at 720p60 lossless | **0.21** logical CPUs (of 4) for `rec.exe` (the encoder, receiver and writer together; the 2-thread encoder at 5 ms per 16.7 ms frame) |
| P8 host memory ≤ 400 MB | **294 MB** private (217 MB working set) at 720p with the default 256 MB queue; at 1080p the arena is the same, the rest grows with the frame size |
| P6 stop → file closed | the summary appears within 0.1 – 0.5 s of F9 (M2 checked the capture; now it includes padding, the last writes and closing the file) |
| P9 unexplained frame loss in the test app | 0 (above) |

Compression of the test app is 9.2:1, which says nothing about a game: the test app is flat colour
and barcode. Minecraft's measured ratio from the codec work is 5.7:1.

## Problems found and fixed while testing

- **Grid phase (lock mode).** With the plan's rule the pacer only ever waits for early frames, so a game
  running at exactly the recording rate that enters at a bad phase stays late by that phase (pacing p99
  up to 15 ms in short recordings). The grid now starts at the first captured frame and follows the game's
  phase, bounded to one tick so the video stays tied to the wall clock (D-079). My first version had no
  bound and slowed a 45 fps game's recording to 551 frames in 12 s instead of 720: caught by the
  slower-game test, fixed.
- **Timer oversleep.** A 65 ms oversleep of the wait timer once produced a frame stamped with the wrong
  tick; the tick is now decided after the wait.
- **File-name clash.** Two recordings starting in the same hundredth of a second chose the same name; the
  name is now reserved atomically (D-085).
- **Warnings that fired on a healthy disk.** The plan's fixed 50 ms for an 8 MiB write is 160 MB/s; slow
  writes are now judged against the recording's data rate (D-081).
- **Frames in flight at a freeze** (not fixed, see below).

## Found on the first fullscreen Minecraft recording

The user recorded Minecraft fullscreen (1366×745 → 1280×720, locked): 26.6 s, 1,596 frames, but 34 frames
were dropped by the hook because the host fell behind (W1202), 38 ticks were filled with DUPs, the disk
averaged 24 MB/s with 45 very-slow writes (E4102: 8 MiB in ~290 ms), and the encoder had spikes (p99 48 ms,
max 116 ms, against 6 ms on average). The packet queue stayed at 0–2% the whole time, so the disk kept up;
the host's receiver/encoder was being starved. A user-mode CPU load (3 busy processes) did not reproduce it.
The cause was found by looking at the drive: **the whole C: drive is NTFS-compressed**, and the recording was
being compressed a second time by NTFS on every write. With the same noisy test video, a compressed folder gave
27 MB/s and an uncompressed one 390 MB/s. The disk writer now creates every recording uncompressed (D-086) and
the frame ring is 16 slots deep instead of 8 (D-087). After the fix, noisy video into the same compressed
folder: **388 MB/s, 0 very-slow writes**, file created uncompressed. Not yet confirmed on a fullscreen
Minecraft recording (the user's next run).

**Second fullscreen run (disk fixed).** Disk 230-488 MB/s, but 92 frames dropped (W1202) and 226 ticks filled with
DUPs. Per-second telemetry: the game froze at 8-12 s (handled as a stall), then at 18-30 s encode time rose to
10-29 ms average (max 191 ms) with the game's own frame times normal and the packet queue empty: the host's
encoder was starved of CPU by the foreground game. Mitigation (D-088): encoder and receiver threads at
above-normal priority, EcoQoS opt-out. Stress test with 4 busy above-normal processes on the 4 logical CPUs:
encode 6.3 ms average vs 5.5 unloaded, 0 frames filled. Not yet confirmed on the real game.

## Known limits

- **Frames lost to stall latency (D-078).** When a game stops presenting, the last ~5 frames before the
  stop stay in the GPU read-back pipeline until it resumes, then arrive too late and are dropped (W2301).
  The video freezes ~80 ms before the game did. `rec verify` reports these separately (at most 8, only
  after a run of 15+ DUPs). Fixing it needs read-backs without a `Present` call, which means touching the
  game's immediate context from another thread.
- **P4 pacing p99** above (not reliably met).
- **Disk stalls.** In one test-app run the disk averaged 30 MB/s with three E4102 events (NTFS compression of
  the drive, see above, not the SSD); nothing was lost because the 256 MB queue absorbed it. With a slower disk and a longer stall the
  queue fills and frames are dropped (W3104): the rate controller that prevents that is M5.
- **Not done:** crash safety (periodic index flush, `rec repair`: M7), FAT32 splitting (M7), disk-space
  guard and `bench-disk` (M5), audio (M6). A recording that is killed mid-way has no final header and
  does not open until repaired.
- **Direct3D 11 only, 64-bit only**, as before. The one unexplained blip from M2 (game handles 197 → 232
  in one run of the 20-cycle test) happened twice more in the first run after a full rebuild and not in
  nine other runs, including after a hook-only rebuild; still unexplained.
- Telemetry columns `map_copy_ms` and `audio_drift_ms` are empty (not measured / no audio), and
  `pacing_error_ms` is an addition at the end.

## Next: M4

OpenGL backend (PBO read-back) with host BGRA→I420 conversion, the Minecraft recording, and the overhead
A/B against FRAPS. Minecraft Bedrock is Direct3D 11, so it can already be recorded with this build; the
FRAPS comparison on the same scene is what M4 adds.
