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
| RCV1 codec | M2 — benchmark harness, compression report vs FRAPS | next (needs the FRAPS clip) |
| Recorder | M0 | not started |

## Build
Requires Visual Studio (2022 or 2026) with "Desktop development with C++".

```
build.bat release        (or: debug, clang-debug)
build\release\codec\rcv_tests.exe
```

## rcv_cli
```
rcv_cli encode -i in.yuv -s 1360x744 -r 60 -o out.rcv
rcv_cli verify -i in.yuv -c out.rcv
rcv_cli stats  -i out.rcv
rcv_cli decode -i out.rcv -o - | ffmpeg -f rawvideo -pix_fmt yuv420p -s 1360x744 -r 60 -i - -c:v libx264 -crf 16 out.mp4
```
Raw input is planar I420 (`ffmpeg ... -pix_fmt yuv420p -f rawvideo out.yuv`).
