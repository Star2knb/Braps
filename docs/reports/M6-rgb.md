# RCV1 — M6 report (lossless RGB: GBR format, BGRA input and output)

**Date:** 2026-09-28 · **Codec state:** M6 — GBR format (§4.1, §4.4), BGRA/BGRX input, BGRA output,
P-frames and DUP on RGB · **Tools:** `rcv_bench --format gbr`, `codec/bench/compare_rgb.ps1`

## Verdict

| Criterion | Target | Measured | Status |
|---|---|---|---|
| A1 on RGB vectors (M6 gate) | bit-exact | every frame of the 1,597-frame Minecraft clip as BGRA, skip on and off; tests: 6 sizes incl. odd (2×2 … 321×179) × 6 contents × 2 predictors × 3 slice counts; P/DUP sequences | **met** |
| A3 on RGB | identical bytes | scalar / SSE4.1 / AVX2 × 1–4 threads; BGRX with random alpha = BGRA | **met** |
| Colour kernels | equal to scalar | BGRA→GBR and GBR→BGRA, widths 1–130 | **met** |
| Speed | (no RGB target in the plan) | **not yet measured cleanly** — see below | pending |

## Compression — Minecraft as RGB (1,597 frames, 1360×744)

All codecs see the same BGRA pixels (converted from the FRAPS recording by FFmpeg and piped, so no
6.5 GB corpus on disk); ratios use 3 bytes per pixel as the raw size.

| Codec | Real-frame ratio (median) | Whole file (incl. duplicates) | Peak MB/s @60 fps | Encode (1 thread) |
|---|---|---|---|---|
| **RCV1 GBR, skip on** | **5.419** | **7.371** | 39.2 | see below |
| RCV1 GBR, skip off | 4.673 | 5.432 | 43.2 | |
| Ut Video (median, gbrp) | 4.632 | 4.851 | 43.4 | ~7.2 ms* |
| FFV1 level 3 (bgr0) | 6.466 | 7.324 | 29.3 | ~21.4 ms* |

\* FFmpeg user CPU time per frame, including the BGRA→planar rearrangement.

- As on YUV, RCV1 without skip matches Ut Video (+0.9%): same idea — median prediction + Huffman.
- With temporal skip, the **whole file ends up slightly smaller than FFV1's** (7.37 vs 7.32), at a
  fraction of FFV1's cost; frame by frame FFV1's context modelling still wins (6.47 vs 5.42 median).
- Caveat ([D-037](../DECISIONS.md)): this "RGB" is YUV 4:2:0 converted back to RGB, so its colour is
  smoother than a true RGB capture. Real RGB numbers need captures from the recorder (D3D/OpenGL
  readback), which the codec can now take directly.

## Speed — pending a quiet machine

The speed runs were contaminated: **Warframe, Roblox and Discord were running** during the
measurements (Warframe had used 962 s of CPU). The YUV control benchmark, 3.0 ms/frame on the quiet
machine in M5, measured 14.3 ms under the same load, so every absolute timing from that session is
~4–5× too slow and is not reported.

Relative figure from the same loaded session (indicative only): single-thread RGB encoding took
**1.65×** as long as YUV (23.5 vs 14.3 ms p50, skip off). RGB codes 2× the samples of YUV 4:2:0 but
skips YUV's copy into the reference (staging swap, [D-034](../DECISIONS.md)), so on the quiet machine
this suggests roughly **5 ms single-threaded, ~2.7 ms on two threads** for 1360×744 RGB — to be
confirmed with:

```
build\release\codec\rcv_bench.exe -i corpus\minecraft_rgb300.bgra -s 1360x744 --format gbr --threads 1,2 --skip both --no-decode
```

## What was built

- **GBR format** (§4.1, §4.4): planes G, B−G, R−G; odd sizes allowed; alpha ignored; near-lossless
  rejected as invalid for GBR.
- **Encoder**: a prepare pass per slice converts BGRA into staging planes and, in the same job, runs
  the phase A skip compare on them; phase B predicts from the staging planes, which then become the
  reference by pointer swap — no copy ([D-034](../DECISIONS.md)).
- **Decoder**: BGRA output (alpha 255) for GBR streams; YUV↔RGB combinations are rejected, since the
  codec never converts colour spaces ([D-035](../DECISIONS.md)).
- **SSE2 colour kernels** (`src/colour.cpp`), 16 pixels per step, x64 baseline ([D-036](../DECISIONS.md)).
- **Tools**: `rcv_cli encode -f rgb` / decode / verify / stats with BGRA; `rcv_bench --format gbr` and
  `-i -` (stdin); `compare_rgb.ps1` for the piped Ut Video / FFV1 comparison.
- **Tests** (52): colour kernels vs scalar; RGB round trips; alpha ignored; RGB P/DUP sequences with
  ISA × thread determinism; argument and output-layout checks; max packet size. All pass with every
  ISA forced, 1 and 4 threads, debug, and clang-cl; 20 low-priority repeated runs under load without
  a hang or failure. A decoded frame was rendered to PNG to confirm the channel order visually.

## Note for the recorder

The Minecraft clip is **Bedrock Edition** (v26.52), not Java. Bedrock renders with DirectX, and
FRAPS (D3D ≤ 11) recorded it, so it ran on D3D11 — the recorder's first backend. The recorder plan's
assumption (§4.3) that the test game uses OpenGL should be updated.
