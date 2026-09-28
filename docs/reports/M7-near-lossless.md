# RCV1 — M7 report (near-lossless mode)

**Date:** 2026-09-28 · **Codec state:** M7 — NEAR 1–3 per frame (§5.5) on I- and P-frames, near
skip compare, decoder support; YUV420 only · **Tools:** `rcv_bench --near`, `rcv_cli --near`

## Verdict

| Criterion | Target | Measured | Status |
|---|---|---|---|
| A2 Near-lossless bound | max \|decoded − source\| ≤ NEAR, every sample | exactly 1 / 2 / 3 at NEAR 1 / 2 / 3 on all 1,597 Minecraft frames; tests: every NEAR × prediction × sample (196,608 cases), I-frames over sizes/contents/predictors/slices, P-frame chains, per-frame NEAR switching | **met** |
| Per-frame NEAR switching (M7 gate) | decodes correctly | sequence switching NEAR every frame (0,1,3,2,…) stays within each frame's bound; lossless frames after near ones are exact | **met** |
| A3 with near-lossless | identical bytes | scalar/SSE4.1/AVX2 × 1–4 threads; NV12 = I420 | **met** |
| A7 Near-lossless speed, 1 thread, 1360×744 | ≤ 8.0 ms | 8.72 ms p50 measured **under load** (control 1.13× slower than idle) → ≈ 7.7 ms estimated idle | **probably met — needs a clean run** |

## Compression — Minecraft 1360×744, 1,597 frames, 2 threads, temporal skip on

| NEAR | Max error | Real-frame median | Whole file | Avg / peak MB/s @60 fps | Frame types (I / P / DUP) |
|---|---|---|---|---|---|
| 0 (lossless) | 0 | 4.210 | 5.728 | 15.9 / 25.6 | 52 / 1,389 / 156 |
| **1** | **1** | **6.183** | **8.437** | **10.8 / 17.3** | 14 / 1,427 / 156 |
| 2 | 2 | 7.460 | 10.142 | 9.0 / 14.6 | 14 / 1,428 / 155 |
| 3 | 3 | 8.641 | 11.738 | 7.8 / 12.9 | 14 / 1,428 / 155 |

- NEAR 1 already cuts the data rate by a third (whole file +47%); NEAR 3 halves it (+105%).
- Tolerant skipping also finds more unchanged blocks (P-frames skip 32–42% of blocks vs 30%), and
  fewer frames need to be I-frames.
- Against FRAPS (3.354:1 whole file on this clip): 2.5× smaller at NEAR 1, 3.5× at NEAR 3 — this
  is the headroom the recorder's rate controller uses when the disk falls behind (recorder plan §9).

## Speed — measured under load, clean run pending

Roblox was running throughout, so absolute timings are too slow; each run was paired with the
lossless YUV control (3.0 ms/frame idle, 1 thread).

| Run | Control (idle 3.0 ms) | Lossless | NEAR 1 | NEAR 3 | Estimate idle NEAR 1 |
|---|---|---|---|---|---|
| 1 thread, encoder only | 3.40 ms (1.13×) | 3.33 ms | 8.72 ms | 8.68 ms | ≈ 7.7 ms |
| 2 threads, encoder only | 5.64 ms (1.9×) | 3.00 ms | 6.25 ms | 6.27 ms | ≈ 3.3 ms |

Where the time goes (1 thread, mean ms per coded frame, under load): prediction+quantisation 5.77,
entropy 1.27, skip compare 0.57. Near-lossless costs ~2.5× lossless because prediction must use the
reconstructed samples and so runs serially along each row (like decoding) instead of in SIMD.

Optimisation steps, NEAR 1, 1 thread (same load level): serial loop 11.8 ms → without the multiply
on the dependency chain and with a **two-row wavefront** (row j+1 one sample behind row j) 8.7 ms.
The remaining limit is the per-sample dependency chain (MED → quantiser table → clamp, ≈15 cycles)
plus register pressure with two rows in flight; the generated code uses conditional moves, not
branches, for MED and the clamps.

## What was built

- **Encoder** (`encoder.cpp`): serial near-lossless coding with reconstructed neighbours, quantiser
  tables (`src/nearlossless.{h,cpp}`), two-row wavefront, P-frame segments; header NEAR + flag.
- **Skip compare** with tolerance (SSE2 saturating absolute difference).
- **Decoder**: reconstruction policy (lossless / near-lossless) across all paths, including lockstep
  pairs and masked P-frame rows.
- **Tools**: `rcv_cli verify` tolerates the stream's NEAR and reports max error; `rcv_bench --near`.
- **Tests** (59): exhaustive quantiser check; I-frame bound; compression monotone in NEAR; no drift
  on P chains and DUP for changes within NEAR of the reconstruction; per-frame NEAR switching;
  ISA × thread determinism and NV12; corruption. All pass with every ISA forced, 1 and 4 threads,
  debug, and clang-cl.

## To finish

One timing session with no games running, covering M6 (RGB) and M7 (near-lossless) together with
the M9 final report:

```
build\release\codec\rcv_bench.exe -i corpus\minecraft_1360x744.yuv -s 1360x744 --threads 1,2 --near 0,1,2,3 --no-decode
build\release\codec\rcv_bench.exe -i corpus\minecraft_rgb300.bgra -s 1360x744 --format gbr --threads 1,2 --skip both --no-decode
```
