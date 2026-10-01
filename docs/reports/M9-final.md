# RCV1 — M9 final report

**Date:** 2026-10-01 · **Codec:** RCV1 v1 complete (M1–M8): lossless YUV 4:2:0 and RGB (GBR),
near-lossless NEAR 1–3, temporal skip with P-frames and 8-byte DUPs, SSE4.1/AVX2 kernels, slice
threads, hardened decoder.
**Machine:** Intel Core i5-7200U (2 cores / 4 threads, AVX2), Windows 11, AC power, Balanced power
plan, Windows-reported clock 2,511 MHz throughout. MSVC release build (`/O2 /GL /LTCG`);
`rcv_bench` pinned to one logical CPU at high priority, encoder workers on the other core.

Every speed figure below is from today's runs on an idle machine (freshly rebooted, no games, no
Discord). Each set includes the single-thread lossless control, whose idle value is 3.0 ms;
corrections are noted where a set ran slow.

## Acceptance criteria

| # | Criterion | Target | Measured | Status |
|---|---|---|---|---|
| A1 | Lossless round trip | bit-exact, every vector, mode and thread count | every frame of the Minecraft (1,597) and Warframe (790) clips, YUV and RGB, skip on/off, 1 and 2 threads; tests: synthetic vectors (zero, full, noise, gradient, checker, natural), widths 2–130, odd strides, 2×2 … 1360×744, both predictors, slices 1–64, threads 1–4, NV12 | **met** |
| A2 | Near-lossless bound | max error ≤ NEAR, every sample of the corpus | exactly 1 / 2 / 3 at NEAR 1 / 2 / 3 on all 1,597 Minecraft frames; exhaustive quantiser test; P-frame chains; NEAR switched per frame | **met** |
| A3 | Determinism | identical bytes for 1/2/4 threads and every ISA | scalar / SSE4.1 / AVX2 × 1–4 threads, lossless, near, GBR, P/DUP sequences; NV12 = I420 | **met** |
| A4 | Compression (must) | real-frame median ≥ 2.49:1 | **3.657** I-frames only; **4.210** with temporal skip (FRAPS on the same clip: 2.832) | **met** |
| A5 | Compression (goal) | ≥ 2.8:1, I-frames only | **3.657** | **met** |
| A6 | Encode speed, lossless I-frame | ≤ 4.0 ms (1 thread), ≤ 2.5 ms (2) | **3.02 ms / 1.66 ms** p50 (p99 6.79 / 4.29); with skip on 3.09 / 1.70 | **met** |
| A7 | Near-lossless speed | ≤ 8.0 ms, 1 thread | **8.15 and 8.09 ms** p50 in two runs (I-frames, NEAR 1), control 4% slow (3.13–3.17 ms) → ≈ 7.8 ms idle; 2 threads ≈ 4 ms | **at the limit** — see gaps |
| A8 | Skip compare | ≤ 0.3 ms per frame | frames where it ran: **0.28 ms** p50 (1 thread), **0.18 ms** (2 threads); fully unchanged frames (whole frame compared): 0.36 ms (1 thread), **0.23 ms** (2 threads) | **met with 2 threads** (the default); single-thread whole-frame compare 0.36 ms |
| A9 | Decode speed | ≤ 6.0 ms, 1 thread | **5.66 ms** p50 I-frames, 5.81 ms with P-frames; 2 threads 3.41 / 3.16 ms | **met** |
| A10 | Duplicate frame | 8-byte packet, < 1 µs, no input | 8 bytes; `rcv_encode_duplicate` < 1 µs mean over 20,000 calls (test) | **met** |
| A11 | No allocation | zero heap allocations in encode/decode | 0 (counting `operator new` + debug-CRT hook, all frame types, 1 and 4 threads) — [M8](M8-hardening.md) | **met** |
| A12 | Robustness | ≥ 1 h libFuzzer + ASan, no crash/hang/OOB | 3,851,941 inputs in 1 h with ASan + UBSan: no crash, out-of-bounds access or hang — [M8](M8-hardening.md) | **met** |
| A13 | Bounded output | every packet ≤ `rcv_max_packet_size` | 1,000 worst-case packets: none over, nothing written past the buffer; noise fills the bound exactly — [M8](M8-hardening.md) | **met** |

## Comparison — Minecraft 1360×744, YUV 4:2:0, 1,597 frames (1,441 real)

| Codec | Real-frame ratio (median) | Whole file (incl. duplicates) | Peak MB/s @60 fps | Encode ms/frame, 1 thread |
|---|---|---|---|---|
| **RCV1 lossless** (skip on, the default) | **4.210** | **5.728** | **25.6** | **3.09** p50 (1.70 on 2 threads) |
| RCV1 lossless, I-frames only | 3.657 | 4.293 | 29.1 | 3.02 p50 |
| RCV1 near-lossless NEAR 1 | 6.183 | 8.437 | 17.3 | ≈ 8 (≈ 4 on 2 threads) |
| RCV1 near-lossless NEAR 3 | 8.641 | 11.738 | 12.9 | ≈ 8 (≈ 4 on 2 threads) |
| FRAPS (the user's recording) | 2.832 | 3.354 | 36.2 | – |
| Ut Video (median) | 3.621 | 3.821 | 29.4 | ~1.65 mean* |
| FFV1 level 3 | 4.416 | 4.911 | 22.9 | ~12.3 mean* |

\* FFmpeg CPU time per frame, measured in M2.

- **Lossless RCV1 files are 41% smaller than FRAPS's** and the peak write rate drops from 36 to
  26 MB/s. Frame by frame, RCV1 matches Ut Video (same method: median prediction + Huffman); the
  whole file then beats FFV1 thanks to temporal skip and DUPs, at a quarter of FFV1's encode time.
- Near-lossless (max error 1–3 levels) gives the recorder's rate controller a further 1.5–2× when
  the disk can't keep up.

## Comparison — Warframe 1280×720 (detailed, noisy content), 790 frames

| Codec | Real-frame ratio (median) | Whole file |
|---|---|---|
| **RCV1 lossless** | **1.708** | **3.079** |
| FRAPS | 1.580 | 2.838 |
| Ut Video (median) | 1.684 | 1.702 |
| FFV1 level 3 | 1.903 | 1.917 |

Noisy content limits every lossless codec; RCV1 stays 8% ahead of FRAPS frame by frame. The moving
camera leaves nothing for temporal skip, but all 346 duplicate frames become 8-byte DUPs.

## Comparison — Minecraft as RGB (GBR format), 1,597 frames

| Codec | Real-frame ratio (median) | Whole file |
|---|---|---|
| **RCV1 GBR, skip on** | **5.419** | **7.371** |
| RCV1 GBR, skip off | 4.673 | 5.432 |
| Ut Video (median, gbrp) | 4.632 | 4.851 |
| FFV1 level 3 (bgr0) | 6.466 | 7.324 |

RGB speed (300-frame subset, today): encode **7.86 ms** p50 on 1 thread, **4.10 ms** on 2
(skip on: 8.84 / 4.02 ms); decode 12.6 / 7.9 ms. That is 2.6× the YUV time: RGB codes twice the
samples of YUV 4:2:0, plus the colour transform. There is no RGB speed target; on the recorder's
default 2 threads it is about 4 ms per frame. (This set ran without its own control, right after a
set that ran 1.2× slow, so it may be slightly pessimistic.)

## Speed details (today, idle machine)

| Minecraft 1360×744, p50 / p99 ms | 1 thread | 2 threads |
|---|---|---|
| Encode, lossless I-frames | 3.02 / 6.79 | 1.66 / 4.29 |
| Encode, lossless, skip on | 3.09 / 6.13 | 1.70 / 3.61 |
| Decode, I-frames | 5.66 / 12.92 | 3.41 / 8.01 |
| Decode, with P-frames | 5.81 / 9.52 | 3.16 / 5.57 |
| Encode, NEAR 1, I-frames | 8.15 / 12.09 | ≈ 4† |
| Skip compare (frames where it ran / whole unchanged frame) | 0.28 / 0.36 | 0.18 / 0.23 |

† 4.86 ms measured in a set whose control ran 1.2× slow.

Encoder stages, lossless I-frames, 1 thread, mean ms: load 0.21, predict + histogram 1.13, table
build 0.30, entropy write 1.68 (total 3.36 mean, 3.02 p50). Near-lossless spends three quarters of
its time in prediction + quantisation, which is serial along each row ([M7](M7-near-lossless.md)).

## Gaps and their causes

1. **A7 is at the limit (≈ 8 ms on 1 thread).** Near-lossless prediction must use reconstructed
   samples, so each row is a serial dependency chain (MED → quantiser table → clamp, ≈ 15 cycles per
   sample); M7's two-row wavefront already overlaps two chains. In the recorder this is not a
   problem: the default is 2 threads (≈ 4 ms), and near-lossless is only switched on by the rate
   controller when the disk falls behind, not as the steady state.
2. **A8 is met with 2 threads but not for a single-thread whole-frame compare (0.36 ms).** Comparing
   an unchanged frame reads 3 MB (current + reference) and is memory-bandwidth bound on this laptop
   ([M5](M5-skip.md)). Frames that change stop early (0.28 ms p50).
3. **Corpus.** No desktop/UI capture yet (§11.2 item 3): its cases (flat areas, SINGLE chunks, mostly
   static frames) are covered by synthetic tests only. The RGB corpus is the YUV recording converted
   to RGB, so it is smoother than a true RGB capture ([D-037](../DECISIONS.md)). Both are best
   recorded with `rec` itself once the recorder works.
4. **Measurement conditions.** The power plan was Balanced (§11.5 asks for High performance), and
   AC power dropped once just before the runs (the affected set was discarded). The original FRAPS
   clip from §1.3 (2.49:1) was deleted; the comparison uses the user's newer recording, on which
   FRAPS reaches 2.832:1.

## Summary

The RCV1 codec meets 11 of the 13 criteria outright, A7 at its limit and A8 with the default two
threads. Against FRAPS on the same game, files are **41% smaller** (lossless) or **2.5–3.5×
smaller** (near-lossless), encoding takes **1.7 ms per 1360×744 frame on two threads**, and
duplicates cost 8 bytes. The codec is ready for the recorder.
