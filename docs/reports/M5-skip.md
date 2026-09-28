# RCV1 — M5 report (temporal skip, P-frames, DUP, keyframes)

**Date:** 2026-09-28 · **Codec state:** M5 — phase A skip compare, P-frames, automatic DUP,
keyframe logic, in-place reference update; AVX2, 2 threads · **Tool:** `rcv_bench --skip both`

## Verdict

| Criterion (codec plan §2.3, §13 M5) | Target | Measured | Status |
|---|---|---|---|
| Frame-type tests | §5.2 rules | DUP on identical, P on partial change, I on full change / keyframe interval / `force_keyframe` / skip disabled; P after seek -> `NO_REFERENCE` | **met** |
| A1 Lossless round trip | bit-exact | every frame of both corpora with skip on and off, 1 and 2 threads; P-frame tests over 5 sizes (incl. partial edge blocks), both predictors, 3 slice counts, NV12 | **met** |
| A3 Determinism | identical bytes | P/DUP sequences identical for scalar/SSE4.1/AVX2 × 1–4 threads; NV12 = I420 packets | **met** |
| A10 Duplicate frame | 8 bytes, < 1 µs, no input | 8 bytes; `rcv_encode_duplicate` < 1 µs mean over 20,000 calls (asserted in tests) | **met** |
| A8 Skip compare, 1360×744 | ≤ 0.3 ms | frames where it ran: **0.26 ms** p50 (1 thread), **0.20 ms** (2 threads). Fully unchanged frames (whole frame compared): 0.44 ms (1 thread), **0.28 ms** (2 threads); p99 ≈ 1 ms | **met at p50 with the default 2 threads; not for a single-thread whole-frame compare** (memory-bandwidth bound, see below) |
| A6 Encode with skip on | ≤ 4.0 ms (1 thread), ≤ 2.5 ms (2) | 3.11 ms / **1.69 ms** p50 (encoder only) | met |

## Compression — Minecraft 1360×744 (1,597 frames, 2 threads)

| | Real-frame ratio (median) | Aggregate | **Whole file, incl. duplicates** | Avg / peak MB/s @60 fps |
|---|---|---|---|---|
| **RCV1, skip on (M5)** | **4.210** | **5.168** | **5.728** | **15.9 / 25.6** |
| RCV1, skip off (= M4) | 3.657 | 3.874 | 4.293 | 21.2 / 29.1 |
| FRAPS | 2.832 | 3.030 | 3.354 | 27.2 / 36.2 |

- Frame types with skip on: **52 I, 1,389 P, 156 DUP**; a P-frame skips **30% of its blocks** on average
  (static sky, terrain and HUD while the player moves). The best P-frame compresses 40:1.
- **Files are 25% smaller than M4 and 41% smaller than FRAPS**; the peak write rate falls from FRAPS's
  36 MB/s to 26 MB/s.

## Compression — Warframe 1280×720 (790 frames)

Temporal skip gains nothing here, and costs nothing: the camera is always moving, so only 25 frames
had any unchanged block (0.19% of blocks on average). All 346 duplicate frames were detected by
phase A itself and coded as 8-byte DUPs. Real-frame median stays 1.708:1 (FRAPS 1.580:1), whole file
3.079:1 (FRAPS 2.838:1).

## Speed

Encoder only (`--no-decode`, as the recorder runs it), p50 / p99 per coded frame:

| Corpus | Threads | Skip on | Skip off |
|---|---|---|---|
| Minecraft | 1 | 3.11 / 6.06 ms | 3.00 / 5.25 ms |
| Minecraft | 2 | **1.69** / 3.92 ms | 1.58 / 3.60 ms |
| Warframe | 2 | 1.70 / 3.37 ms | 1.68 / 3.58 ms |

Temporal skip costs ~0.1 ms per frame (the compare, partly paid back by coding fewer samples: mean
encode time is actually lower with skip on, 3.16 vs 3.26 ms single-threaded). Decoding: 2.89 ms p50
on 2 threads with P-frames, unchanged from M4.

Encoder stages, Minecraft, 1 thread, mean ms per coded frame (skip compare per frame it ran on):

| | Skip compare | Load | Predict + histogram | Table build | Entropy write | Total |
|---|---|---|---|---|---|---|
| Skip on | 0.34 | 0.21 | 1.20 | 0.21 | 1.17 | 3.16 |
| Skip off | – | 0.21 | 1.10 | 0.28 | 1.62 | 3.26 |

### Why A8 is only partly met

On a changing frame the compare stops after about one row per block row (≈0.03–0.05 ms). On a
frame where nothing (or most things) stayed the same, every sample of both the new frame and the
reference must be read — about 3 MB at 1360×744 — and at the laptop's single-thread memory bandwidth
that takes ~0.44 ms. Changing the access order (block-wise vs. row-wise sweeps) made no difference,
and running the encoder without the benchmark's decoder didn't either, so it is bandwidth, not
cache misuse. Two threads bring it to 0.28 ms. Per-block hashes could halve the reads, but a hash
collision would silently skip a changed block and break the lossless guarantee
([D-030](../DECISIONS.md)). In the frame budget (16.7 ms at 60 fps) the worst case is ~3%.

## What was built

- **Phase A** (`src/skipmap.cpp`): per-slice jobs; SSE2 row-wise sweep with per-block flags and
  early exit; NV12 chroma compared against the planar reference on the fly.
- **Frame-type decision** per §5.2; `rcv_encode_frame` can return a DUP itself.
- **P-frame encoding**: skip map (§6.3), fully skipped rows free, whole-row copy + SIMD residuals +
  compaction for partial rows ([D-031](../DECISIONS.md)).
- **P-frame decoding**: strict skip-map validation, segment-wise row rebuild; skipped blocks keep the
  reference samples in place ([D-032](../DECISIONS.md)).
- **Tools**: `rcv_bench --skip on|off|both --no-decode` with a frame-type table and phase A timings;
  `rcv_cli stats` reads P-frames and reports skipped blocks.
- **Tests** (46): §5.2 frame types, keyframe interval counting DUPs, `force_keyframe`, skip disabled;
  P-frame round trips over a change sequence (identical, one pixel, moving square, chroma-only,
  edge block, noisy region, scene change) at 5 sizes × 2 predictors × 3 slice counts; NV12 = I420;
  ISA × thread determinism; skip-map packing and padding; bad P-frames; random corruption of P/DUP
  packets; `rcv_encode_duplicate` speed. All pass with every ISA forced, 1 and 4 threads, and under
  clang-cl; 30 low-priority repeated runs without a hang or failure.

## Reproduce

```
build.bat release
build\release\codec\rcv_bench.exe -i corpus\minecraft_1360x744.yuv -s 1360x744 --threads 1,2 --skip both
build\release\codec\rcv_bench.exe -i corpus\minecraft_1360x744.yuv -s 1360x744 --threads 1,2 --skip both --no-decode
```
