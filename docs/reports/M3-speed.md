# RCV1 — M3 speed report (SIMD kernels, fast Huffman)

**Date:** 2026-09-28 · **Codec state:** M3 — CPU dispatch (scalar / SSE4.1 / AVX2), SIMD residual
kernels, fast Huffman writer and decoder; still single-threaded · **Tool:** `rcv_bench`

## Verdict

| Criterion (codec plan §2.3) | Target | M2 (scalar) | **M3** | Status |
|---|---|---|---|---|
| A6 Encode, 1 thread, 1360×744 lossless | ≤ 4.0 ms | 11.00 ms | **3.18 ms** p50 (AVX2), 3.59 ms (SSE4.1) | **met** |
| A9 Decode, 1 thread, 1360×744 lossless | ≤ 6.0 ms | 14.57 ms | **5.26 ms** p50 | **met** |
| A3 Determinism across ISAs | identical bytes | – | scalar, SSE4.1 and AVX2 packets identical (tests + all 1,597 corpus frames) | **met** (ISA part; threads come in M4) |
| A1 Lossless round trip | bit-exact | met | all corpus frames bit-exact at every ISA level | **met** |
| M3 gate: SIMD equivalence tests | pass | – | 33 tests pass with AVX2, forced SSE4.1, forced scalar, and under clang-cl | **met** |

Compression is unchanged by construction (identical output): median **3.657:1** vs FRAPS 2.832:1.

## Results (full corpus, 1,597 frames, MED and LEFT, 8 slices)

Same corpus, machine and method as the [M2 report](M2-compression.md): i5-7200U on AC power, bench
thread pinned to one logical CPU, MSVC 19.51 Release, single run.

| Config | Encode ms p50 / p99 / max | Decode ms p50 / p99 |
|---|---|---|
| RCV1 MED, scalar | 9.17 / 13.14 / 16.66 | 5.28 / 8.87 |
| RCV1 MED, SSE4.1 | 3.59 / 6.10 / 7.66 | 5.24 / 8.81 |
| **RCV1 MED, AVX2** (default on this CPU) | **3.18** / 5.37 / 7.83 | **5.26** / 8.78 |
| RCV1 LEFT, scalar | 4.32 / 6.93 / 9.05 | 3.64 / 6.00 |
| RCV1 LEFT, SSE4.1 | 3.42 / 5.96 / 9.52 | 3.65 / 6.01 |
| RCV1 LEFT, AVX2 | 3.10 / 5.41 / 6.92 | 3.63 / 5.98 |
| *Ut Video (median), FFmpeg, 1 thread* | *~1.65 (mean)* | – |
| *FFV1 level 3, FFmpeg, 1 thread* | *~12.27 (mean)* | – |

Decoding has no ISA-specific kernels (it is serial per row), so its time is the same at every level.

### Encoder time by stage (mean ms per coded frame)

| Config | Load | Predict + histogram | Table build | Entropy write | Total |
|---|---|---|---|---|---|
| M2: MED scalar | 0.22 | 6.65 | 0.67 | 3.06 | 10.60 |
| M3: MED scalar | 0.20 | 6.31 | 0.29 | 2.06 | 8.87 |
| M3: MED SSE4.1 | 0.22 | 1.23 | 0.29 | 2.06 | 3.80 |
| **M3: MED AVX2** | 0.22 | **1.13** | **0.28** | **1.75** | **3.38** |

## What changed

**Encoder**
1. **SIMD residual kernels** (`predict_sse41.cpp`, `predict_avx2.cpp`): the MED select form of plan §5.4
   on 16/32 samples at a time — `min/max_epu8`, two unsigned compares, two `blendv`. Exactly equal to
   the scalar reference (tested on every width 1–130, random strides, slice starts, and a small value
   alphabet that forces MED's tie cases).
2. **Histogram**: four interleaved sub-histograms fed 8 bytes at a time (§5.6 step 1), shared by all levels.
3. **Huffman writer**: codes stored left-aligned in 64-bit table entries so each symbol is one
   shift, one OR and one add; 4 symbols per 8-byte big-endian store. A second copy is compiled with
   `/arch:AVX2` so the variable shifts become BMI2 `shrx` (1.75 vs 2.06 ms). Byte-identical to the
   reference `BitWriter`.
4. **Table build**: sort once by a packed (count, symbol) key; the length-limit retries restore order
   with an insertion sort (0.67 → 0.28 ms for 24 tables). Same lengths as before.

**Decoder**
1. **Fused single pass**: Huffman decoding and reconstruction in one loop, straight into the reference
   frame (no symbol buffer).
2. **Two slices in lockstep**: neighbouring HUFFMAN chunks of a plane are decoded together, giving the
   CPU two independent dependency chains to overlap on one core (the approach of multi-stream Huffman
   decoders; RCV1's independent slices provide the streams for free).
3. **Cheaper symbol step**: 8-byte refills; split `sym`/`len` lookup tables (one byte load each);
   branch-free MED as `clamp(a + b − c, min(a,b), max(a,b))` (proven equal to MED for all 2²⁴ inputs);
   and the end-of-chunk check moved out of the per-symbol path — past the end the reader supplies
   zero bits and the bit count goes negative, which is checked once per chunk. Invalid streams are
   still rejected and nothing is read past the chunk.
4. **Aliasing fix**: pixel stores go through `uint8_t*`, which may alias anything, so decoder state
   reached by reference was reloaded from memory on every symbol; the loops now work on local copies.

## Observations

- The remaining encode time is ~33% prediction + histogram (mostly the histogram), ~52% entropy
  writing and ~8% table building. Ut Video's ~1.65 ms shows there is still headroom, but A6 is met,
  and M4's two worker threads will roughly halve the wall-clock time again.
- Encode p99 (5.4 ms) and max (7.8 ms) are well inside the 16.7 ms frame budget at 60 fps.
- Decoding is serial per slice; M4 will decode slices on several threads.

## Limitations

- One scene, one machine, one run; laptop turbo/thermal state moves results by a few percent
  (repeat runs of a 400-frame sample varied by ~5%).
- No CPU without AVX2 was available, so the SSE4.1 and scalar paths were exercised by forcing them
  (`RCV_FORCE_ISA`, `--isa`) on this CPU, not on older hardware.

## Reproduce

```
build.bat release
build\release\codec\rcv_bench.exe -i corpus\minecraft_1360x744.yuv -s 1360x744 --predictor both --isa all
set RCV_FORCE_ISA=scalar & build\release\codec\rcv_tests.exe
```
