# RCV1 — M4 report (thread pool, slice-parallel encode/decode)

**Date:** 2026-09-28 · **Codec state:** M4 — persistent thread pool, one encoder job per
(plane, slice) chunk, paired-slice decoder jobs; AVX2 kernels · **Tool:** `rcv_bench --threads`

## Verdict

| Criterion (codec plan §2.3) | Target | Measured (MED, 8 slices, AVX2) | Status |
|---|---|---|---|
| A6 Encode, **2 threads**, 1360×744 lossless | ≤ 2.5 ms | **1.60 ms** p50 (Minecraft); 1.69 ms at 1280×720 (Warframe) | **met** |
| A6 Encode, 1 thread | ≤ 4.0 ms | 3.06 ms p50 | met (unchanged from M3) |
| A9 Decode, 1 thread | ≤ 6.0 ms | 5.50 ms p50; **2.87 ms** with 2 threads | met |
| A3 Determinism | identical bytes for threads 1/2/4 and every ISA | identical in tests (1–4 threads × scalar/SSE4.1/AVX2, 600-frame stress) and on both corpora | **met** |
| A1 Lossless round trip | bit-exact | every frame of both corpora at 1, 2 and 4 threads | met |
| M4 gate | A3 + A6 two-thread measured | as above | **met** |

## Results

Machine and method as in the [M2](M2-compression.md) and [M3](M3-speed.md) reports (i5-7200U, 2 cores /
4 threads, AC power, single run). The bench thread is pinned to logical CPU 3; encoder workers are
pinned to other physical cores first (CPU 1, then the hyper-thread siblings 2 and 0).

| Corpus | Threads | Encode ms p50 / p99 / max | Decode ms p50 / p99 | Encode speed-up |
|---|---|---|---|---|
| Minecraft 1360×744, 1,441 real frames | 1 | 3.06 / 5.15 / 8.26 | 5.50 / 9.93 | — |
| | **2** | **1.60** / 3.06 / 3.76 | **2.87** / 5.67 | 1.91× |
| | 4 | 1.50 / 3.26 / 5.58 | 2.86 / 4.69 | 2.04× |
| Warframe 1280×720, 444 real frames | 1 | 3.15 / 5.00 / 7.31 | 5.55 / 7.80 | — |
| | **2** | **1.69** / 3.17 / 3.43 | **2.93** / 5.46 | 1.86× |
| | 4 | 1.56 / 3.38 / 4.00 | 2.85 / 4.64 | 2.02× |

Compression is identical at every thread count (Minecraft 3.657:1, Warframe 1.708:1 real-frame median).

- **Two threads scale almost perfectly** (1.9×): 24 chunk jobs per frame, big luma chunks first, and
  the fixed-order copy into the packet costs ~0.05 ms.
- **Four threads add little** on this CPU: the extra two are hyper-threads of the same two cores.
  This matches the recorder plan's budget of 2 codec threads on a 2C/4T laptop, leaving the other two
  logical CPUs to the game.
- At 60 fps the frame budget is 16.7 ms; two-thread encoding uses under 10% of it (p99 about 3 ms).

## Second corpus: Warframe (noisy content, plan §11.2)

A FRAPS recording of Warframe (1280×720, 60 fps, 791 frames: 446 real, 345 duplicate) now joins the
Minecraft corpus, so the numbers aren't tuned to one game. Full comparison, M3/M4 build, 1 thread:

| Codec | Real frames | Real-frame ratio (median) | Worst | Best | Overall | Peak MB/s | Encode ms |
|---|---|---|---|---|---|---|---|
| **RCV1 MED S=8** | 444 | **1.708** | 1.569 | 2.331 | 3.079 | 30.9 | 3.25 (p50) |
| RCV1 LEFT S=8 | 444 | 1.668 | 1.511 | 2.283 | 3.007 | 31.7 | 2.93 (p50) |
| FRAPS | 446 | 1.580 | 1.468 | 2.124 | 2.838 | 33.5 | – |
| Ut Video (median) | 444 | 1.684 | 1.561 | 2.319 | 1.702 | 52.2 | ~1.40 (mean) |
| FFV1 level 3 | 444 | 1.903 | 1.677 | 2.676 | 1.917 | 47.9 | ~13.43 (mean) |

On detailed, noisy content every lossless codec struggles (FRAPS 1.58:1 vs 2.83:1 on Minecraft);
RCV1 still beats FRAPS by 8% (vs 29% on Minecraft) and stays just ahead of Ut Video. The game ran at
about 34 fps, so 44% of the frames are duplicates — which is where M5's skip/DUP work pays off.

## What was built

- **`ThreadPool`** (`src/threadpool.{h,cpp}`): `num_threads − 1` persistent workers plus the calling
  thread; ~2 µs spin, then C++20 `atomic::wait`; `on_worker_start` hook for priority/affinity; no
  allocation per dispatch. See [D-024](../DECISIONS.md).
- **Encoder**: each job copies its own input rows, predicts and entropy-codes into a private buffer;
  the caller assembles directory + chunks in fixed order ([D-025](../DECISIONS.md)).
- **Decoder**: paired-slice jobs with per-worker decode tables ([D-026](../DECISIONS.md)).
- **Tests** (38): pool exactly-once over 20,000 alternating tiny/large dispatches per thread count
  with yielding jobs; worker-start callback; identical packets for 1–4 threads × every ISA;
  multi-threaded decode; 600-frame 4-thread stress. `RCV_FORCE_THREADS` re-runs the suite at any count.

## A bug worth recording

The first pool version deadlocked — rarely, and only when the test ran at lower priority in the
background. A worker late for one dispatch could combine that dispatch's final index with the *next*
dispatch's job count (kept in a separate atomic) and claim a job that didn't exist, corrupting the
completion counter. The fix packs generation, job count and index into the one word that is
compare-exchanged, so a claim is always consistent with exactly one dispatch. Afterwards: 50 runs at
below-normal priority with a 30 s watchdog (≈3 million dispatches), 0 hangs, 0 failures; ctest now
has a timeout so a deadlock fails instead of hanging.

## Limitations

- One machine, one run per configuration; ±5% run-to-run variation is typical on this laptop.
- Decoder workers aren't pinned (the decoder API has no worker-start hook; it's only used for
  playback/conversion).
- No thread-sanitizer is available for MSVC/clang-cl on Windows; concurrency confidence comes from the
  design argument (D-024), the stress tests, and the repeated low-priority runs.

## Reproduce

```
build.bat release
build\release\codec\rcv_bench.exe -i corpus\minecraft_1360x744.yuv -s 1360x744 --threads 1,2,4
build\release\codec\rcv_bench.exe -i corpus\warframe_1280x720.yuv -s 1280x720 --threads 1,2,4
powershell -ExecutionPolicy Bypass -File codec\bench\compare_baselines.ps1 -Yuv corpus\warframe_1280x720.yuv ^
    -Size 1280x720 -Fraps "Warframe 2026-09-28 02-10-26-14.avi"
```
