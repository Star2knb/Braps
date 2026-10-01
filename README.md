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
| Recorder | M1 — test app, injection, kiero2 + MinHook `Present` hook (measuring only), `rec list` | next |

## Build
Requires Visual Studio (2022 or 2026) with "Desktop development with C++".

```
build.bat release        (or: debug, clang-debug; x86-release / x86-debug for the 32-bit parts)
build\release\codec\rcv_tests.exe
build\release\tests\rec_tests.exe
```

## rec
```
rec doctor                           check this PC for recording
rec config show                      every setting (%LOCALAPPDATA%\rec\rec.toml)
rec config set record.fps 50         change one setting; rec config reset restores the defaults
rec --help                           all commands (launch, attach, convert, ... arrive with later milestones)
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
