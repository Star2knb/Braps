# RCV1 — M2 compression and speed report

**Date:** 2026-09-28 · **Codec state:** M1 (scalar, single-threaded, lossless I-frames + DUP) ·
**Tool:** `rcv_bench` + `codec/bench/compare_baselines.ps1`

## Verdict

| Criterion (codec plan §2.3) | Requirement | Measured | Status |
|---|---|---|---|
| A1 Lossless round trip | bit-exact | all 1,597 corpus frames bit-exact, 4 configurations | **met** (real corpus; synthetic vectors covered by unit tests) |
| A4 Compression (must) | ≥ FRAPS on the same corpus | median **3.657:1** vs FRAPS **2.832:1** (plan's old-clip floor: 2.49) | **met** |
| A5 Compression (goal) | ≥ 2.8:1, I-frames only | 3.657:1 | **met** |
| A6 Encode speed, 1 thread | ≤ 4.0 ms | **11.00 ms** p50 (MED, S=8) | **not met** — 2.75× over; M3 target |
| A9 Decode speed, 1 thread | ≤ 6.0 ms | **14.57 ms** p50 | **not met** — 2.4× over; M3 target |
| A10 Duplicate frame | 8-byte packet | 8 bytes | **met** |

M2's own gate (A4 + report) is met. Speed was never expected to be met by the scalar reference code;
it is the subject of M3 (SIMD) and M4 (threads).

## Corpus and method

- **Corpus:** new FRAPS recording, Minecraft, 1360×744 `yuvj420p` (full range), 60 fps, 1,597 frames
  (26.6 s), recorded on the development laptop. The original clip in plan §1.3 was deleted
  ([D-015](../DECISIONS.md)). This scene compresses better than the old one (FRAPS 2.83:1 vs 2.49:1)
  and has far fewer duplicates (154 vs 1,436), so A4 is judged against FRAPS on these same frames.
- **Frames** were extracted with `ffmpeg -i clip.avi -an -f rawvideo -pix_fmt yuvj420p` (no conversion).
  All codecs see identical YUV data; Ut Video and FFV1 read it declared as `yuv420p`, so FFmpeg does
  not convert it either.
- **Real frames:** each codec is measured on the frames it stored in full (FRAPS 1,443; RCV1 codes
  156 identical-to-previous frames as 8-byte DUPs, leaving 1,441) — see [D-016](../DECISIONS.md).
  Ratio = raw frame size (1,517,760 bytes) / packet size.
- **Machine:** Intel Core i5-7200U (2C/4T, AVX2), AC power, Windows-reported clock 2,511 MHz
  (Windows does not show turbo here). Bench thread pinned to logical CPU 3 at HIGHEST priority.
  MSVC 19.51 Release (`/O2 /Oi /GL`). One run; expect a few % run-to-run variation.
- **Baselines:** FRAPS sizes are read from its AVI. Ut Video (`-pred median`) and FFV1 (`-level 3`,
  defaults otherwise) were encoded by FFmpeg 9.0.2 with `-threads 1`; their times are FFmpeg's user
  CPU time per frame (approximate: includes small demux/mux costs) and they were not decoded.

## Results

| Codec | Real frames | Real-frame ratio (median) | Worst | Best | Aggregate | Overall (incl. dups) | Avg MB/s @60 | Peak MB/s (1 s) | Encode ms p50 / p99 / max | Decode ms p50 / p99 |
|---|---|---|---|---|---|---|---|---|---|---|
| **RCV1 MED S=8** (default) | 1441 | **3.657** | 3.127 | 6.557 | 3.874 | 4.293 | 21.2 | 29.1 | 11.00 / 15.23 / 25.40 | 14.57 / 17.24 |
| RCV1 MED S=1 | 1441 | 3.607 | 3.092 | 6.626 | 3.829 | 4.244 | 21.5 | 29.4 | 10.70 / 14.28 / 16.76 | 14.58 / 17.27 |
| RCV1 LEFT S=8 | 1441 | 3.359 | 2.894 | 5.896 | 3.541 | 3.925 | 23.2 | 31.4 | 5.89 / 8.81 / 13.95 | 9.29 / 11.45 |
| RCV1 LEFT S=1 | 1441 | 3.294 | 2.859 | 5.913 | 3.484 | 3.861 | 23.6 | 31.8 | 5.45 / 8.33 / 13.09 | 9.21 / 11.20 |
| FRAPS | 1443 | 2.832 | 2.513 | 5.392 | 3.030 | 3.354 | 27.2 | 36.2 | – | – |
| Ut Video (median, 1 thread) | 1441 | 3.621 | 3.098 | 6.585 | 3.841 | 3.821 | 23.8 | 29.4 | ~1.65 (mean) | – |
| FFV1 level 3 (1 thread) | 1441 | 4.416 | 3.885 | 17.227 | 4.955 | 4.911 | 18.5 | 22.9 | ~12.27 (mean) | – |

"Aggregate" = total raw bytes / total bytes over real frames. "Overall" includes duplicate frames as
each codec stores them (FRAPS and RCV1: 8-byte markers; Ut Video and FFV1 code every frame).

### Encoder time by stage (mean ms per coded frame, 1 thread, scalar)

| Config | Load input | Predict + histogram | Table build | Entropy write | Other | Total |
|---|---|---|---|---|---|---|
| RCV1 MED S=8 | 0.22 | **6.65** | 0.67 | **3.06** | 0.00 | 10.60 |
| RCV1 MED S=1 | 0.22 | 6.71 | 0.23 | 3.11 | 0.00 | 10.27 |
| RCV1 LEFT S=8 | 0.23 | 1.94 | 0.76 | 3.14 | 0.00 | 6.07 |
| RCV1 LEFT S=1 | 0.23 | 1.98 | 0.27 | 3.16 | 0.00 | 5.64 |

## Findings

1. **RCV1 beats FRAPS by a wide margin.** Real frames are 22.6% smaller (median ratio +29%), whole
   files 21.9% smaller including duplicates, and the peak write rate drops from 36.2 to 29.1 MB/s at
   60 fps — comfortably within a laptop HDD's sequential speed.
2. **RCV1 ≈ Ut Video on size (+1.0%)**, as expected: both are median prediction + per-slice Huffman.
   Ut Video's hand-optimised encoder does the equivalent work in ~1.65 ms, which shows the A6 target
   of 4.0 ms is realistic for this algorithm.
3. **FFV1 compresses 21% better** (context modelling + adaptive range coding) at ~12.3 ms/frame on one
   thread — too slow for the recorder's CPU budget, but it quantifies the headroom for the v2 ideas
   in plan §14 (rANS / context coding).
4. **Per-slice tables pay for themselves.** S=8 is 1.4% *smaller* than S=1 despite the extra
   restarts, because each slice gets its own Huffman table. Slice-parallel encoding (M4) therefore
   costs no compression.
5. **MED is worth keeping** (+8.9% vs LEFT). Its scalar cost (6.65 vs 1.94 ms) comes from the
   data-dependent branches; a branch-free SIMD version removes almost all of the difference.
6. **Where the time goes:** predict + histogram 63%, entropy write 29%, table build 6%, input copy 2%.
   Header, directory and assembly are negligible.

## Plan for M3 (speed)

| Stage | Now | Approach | Rough target |
|---|---|---|---|
| Predict (MED) | part of 6.65 | AVX2/SSE4.1 branch-free select form (plan §5.4), 32 samples per step | ~0.4 ms |
| Histogram | part of 6.65 | 4 interleaved sub-histograms (plan §5.6.1), fused with the residual pass | ~1.0 ms |
| Entropy write | 3.06 | combined code+length table, fewer flushes (64-bit writes), branch-light loop | ~1.3 ms |
| Table build | 0.67 | avoid re-sorting on every length-limit retry; cheaper node selection | ~0.2 ms |
| Decode | 14.57 | fuse entropy decode + reconstruct, 64-bit refills, then SIMD-friendly LEFT/MED paths | ≤ 6 ms |

Sum of the targets ≈ 3.1–3.5 ms per frame on one thread, i.e. A6 is plausible. Each kernel gets a
scalar-equivalence test before its speed is measured (plan §0 rule 2).

## Limitations

- One scene from one game. Plan §11.2 also asks for a noisier game and a desktop/UI corpus so the
  targets aren't tuned to Minecraft alone.
- Windows-reported CPU clock does not reveal turbo; laptop thermals were not controlled beyond AC power.
- External codec timings are approximate and not decode-verified here (both are standard lossless codecs).

## Reproduce

```
ffmpeg -i "Minecraft 2026-09-28 15-15-15-36.avi" -an -f rawvideo -pix_fmt yuvj420p corpus\minecraft_1360x744.yuv
build.bat release
powershell -ExecutionPolicy Bypass -File codec\bench\compare_baselines.ps1 -Yuv corpus\minecraft_1360x744.yuv ^
    -Size 1360x744 -Fraps "Minecraft 2026-09-28 15-15-15-36.avi" -Report corpus\m2_report.md
```
