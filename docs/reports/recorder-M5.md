# rec — recorder M5 report (rate controller, bench-disk, monitors, fault injection)

**Date:** 2026-10-02 · **Plan:** `Recorder_Implementation_Planv2.md` §9, §10.4, §11.4, §14.3, §16 M5 · **Decisions:** D-094 – D-097

A recording now protects itself: when the disk cannot keep up it compresses harder and then drops frames on
purpose (as repeated frames), when the CPU cannot keep up it drops frames, and it watches free space, power and the
machine's CPU. `rec bench-disk` measures the drive and the result is checked at the start of every recording. Every
fault the plan lists can be injected from the command line, and `tests/m5_faults.ps1` checks that each one
produces its log codes and the expected behaviour. Tested on the synthetic test app (a window parked off the
desktop); no real game was recorded with a fault or with the rate controller acting.

## Acceptance

| Check (§16 M5) | Result | Status |
|---|---|---|
| Every fault flag produces the right codes and behaviour | `m5_faults.ps1`: all 9 sections pass (table below) | **met** |
| `--debug-throttle-disk 20` stays in sync with no stalls | The test app's video needs ~14-15 MB/s, so 20 never bites; the same checks pass at 10 MB/s (14 MB/s needed: NEAR 1 for 13-15 s of 25) and at 6 MB/s with 1080p native video (rate controller acts, no frame lost to the queue, one frame per tick, game at ~60 fps, `rec verify` PASS) | **met**, with a slower disk than the plan's number |
| All §10.4 codes wired | W1203, W1210, W3101, W3102, W3103, W4001, W4103, E4104, W6101, W6102, I6002 are new; E4105 was there; **I4106 (file split) is not**: it comes with FAT32 splitting in M7 | **met except I4106** |
| `bench-disk`, startup disk check, `doctor` | Done: 446 MB/s measured on this laptop's C: drive, `doctor` shows it, a cached 20 MB/s makes the next recording log W4001 | **met** |

Unit tests: `rec_tests` **56/56** in release and debug (new: rate controller ×8, three pipeline tests with a throttled
disk, a full frame ring and `rcv-strict`, the monitors ×4, disk benchmark and cache). Release, debug and x86
builds: 0 warnings. Regression: `m4_opengl.ps1` PASS, `m3_record.ps1` PASS (see "one failure" below),
`m1_attach_detach.ps1` (30 cycles) PASS.

## The fault flags (`m5_faults.ps1`, test app, 720p60 unless noted)

| Flag | What happened |
|---|---|
| `--debug-throttle-disk 10` (video needs ~14 MB/s) | Left lossless after the queue passed 40%: NEAR 1 for 13-15 of 25 s, W3101 logged, 0 frames lost to the queue, game 60 fps, file verifies |
| `--debug-throttle-disk 6` (1080p native) | Controller acted, video one frame per tick, 0 queue drops, game ~60 fps |
| `--debug-write-stall 500 3` | E4102 reported, nothing lost, file verifies |
| `--debug-encoder-delay 22` | W3103 and W3102, frames dropped on purpose, video as long as the recording, no W1202, game 60 fps |
| `--debug-drop-readback 10` | W1201; about a tenth of the ticks became repeated frames; none shows a new picture |
| `--debug-hook-throw` | E1107 in the recording's log, the game kept running, recording ended and the file verifies |
| `--debug-fill-disk` | W4103 at 5 GB, E4104 at 1 GB (about 7 s in), recording stopped by itself, file finished, game running |
| `--debug-device-removed` | E1209 once, capture resumed (under 10% repeated frames), no capture error |
| `--debug-kiero-fail d3d11` | E1105, "the hook could not install", game running; `opengl` on a D3D app changes nothing |

## Things worth knowing

- **The queue absorbs a lot before the controller acts.** The packet queue is 256 MB, and level 1 starts at 40%
  (~100 MB). A disk 4 MB/s short of the video's rate therefore takes ~25 s to get to NEAR 1, which is the design
  (a brief slow patch is absorbed without losing quality), not a delay.
- **On this test content NEAR 1-3 and dropping were reached only in the unit tests** (and level 4 in
  `test_pipeline`); the script reached NEAR 1. The test app's flat colour and noise squares compress by a
  fixed amount, so slowing the disk further mostly fills the queue; with a real game this will look different.
- **FRAPS blocks the hook.** With FRAPS running (it loads into every new process) the hook saw no frames in fresh
  test processes. `m5_faults.ps1` refuses to run while FRAPS is running. This is not new, but it is the first time
  it stopped a test; Minecraft with FRAPS in M1 did work, so it depends on how FRAPS hooks the process.
- **One failure that did not repeat:** the first `m3_record.ps1` run after the final rebuild failed three checks in
  the 15 s steady recording (12 DUPs, something dropped) while the machine was busy right after the build. A plain
  recording straight afterwards had 0 drops and the second run of the whole script passed. If it comes back, look
  at whether the rate controller's CPU rule fired during a transient (the log would show W3102).
- A **plain 15 s recording** logs 14 × W1203 (hook over 1 ms in one second) and W3103 once (18.8 ms for one frame); both
  are real and rate-limited to once a second.

## Known limits

- I4106 (file splitting) and FAT32 handling: M7.
- The rate controller was never exercised with a real game, and the CPU rule's thresholds are the plan's
  (ring 50% / 25%, 80% of the frame budget) with my guard on the encode-time rule (D-095).
- `--debug-device-removed` is simulated inside the backend; a real removed device is the game's to recreate.
- `bench-overhead` is still not implemented (skipped at the user's request in M4).

## Next: M6

Audio: WASAPI capture of the game's audio (or the system's), A/V sync, a second track for the microphone.
