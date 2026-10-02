# rec — frame-buffer-hook screen recorder

Plans: [RCV1_Codec_Plan.md](RCV1_Codec_Plan.md) (codec) and
[Recorder_Implementation_Planv2.md](Recorder_Implementation_Planv2.md) (recorder).
Decisions made during implementation: [docs/DECISIONS.md](docs/DECISIONS.md).

This is the V2 rewrite, developed on the `braps-v2` branch. The V1 recorder (from `main`) is kept
unchanged in [legacy/](legacy/) for reference; it is not part of the V2 build.

## Status
| Part | Milestone | State |
|---|---|---|
| RCV1 codec | M1 — scalar lossless YUV420 I-frame encoder/decoder, CLI, tests | done |
| RCV1 codec | M2 — benchmark harness, compression report vs FRAPS | done — [report](docs/reports/M2-compression.md) |
| RCV1 codec | M3 — SSE4.1/AVX2 kernels, faster Huffman | done — [report](docs/reports/M3-speed.md): encode 3.18 ms, decode 5.26 ms (1 thread) |
| RCV1 codec | M4 — thread pool, slice-parallel encode/decode | done — [report](docs/reports/M4-threads.md): encode 1.60 ms, decode 2.87 ms (2 threads) |
| RCV1 codec | M5 — temporal skip (P-frames), auto-DUP, keyframe logic | done — [report](docs/reports/M5-skip.md): Minecraft files 41% smaller than FRAPS |
| RCV1 codec | M6 — lossless RGB (GBR format, BGRA input/output) | done — [report](docs/reports/M6-rgb.md) |
| RCV1 codec | M7 — near-lossless mode, per-frame NEAR switching | done — [report](docs/reports/M7-near-lossless.md): NEAR 1 = 8.4:1, NEAR 3 = 11.7:1 whole file |
| RCV1 codec | M8 — hardening: validation audit, CRC-32C, no-allocation test, fuzzing | done — [report](docs/reports/M8-hardening.md) |
| RCV1 codec | M9 — final report, all criteria | done — [report](docs/reports/M9-final.md): files 41% smaller than FRAPS, 1.7 ms/frame on 2 threads |
| Recorder | M0 — layout, CMake x64/x86, CLI skeleton, logging, `rec doctor` | done — [report](docs/reports/recorder-M0.md) |
| Recorder | M1 — test app, injection, kiero2 + MinHook `Present` hook (measuring only), `rec list` | done — [report](docs/reports/recorder-M1.md) |
| Recorder | M2 — D3D11 capture (scale + NV12 shader, staging ring), shared frame ring, F9 hotkey and cue | done — [report](docs/reports/recorder-M2.md): 0.3 ms median hook time per captured frame, stop in 0.1–0.3 s |
| Recorder | M3 — RCV1 encoder, timeline with DUP filling, lock/free pacing, AVI writer, `rec verify`, `rec convert` | done — [report](docs/reports/recorder-M3.md): lossless 720p60 recordings, 9.2:1 on the test app, nothing lost; Minecraft not yet tried |
| Recorder | M4 — OpenGL backend (blit + PBO read-back), host BGRA→NV12 conversion | done — [report](docs/reports/recorder-M4.md): the OpenGL test app records losslessly (hook 0.2 ms median, host conversion 1 ms); recorded Geometry Dash too (PASS, nothing dropped) |

## Build
Requires Visual Studio (2022 or 2026) with "Desktop development with C++".

```
build.bat release        (or: debug, clang-debug; x86-release / x86-debug for the 32-bit parts)
build\release\codec\rcv_tests.exe
build\release\tests\rec_tests.exe
```
The recorder programs (`rec.exe`, `rec_hook64.dll`, `rec_testapp.exe`) are built into `build\release\bin`;
`rec.exe` looks for the hook DLL next to itself.

## rec
```
rec doctor                           check this PC for recording
rec config show                      every setting (%LOCALAPPDATA%\rec\rec.toml)
rec config set record.fps 50         change one setting; rec config reset restores the defaults
rec list                             processes with Direct3D / OpenGL / Vulkan loaded
rec launch game.exe [-- game args]   start a game with the hook in before it creates its device
rec attach --pid N | --name X.exe    hook a running game
rec detach [--pid N | --name X.exe]  remove the hook (default: from every game that has it)
rec verify FILE.avi [--testapp]      check a recording: structure, indexes, every frame decoded
rec convert FILE.avi [--to mp4|mkv] [--crf 16] [--out FILE]   decode and encode with FFmpeg (video only)
rec --help                           all commands (repair, bench-disk, ... arrive with later milestones)
```
`launch`/`attach` show the game's frame rate and the hook's own cost per frame. **F9 starts and stops
recording** (hotkey and sound cue configurable with `--hotkey` / `--no-sound`). Each recording is a
lossless RCV1 video in an AVI file, `<Game> YYYY-MM-DD HH-MM-SS-cc.avi` in `record.out_dir` (default
`%USERPROFILE%\Videos\rec`, or `--out DIR`), with `.frames.csv` (one row per frame), `.summary.json` and
`.log` beside it. Works with Direct3D 11 and OpenGL (3.0 or newer) games, windowed or fullscreen, 64-bit. By default the game is
**locked** to the recording's frame rate (`--no-lock` lets it run free); frames the game didn't present
on time become repeated frames, so the file always has a constant frame rate. Ctrl+C stops a running
recording, removes the hook and exits; the game keeps running.
Options: `--duration S` (detach and exit after S seconds), `--force` (see below), `--record-for S` (start
a recording by itself, for scripts), `--save-frame file.png` (write one captured frame as a picture).
No audio yet (M6), no D3D9 / D3D12 / Vulkan (M8, M10), no 32-bit games (M7).

**Disk.** Recordings are written with large unbuffered writes and are already compressed, so `rec` creates
them without NTFS compression even in a compressed folder (letting NTFS compress them again cut a drive's
speed from ~390 MB/s to ~27 MB/s and starved the encoder). Lossless 720p60 needs 5–15 MB/s in games, more in
busy scenes; `rec doctor` shows the drive and free space.

**CPU.** A game can use every core and starve the recorder, which then loses frames. `rec`'s encoder
threads run at above-normal priority and opt out of Windows power throttling (`record.encoder_priority =
"normal"` turns the priority off).

**Anti-cheat.** Injecting into a game protected by anti-cheat can get your account banned. `rec` looks
for known anti-cheat components (the game's loaded modules, its folder, running processes, kernel
drivers; your own list is `safety.anticheat_blocklist`) and refuses with E1004 if it finds any.
`--force` overrides this, only for games you own and know are safe offline. Don't use it on games with
Easy Anti-Cheat, BattlEye, Vanguard or Hyperion. Only 64-bit games for now; 32-bit arrives with M7.

Test app and scripts (build first):
```
build.bat release
build\release\bin\rec_testapp.exe --vsync --seconds 60          a D3D11 window standing in for a game (--gl: OpenGL)
powershell -File tests\m1_attach_detach.ps1                       100 attach/detach cycles against it
powershell -File tests\m1_robustness.ps1                          host killed, no-D3D process, Present1, anti-cheat
powershell -File tests\m2_capture.ps1                             capture, F9 in a window and in fullscreen, leaks (presses F9!)
powershell -File tests\m3_record.ps1 [-SoakMinutes 5]             recordings: nothing lost, freeze, slower game, verify, FFmpeg
powershell -File tests\m4_opengl.ps1                              OpenGL: SwapBuffers/wgl, core, multisample, letterbox, freeze, launch, cycles
```
Logs: `%LOCALAPPDATA%\rec\logs\rec.log`.

## rcv_cli
```
rcv_cli encode -i in.yuv -s 1360x744 -r 60 -o out.rcv
rcv_cli verify -i in.yuv -c out.rcv
rcv_cli stats  -i out.rcv
rcv_cli decode -i out.rcv -o - | ffmpeg -f rawvideo -pix_fmt yuv420p -s 1360x744 -r 60 -i - -c:v libx264 -crf 16 out.mp4
```
Raw input is planar I420 (`ffmpeg ... -pix_fmt yuv420p -f rawvideo out.yuv`), or packed BGRA with
`-f rgb` (`ffmpeg ... -pix_fmt bgra -f rawvideo out.bgra`), which is stored losslessly as GBR.

## Benchmarks
Test corpora live in `corpus/` (git-ignored). From a FRAPS recording:
```
ffmpeg -i "Minecraft ....avi" -an -f rawvideo -pix_fmt yuvj420p corpus\minecraft_1360x744.yuv
build\release\codec\rcv_bench.exe -i corpus\minecraft_1360x744.yuv -s 1360x744 --predictor both --threads 1,2
powershell -File codec\bench\compare_baselines.ps1 -Yuv corpus\minecraft_1360x744.yuv -Size 1360x744 -Fraps "Minecraft ....avi"
```
`rcv_bench` pins itself to one CPU and prints the machine's power state; run on AC power.

## Fuzzing
The decoder fuzzer (libFuzzer + ASan + UBSan) builds with the clang-cl preset:
```
build.bat clang-debug
build\clang-debug\codec\rcv_fuzz_seeds.exe corpus\fuzz_seeds
cd corpus
..\build\clang-debug\codec\rcv_fuzz.exe -max_total_time=3600 -timeout=10 -artifact_prefix=fuzz_artifacts\ fuzz_corpus fuzz_seeds
```
Create `fuzz_corpus` and `fuzz_artifacts` first; a crash is saved to `fuzz_artifacts\` and can be
replayed with `rcv_fuzz.exe <file>`.
