# RCV1 — Real-time Lossless Screen-Capture Codec
## Build Plan & Specification (v1.0, for AI-assisted implementation)

> **Working name:** RCV1 ("Recorder Codec Video 1"). The name and FourCC are placeholders and can be renamed freely before first release.
>
> **Scope of this document:** the codec only — an encoder/decoder library, its bitstream format, its test tooling, and its performance/compression acceptance criteria. Frame capture (API hooking, GPU copy, readback), the rate controller, the container/disk writer, audio, and the hardware-encoder fallback are covered in the separate **Implementation Plan** and are listed here only as interfaces.

---

## 0. Instructions for the AI builder

Read this whole document before writing code. Then follow these rules:

1. **This document is the source of truth for the bitstream.** Do not change any byte layout, bit order, predictor definition or Huffman rule without updating this document in the same change. Encoder and decoder must match it bit for bit.
2. **Scalar reference first.** Every kernel (prediction, histogram, skip compare, Huffman encode/decode, colour transforms) gets a plain, obviously-correct scalar implementation first. SIMD versions come later and are tested against the scalar version on random data.
3. **No heap allocation inside `rcv_encode_frame`, `rcv_encode_duplicate` or `rcv_decode_frame`.** Allocate everything in `*_create`. This is a hard requirement, verified by a test (§11.4).
4. **The core library never throws, never logs to console, never calls the OS for anything except threads and synchronisation.** Errors are returned as `rcv_status` codes.
5. **Output must be deterministic:** identical input and settings must produce identical bytes regardless of thread count or CPU (scalar vs SSE vs AVX2 path).
6. **Build in the milestone order in §13.** Each milestone has acceptance tests. Do not start the next milestone until the current one passes.
7. **Performance numbers in this document are targets, not measurements.** Report actual measured numbers; never claim a target is met without a benchmark run showing it.
8. When something in this spec is ambiguous, stop and ask rather than guessing, and record the decision in `docs/DECISIONS.md`.

---

## 1. Background and baseline

### 1.1 What we are replacing
The target behaviour is that of legacy in-game recorders (FRAPS, Dxtory): lossless or visually lossless capture with negligible impact on the recorded application, on modest hardware. Earlier attempts (raw frame dumps, growing file, CPU buffer) failed because they wrote 220–500 MB/s of raw RGBA and blocked the render thread. The fix is to **reduce the data before it goes anywhere, cheaply, off the render thread**. RCV1 is that data-reduction step.

### 1.2 Development hardware (minimum target)
- Intel Core i5-7200U (Kaby Lake, 2 cores / 4 threads, AVX2 + BMI2 + SSE4.2), Intel HD Graphics 620 (shared memory), SSD present but **the design must not depend on an SSD**.
- Must also run correctly (with scalar/SSE fallback) on older x64 CPUs without AVX2.

### 1.3 Measured FRAPS baseline (from the user's own test clip)
Minecraft, 1360×744, `yuvj420p` (YUV 4:2:0, full range), fixed 60 fps, 2,129 frames, 344.8 MB:

| Metric | Measured value |
|---|---|
| Real (non-duplicate) frames | 693 (33%) |
| Duplicate frames (8-byte markers) | 1,436 (67%) |
| Real-frame compressed size | 291,360 – 639,544 bytes; median 609,520 bytes |
| Raw frame size (1360×744×1.5) | 1,517,760 bytes |
| **Real-frame compression ratio** | **median 2.49:1**, worst 2.37:1, best 5.21:1 |
| Overall ratio incl. duplicates | 9.37:1 |
| Peak observed write rate (near-60 unique fps) | ≈37 MB/s |

**Conclusion used throughout this spec:** FRAPS's advantage comes from (a) a cheap intra-frame lossless codec at ~2.5:1, (b) YUV 4:2:0, and (c) near-free duplicate frames. RCV1 must match (a)–(c) and improve on them with better prediction, per-slice entropy tables, and temporal block skipping.

---

## 2. Goals, non-goals, acceptance targets

### 2.1 Goals
1. Lossless encoding of 8-bit YUV 4:2:0 (primary) and 8-bit RGB (secondary).
2. Optional **near-lossless** mode (per-sample error bounded to ±1, ±2 or ±3) that the rate controller can switch on **per frame**.
3. **Temporal block skip** (P-frames) and **duplicate frames** (DUP) for static content and stalled games.
4. Multi-threaded via independent slices; deterministic output.
5. Very low CPU cost: a FRAPS-class machine must be able to encode 720p–1360×744 at 60 fps using ≤ 2 worker threads with headroom for the game.
6. A decoder and CLI tool so recordings can be verified and converted to standard formats with FFmpeg.

### 2.2 Non-goals (v1)
- Motion compensation, DCT/transform coding, lossy rate-distortion optimisation.
- 10-bit / HDR, alpha channel, 4:2:2 / 4:4:4 YUV.
- Audio, container muxing, file writing (Implementation Plan).
- GPU-side entropy coding (the GPU is reserved for the game; it only does colour conversion/scaling — Implementation Plan).
- An FFmpeg/VfW/DirectShow decoder plugin (possible v2; v1 converts via CLI).

### 2.3 Acceptance targets
Figures marked **Target** are goals to validate on the i5-7200U; they are not measured.

| # | Criterion | Requirement |
|---|---|---|
| A1 | Lossless round-trip | Bit-exact (MD5 per plane) for every test vector, every mode, every thread count |
| A2 | Near-lossless bound | max \|decoded − source\| ≤ NEAR for every sample, verified over the full corpus |
| A3 | Determinism | Identical packet bytes for threads = 1, 2, 4 and for scalar / SSE4.1 / AVX2 paths |
| A4 | Compression (must) | Real-frame median ratio on the Minecraft corpus **≥ 2.49:1** (FRAPS parity) |
| A5 | Compression (goal) | **Target:** ≥ 2.8:1 on the same corpus, I-frames only |
| A6 | Encode speed | **Target:** 1360×744 lossless I-frame ≤ 4.0 ms on 1 thread; ≤ 2.5 ms on 2 threads |
| A7 | Near-lossless speed | **Target:** 1360×744 ≤ 8.0 ms on 1 thread |
| A8 | Skip compare | **Target:** ≤ 0.3 ms per 1360×744 frame |
| A9 | Decode speed | **Target:** 1360×744 lossless ≤ 6.0 ms on 1 thread (playback/conversion) |
| A10 | Duplicate frame | 8-byte packet, < 1 µs, no input frame required |
| A11 | No allocation | Zero heap allocations in encode/decode calls (instrumented test) |
| A12 | Robustness | Decoder survives ≥ 1 hour of libFuzzer + ASan with no crash, hang or out-of-bounds access |
| A13 | Bounded output | Every packet ≤ `rcv_max_packet_size(config)` |

---

## 3. Recommended language, toolchain and libraries

### 3.1 Recommendation: **C++20, no runtime dependencies in the codec core**

| Option | Verdict | Reason |
|---|---|---|
| **C++20 (MSVC 2022 or clang-cl)** | **Recommended** | Same language as the capture hook (in-process DLL injected into D3D9/D3D11/OpenGL/Vulkan apps); direct SIMD intrinsics; full control of memory and threads; no garbage collector; largest body of reference code (FFmpeg, zstd, Ut Video) to learn from; best-supported by AI code generation for this domain. You already used it. |
| Rust | Viable alternative | Memory-safe parsing is attractive for the decoder and `std::arch` gives SIMD, but the hook/COM/D3D side is less mature and mixing Rust codec + C++ hook adds FFI friction. Consider only if you want the whole project in Rust. |
| C | Viable | Works, but C++20 gives `std::atomic::wait`, `std::span`, templates for SIMD dispatch, and RAII for setup code with no runtime cost. |
| C#, Java, Go, Python | **Do not use** | Garbage-collector pauses and JIT warm-up cause exactly the frame-time jitter we are eliminating, and they cannot live inside the game process cleanly. |

### 3.2 Toolchain
- **Compiler:** MSVC 2022 (v143) as primary, clang-cl as secondary (needed for libFuzzer). Code must compile warning-free on both at `/W4`.
- **Build:** CMake ≥ 3.25 + Ninja. Targets: `rcv` (static lib), `rcv_shared` (DLL with C API), `rcv_cli`, `rcv_tests`, `rcv_bench`, `rcv_fuzz` (clang-cl only).
- **Flags (Release):** `/O2 /Oi /GL /DNDEBUG`, `/arch:AVX2` **only** on the translation units that contain AVX2 kernels; everything else compiles for the x64 baseline (SSE2). Link with `/LTCG`.
- **Architecture:** x64 only.

### 3.3 SIMD strategy
- Three kernel levels: **scalar** (reference, always present), **SSE4.1**, **AVX2**.
- Pick the level once at `rcv_encoder_create` via CPUID (`__cpuid`/`__cpuidex` plus an XGETBV check that the OS saves AVX state). Store function pointers in the encoder object. An override (`RCV_FORCE_ISA=scalar|sse41|avx2` environment variable, read only by tests/CLI and passed in via config) forces a path for testing.
- Each ISA lives in its own `.cpp` so compile flags stay per-file.

### 3.4 Libraries
- **Codec core:** C++ standard library only.
- **Tests:** doctest (single header) or GoogleTest.
- **Benchmarks:** own harness using `QueryPerformanceCounter` (or `std::chrono::steady_clock`); Google Benchmark optional.
- **Fuzzing:** libFuzzer + AddressSanitizer via clang-cl.
- **FFmpeg CLI** (not linked): used only to prepare test corpora and to compare against Ut Video / FFV1.

---

## 4. Data model

### 4.1 Formats
| `rcv_format` | Planes | Plane sizes (coded W×H) | Notes |
|---|---|---|---|
| `RCV_FMT_YUV420` (0) | Y, Cb, Cr | W×H, W/2×H/2, W/2×H/2 | Default. W and H must be even. |
| `RCV_FMT_GBR` (1) | G, B−G, R−G | W×H each | Lossless RGB. Near-lossless not allowed in v1. |

### 4.2 Accepted input layouts (encoder)
| `rcv_input_layout` | Valid for | Description |
|---|---|---|
| `RCV_IN_I420` | YUV420 | Three separate planes, each with its own stride |
| `RCV_IN_NV12` | YUV420 | Y plane + interleaved CbCr plane (common GPU readback format); de-interleaved during the prediction pass |
| `RCV_IN_BGRA` / `RCV_IN_BGRX` | GBR | 8-bit per channel, alpha ignored; transformed to G, B−G, R−G during the prediction pass |

Strides may be any value ≥ row width; negative strides are not supported.

### 4.3 Colour metadata (stored, not applied by the codec)
- Matrix: 0 = BT.601 (default, matches FRAPS), 1 = BT.709.
- Range: 1 = full (0–255, default, matches FRAPS `yuvj420p`), 0 = limited.
- Chroma siting: 0 = centre (2×2 average).
- For reference, the capture shader (Implementation Plan) uses BT.601 full range:
  `Y = 0.299R + 0.587G + 0.114B`, `Cb = 128 − 0.168736R − 0.331264G + 0.5B`, `Cr = 128 + 0.5R − 0.418688G − 0.081312B`, rounded and clamped to [0,255]; chroma = average of the 2×2 block.

### 4.4 RGB reversible transform (GBR format)
Per pixel, with 8-bit wrap-around arithmetic (mod 256):
```
P0 = G
P1 = (B − G) & 0xFF
P2 = (R − G) & 0xFF
Inverse: G = P0; B = (P1 + G) & 0xFF; R = (P2 + G) & 0xFF
```
This is exactly lossless and keeps all planes 8-bit.

### 4.5 Dimensions and cropping
- Coded width/height: 2 … 8192. For YUV420 both must be even.
- The capture side pads odd sizes to even by replicating the last column/row; the true display size is carried in the sequence header (§6.1) for cropping on playback.

### 4.6 Blocks and slices
- **Block** (unit for temporal skip): 16×16 luma samples (8×8 in each 4:2:0 chroma plane; 16×16 in every GBR plane). Edge blocks may be partial. `blocks_x = ceil(W/16)`, `blocks_y = ceil(H/16)`.
- **Slice:** a horizontal band made of whole block rows. With `S` slices, slice `s` covers block rows `[floor(s·blocks_y/S), floor((s+1)·blocks_y/S))`. In luma rows that is 16× those numbers, clipped to H; in 4:2:0 chroma rows it is 8×, clipped to H/2.
- `S` range 1–64 and `S ≤ blocks_y`. Default (`num_slices = 0`): `S = min(8, blocks_y)`.
- **Slices are fully independent:** prediction never reads samples from another slice, so slices can be encoded and decoded in parallel.
- Example: 1360×744 → 85 × 47 = 3,995 blocks; 8 slices of 5–6 block rows.

---

## 5. Coding algorithms

### 5.1 Pipeline per frame (encoder)
```
Phase A (parallel, per slice)   : temporal skip compare → slice skip bitmap + counts
Caller                          : choose frame type (DUP / I / P)
Phase B (parallel, per job)     : for each (plane, slice) job:
                                    predict → residual symbols + histogram
                                    build length-limited Huffman table
                                    encode bitstream (or single-symbol / raw)
                                    update reference buffer for this slice
Caller                          : assemble header + skip map + directory + chunks
```

### 5.2 Frame types
| Type | Code | Payload | Decoder action |
|---|---|---|---|
| DUP | 0 | none (8-byte packet) | Output = previous decoded frame |
| I | 1 | all samples coded | Independent; seek point (keyframe) |
| P | 2 | skip map + non-skipped samples coded | Skipped blocks copied from previous decoded frame |

**Frame-type decision (encoder, in this order):**
1. `force_keyframe` set, or `frames_since_I ≥ keyframe_interval`, or no reference yet → **I** (Phase A is skipped entirely).
2. Temporal skip disabled → **I**.
3. All blocks skipped → **DUP**.
4. No blocks skipped → **I** (same cost, and it creates a free seek point; reset `frames_since_I`).
5. Otherwise → **P**.

`frames_since_I` counts every emitted packet, including DUP. Default `keyframe_interval` = 120.

`rcv_encode_duplicate()` emits a DUP packet without an input frame. The capture layer calls it when the game has not presented a new frame by the capture tick, which saves the GPU copy and readback entirely.

### 5.3 Temporal skip compare (Phase A)
- For each block, compare the current source block with the same block in the encoder's **reference buffer** (the previously *reconstructed* frame), across all three planes.
- Lossless (NEAR = 0): a block is skipped if every sample is equal.
- Near-lossless (NEAR = n): skipped if every |source − reference| ≤ n (this keeps the error bound).
- Output per slice: bitmap bits (1 = skipped) and a skipped-block count.
- SIMD: 16/32-byte compares (`_mm_cmpeq_epi8` + `movemask`, or `_mm256_*`); for NEAR > 0 use `max(a−b, b−a)` via saturating subtracts and compare against n. Exit a block early on the first mismatching row.

### 5.4 Prediction (per plane, per slice)
Let `x` be the current sample at (col i, row j) within a slice-plane, `a` = left, `b` = above, `c` = above-left, where all neighbours are **current-frame values** (for P-frames skipped samples already hold their copied values; for near-lossless they hold reconstructed values).

Neighbour rules — must be implemented exactly:
| Position | Prediction |
|---|---|
| First row of slice, i = 0 | 128 |
| First row of slice, i > 0 | a (left) |
| Other rows, i = 0 | b (above) |
| Other rows, i > 0 | predictor below |

**Predictor 1 — MED (median edge detector, LOCO-I / JPEG-LS), default:**
```
mn = min(a, b); mx = max(a, b)
if      c >= mx: pred = mn
else if c <= mn: pred = mx
else:            pred = a + b − c        // guaranteed in [mn, mx], fits 8 bits
```
**Predictor 0 — LEFT** (fallback / comparison): `pred = a` for i > 0, same edge rules.

> ⚠ **SIMD pitfall:** do *not* compute MED as `median(a, b, (a+b−c) mod 256)` with 8-bit wrapping arithmetic — it gives wrong results when `c` is outside `[mn, mx]`. Use the explicit select form above, with the gradient computed in wrapping 8-bit arithmetic only for the third branch (where it cannot overflow). Unsigned 8-bit compares: `c >= mx` ⇔ `max_epu8(c, mx) == c`.

**Residual symbol (lossless):** `sym = (x − pred) & 0xFF`. No zig-zag mapping is needed because the Huffman table adapts per chunk.

**Encoder vectorisation:** in lossless mode, all neighbours are known source values, so a whole row's predictions can be computed with SIMD (load row j, row j−1, and the same rows shifted by one sample). The decoder is inherently serial along a row (it needs the reconstructed left neighbour) and runs scalar.

**Skipped samples:** in a P-frame, residuals are emitted only for samples in non-skipped blocks, in raster order within the slice-plane. Skipped samples are never coded but are still used as neighbours.

### 5.5 Near-lossless quantisation (YUV420 only, NEAR = n ∈ {1,2,3})
Scalar, serial per row (the encoder must predict from reconstructed values):
```
e    = x − pred                                   // signed int
q    = sign(e) * ((abs(e) + n) / (2n + 1))        // integer division
rec  = clamp(pred + q * (2n + 1), 0, 255)
sym  = q & 0xFF
store rec into the working buffer (it becomes the neighbour for later samples)
```
Guarantee: |rec − x| ≤ n. Decoder: `q = (int8_t)sym; rec = clamp(pred + q*(2n+1), 0, 255)`.
Near-lossless applies to all three YUV planes. NEAR is chosen per frame by the caller (the rate controller) and written in the frame header.

### 5.6 Entropy coding — per-chunk canonical Huffman
One **chunk** = one (plane, slice) pair. Each chunk has its own table.

1. **Histogram:** 256 bins over the chunk's residual symbols. Use 4 interleaved sub-histograms merged at the end (avoids store-to-load stalls).
2. **Mode selection:**
   - 0 coded samples (everything skipped) → mode 3 `EMPTY`.
   - Exactly one distinct symbol → mode 1 `SINGLE` (store the symbol; no bitstream).
   - Otherwise build Huffman (step 3). If the complete HUFFMAN chunk (4-byte chunk header + 128-byte table + padded bitstream) would be larger than the RAW chunk (4 + align4(sample count)), use mode 2 `RAW` (store the symbols as bytes). The bitstream size is known exactly from `Σ hist[s] × len[s]` before encoding, so this decision costs nothing. This bounds worst-case size.
3. **Code lengths:** standard Huffman construction over non-zero bins, **max code length 12**. If the longest code exceeds 12: replace every non-zero count f with `(f + 1) >> 1` (keeping it ≥ 1) and rebuild; repeat until it fits. (Package-merge is an allowed alternative if the output is identical in format; the choice must be deterministic.)
4. **Canonical codes:** sort symbols by (length, symbol value); `code = 0`; for each in order: `code <<= (len − prev_len)`, assign, `code++`.
5. **Bit writer:** 64-bit accumulator, **MSB-first**; bytes emitted in stream order (most significant bits first). At the end, pad with zero bits to a byte boundary, then zero bytes to a 4-byte boundary.
6. **Decoder:** a 4096-entry lookup table (12-bit peek) of `(symbol, length)`. For a symbol with length L and code C, fill entries `C << (12 − L)` through `((C + 1) << (12 − L)) − 1`. Refill from a 64-bit buffer; use a bounds-checked slow path for the last 8 bytes of a chunk.

**Speed notes for the encoder:** process one chunk at a time so the residual buffer (≤ ~130 KB for a 1360-wide, 96-row slice) stays in L2 cache; compute residuals and histogram in the same pass; the encode loop is `acc = (acc << len) | code; bits += len; if (bits >= 32) flush 32 bits`.

### 5.7 Reference buffer management
- Encoder and decoder each keep one **reference frame** (planar, 64-byte aligned strides).
- **Encoder, in-place update (recommended):** in Phase B each job works directly on its slice of the reference buffer:
  - Lossless: copy the source samples of non-skipped blocks into the reference, then predict (source == reconstruction).
  - Near-lossless: write reconstructed samples into the reference as they are produced; skipped blocks already hold the right values.
  - This is safe because Phase A has already finished reading the reference, and slices never read each other's rows.
- DUP leaves the reference unchanged. I-frames overwrite it completely.
- Decoder: decode into the reference in place (P: skipped blocks are already there; I: everything is overwritten). Output = a view of, or copy from, the reference.

---

## 6. Bitstream specification

All multi-byte integers are **little-endian**. Bit order inside entropy-coded data is MSB-first (§5.6).

### 6.1 Sequence header (container "extradata", 32 bytes)
| Offset | Size | Field |
|---|---|---|
| 0 | 4 | Magic `RCVS` (0x52 0x43 0x56 0x53) |
| 4 | 1 | Version = 1 |
| 5 | 1 | Format (0 YUV420, 1 GBR) |
| 6 | 2 | Coded width |
| 8 | 2 | Coded height |
| 10 | 2 | Display width (≤ coded width) |
| 12 | 2 | Display height (≤ coded height) |
| 14 | 1 | Colour: bits 0–3 matrix, bit 4 range (1 = full), bits 5–6 chroma siting |
| 15 | 1 | Reserved (0) |
| 16 | 4 | Frame rate numerator |
| 20 | 4 | Frame rate denominator |
| 24 | 2 | Keyframe interval |
| 26 | 6 | Reserved (0) |

### 6.2 Frame packet header
**Common 8 bytes (all frame types):**
| Offset | Size | Field |
|---|---|---|
| 0 | 4 | Magic `RCV1` (0x52 0x43 0x56 0x31) |
| 4 | 1 | Version = 1 |
| 5 | 1 | Frame type (0 DUP, 1 I, 2 P) |
| 6 | 2 | Flags: bit 0 = CRC present, bit 1 = near-lossless; others 0 |

**A DUP packet ends here (8 bytes total), the same size as FRAPS's duplicate marker.**

**I and P packets continue (header total 32 bytes):**
| Offset | Size | Field |
|---|---|---|
| 8 | 1 | Format (0 YUV420, 1 GBR) |
| 9 | 1 | NEAR (0–3; must be 0 for GBR) |
| 10 | 1 | Predictor (0 LEFT, 1 MED) |
| 11 | 1 | Slice count S (1–64) |
| 12 | 2 | Coded width |
| 14 | 2 | Coded height |
| 16 | 1 | Block size log2 (= 4 in v1) |
| 17 | 1 | Colour (same encoding as §6.1 offset 14) |
| 18 | 2 | Reserved (0) |
| 20 | 4 | Frame number (informational; wraps) |
| 24 | 4 | Payload size in bytes (everything after this 32-byte header) |
| 28 | 4 | CRC-32C of the payload (0 if flag bit 0 is clear) |

CRC-32C (Castagnoli) is chosen because SSE4.2 has a hardware instruction for it (`_mm_crc32_u64`). It is off by default and enabled for debugging.

### 6.3 Payload layout
```
[ skip map ]        P-frames only: ceil(blocks_x × blocks_y / 8) bytes, zero-padded to a multiple of 4.
                    Blocks in raster order (row-major); bit k of byte m is block 8m + k (LSB first). 1 = skipped.
[ chunk directory ] 3 × S little-endian u32: size in bytes of each chunk, ordered
                    plane 0 slices 0..S−1, plane 1 slices 0..S−1, plane 2 slices 0..S−1.
                    Every size is a multiple of 4. Chunk offsets are the prefix sums.
[ chunks ]          In the same order as the directory.
```

### 6.4 Chunk layout
| Offset | Size | Field |
|---|---|---|
| 0 | 1 | Mode: 0 HUFFMAN, 1 SINGLE, 2 RAW, 3 EMPTY |
| 1 | 1 | SINGLE: the symbol; otherwise 0 |
| 2 | 2 | Reserved (0) |
| 4 | … | HUFFMAN: 128 bytes of code lengths (byte k: low nibble = length of symbol 2k, high nibble = length of symbol 2k+1; 0 = unused), then the bitstream, zero-padded to a multiple of 4 bytes |
| | | RAW: one byte per coded sample, zero-padded to a multiple of 4 |
| | | SINGLE / EMPTY: nothing |

The number of coded samples in a chunk is **not stored**; both sides derive it from the plane dimensions, the slice bounds and the skip map.

### 6.5 Decoder validation rules
Reject (return `RCV_ERR_BITSTREAM`, never crash) when: wrong magic/version; unknown frame type/format/mode; width/height out of range or odd for YUV420; S = 0, S > 64 or S > blocks_y; NEAR > 3 or NEAR ≠ 0 with GBR; payload size doesn't match the packet length; directory sizes overflow the payload or aren't multiples of 4; a code-length table violates Kraft's inequality or has lengths > 12; the bitstream ends before all samples are decoded; a P-frame or DUP arrives with no reference (`RCV_ERR_NO_REFERENCE`); frame dimensions/format differ from the reference for a P-frame.

### 6.6 Worst-case packet size
```
max = 32
    + align4(ceil(blocks_x × blocks_y / 8))
    + 3 × S × 4
    + Σ over chunks ( 4 + align4(samples_in_chunk) )      // RAW fallback bound
```
`rcv_max_packet_size()` returns exactly this. Because RAW is chosen whenever Huffman would be larger, no packet exceeds it.

---

## 7. Public C API (`include/rcv/rcv.h`)

A C ABI so the codec can be used from the C++ capture DLL, a Rust tool, or an FFmpeg plugin later.

```c
typedef enum { RCV_OK = 0, RCV_ERR_INVALID_ARG, RCV_ERR_UNSUPPORTED,
               RCV_ERR_BUFFER_TOO_SMALL, RCV_ERR_BITSTREAM,
               RCV_ERR_NO_REFERENCE, RCV_ERR_OUT_OF_MEMORY } rcv_status;

typedef enum { RCV_FMT_YUV420 = 0, RCV_FMT_GBR = 1 } rcv_format;
typedef enum { RCV_IN_I420, RCV_IN_NV12, RCV_IN_BGRA, RCV_IN_BGRX } rcv_input_layout;
typedef enum { RCV_ISA_AUTO, RCV_ISA_SCALAR, RCV_ISA_SSE41, RCV_ISA_AVX2 } rcv_isa;

typedef struct {
    rcv_format        format;
    rcv_input_layout  input_layout;
    uint16_t          coded_width, coded_height;
    uint16_t          display_width, display_height;
    uint8_t           colour_matrix;      /* 0 BT.601, 1 BT.709 */
    uint8_t           full_range;         /* 1 = full */
    uint8_t           num_slices;         /* 0 = auto */
    uint8_t           num_threads;        /* 0 = auto: 2 if >= 4 logical CPUs, else 1 */
    uint8_t           predictor;          /* 1 = MED (default), 0 = LEFT */
    uint8_t           enable_skip;        /* temporal skip + auto-DUP, default 1 */
    uint8_t           enable_crc;         /* default 0 */
    uint16_t          keyframe_interval;  /* default 120 */
    uint32_t          fps_num, fps_den;
    rcv_isa           isa;                /* default AUTO */
    void            (*on_worker_start)(void* user, int worker_index); /* caller sets priority/affinity */
    void*             user;
} rcv_encoder_config;

typedef struct {
    const uint8_t* plane[3];   /* I420: Y,U,V; NV12: Y,UV,NULL; BGRA: pixels,NULL,NULL */
    int32_t        stride[3];
} rcv_frame_in;

typedef struct {
    uint8_t  force_keyframe;
    uint8_t  near;             /* 0..3, YUV420 only */
} rcv_encode_params;

typedef struct {
    uint32_t packet_size;
    uint8_t  frame_type;       /* 0 DUP, 1 I, 2 P */
    uint8_t  is_keyframe;
    uint32_t blocks_skipped, blocks_total;
    uint32_t time_skip_us, time_encode_us, time_total_us;
} rcv_frame_info;

rcv_status rcv_encoder_create(const rcv_encoder_config*, rcv_encoder** out);
void       rcv_encoder_destroy(rcv_encoder*);
size_t     rcv_max_packet_size(const rcv_encoder_config*);
void       rcv_write_sequence_header(const rcv_encoder_config*, uint8_t out[32]);

rcv_status rcv_encode_frame(rcv_encoder*, const rcv_frame_in*, const rcv_encode_params*,
                            uint8_t* out, size_t out_capacity, rcv_frame_info* info);
rcv_status rcv_encode_duplicate(rcv_encoder*, uint8_t* out, size_t out_capacity,
                                rcv_frame_info* info);          /* always 8 bytes */

/* Decoder */
typedef enum { RCV_OUT_I420, RCV_OUT_NV12, RCV_OUT_BGRA } rcv_output_layout;
rcv_status rcv_decoder_create(const uint8_t seq_header[32], uint8_t num_threads,
                              rcv_decoder** out);
void       rcv_decoder_destroy(rcv_decoder*);
rcv_status rcv_decode_frame(rcv_decoder*, const uint8_t* packet, size_t size,
                            rcv_output_layout layout, uint8_t* const plane[3],
                            const int32_t stride[3], rcv_frame_info* info);
void       rcv_decoder_reset(rcv_decoder*);   /* call after a seek; next packet must be I */
```

**Threading contract:** an encoder or decoder object is used by one caller thread at a time. `rcv_encode_frame` is synchronous — it blocks the caller until the packet is complete — so it **must never be called on the game's render thread**; the capture layer calls it from its own encoder thread.

---

## 8. Threading model

- The encoder owns a persistent pool of `num_threads − 1` workers; the calling thread is also a worker. Workers are created in `rcv_encoder_create` and live until destroy.
- Per frame there are up to two dispatches (Phase A, Phase B). A dispatch publishes an array of jobs; workers claim jobs with an atomic fetch-add on a job index. Completion uses an atomic counter plus C++20 `std::atomic::wait/notify`. Workers spin for about 2 µs (with `_mm_pause`) before waiting, to avoid wake-up latency.
- **Job order in Phase B:** biggest first — luma chunks (Y or G) before chroma — for better load balance.
- Each job writes its chunk into its own preallocated scratch buffer (worst-case size). After the phase, the caller writes the directory and copies the chunks into the output packet in fixed order. Output is therefore independent of scheduling (criterion A3).
- `on_worker_start` lets the capture layer set thread priority and affinity (e.g. below-normal priority, avoiding the game's main core). The codec itself does not change priorities.
- The decoder uses the same pool design; decoding is parallel across chunks.

---

## 9. Memory
- All buffers are allocated at create time, 64-byte aligned, with plane strides rounded up to 64 bytes.
- Encoder: reference frame (1.5 × W × H for YUV420, 3 × W × H for GBR) + per-job residual scratch + per-job chunk output scratch (worst-case sized) + skip bitmaps.
- **Target:** encoder total ≤ 32 MB at 1920×1080 YUV420 with 8 slices.
- No per-frame allocation, no `std::vector` growth, no `std::function` allocation in hot paths.

---

## 10. Project structure

```
rcv/
  CMakeLists.txt
  include/rcv/rcv.h                 C API (§7)
  src/
    format.h                        constants, header structs, (de)serialisation
    bitio.h                         MSB-first bit writer/reader
    huffman.h / huffman.cpp         histogram, length-limited build, canonical codes, decode LUT
    predict_scalar.cpp              MED/LEFT residuals, near-lossless, reconstruction
    predict_sse41.cpp               SSE4.1 lossless residual kernels
    predict_avx2.cpp                AVX2 lossless residual kernels
    skip_scalar.cpp / skip_sse41.cpp / skip_avx2.cpp
    colour.cpp                      NV12 de-interleave, BGRA↔GBR, output conversions
    encoder.cpp / decoder.cpp       frame logic, frame-type decision, packet assembly
    threadpool.h / threadpool.cpp
    cpu.cpp                         CPUID/XGETBV dispatch
    crc32c.cpp
  tools/rcv_cli.cpp
  tests/                            unit, round-trip, property, determinism, alloc tests
  fuzz/fuzz_decoder.cpp
  bench/bench_main.cpp
  docs/SPEC.md                      this document
  docs/DECISIONS.md                 decisions log
```

---

## 11. Tools, test corpus and tests

### 11.1 `rcv_cli`
```
rcv_cli encode  -i in.yuv -s 1360x744 -f yuv420 -r 60 [--near N] [--slices S] [--threads T]
                [--no-skip] [--keyint K] [--isa scalar|sse41|avx2] -o out.rcv
rcv_cli decode  -i in.rcv -o out.yuv            (raw planar output)
rcv_cli verify  -i in.yuv -c in.rcv             (round-trip MD5 / max-error check)
rcv_cli stats   -i in.rcv                       (per-frame type, size, ratio, skip %)
```
`.rcv` test files are a minimal test container: the 32-byte sequence header, then `[u32 packet_size][packet]` repeated. Production recordings use the container in the Implementation Plan (AVI OpenDML with FourCC `RCV1` is the working assumption).

Conversion for editing/sharing: `rcv_cli decode -i rec.rcv -o - | ffmpeg -f rawvideo -pix_fmt yuv420p -s 1360x744 -r 60 -i - -c:v libx264 -crf 16 out.mp4`.

### 11.2 Test corpus
1. **Minecraft (FRAPS baseline):** extract the raw frames without range conversion:
   `ffmpeg -i fraps_clip.avi -an -f rawvideo -pix_fmt yuvj420p minecraft_1360x744.yuv` (≈3.23 GB for 2,129 frames; a 600-frame subset is enough for day-to-day runs). The FRAPS duplicates decode to identical frames, so this also tests auto-DUP detection.
2. **A second, noisier game** (foliage, particles or film grain) — captured the same way — so compression targets aren't tuned to Minecraft alone.
3. **Desktop/UI capture** (text, flat colours, mostly static) — exercises skip and SINGLE mode.
4. **Synthetic vectors:** all-zero, all-255, random noise (forces RAW), gradients, checkerboards, single changed pixel per frame, tiny frames (2×2, 16×16, 18×18), widths 2…130 (every value, to hit all SIMD tails), odd strides.
5. **RGB (GBR) vectors:** the same content converted to BGRA.

### 11.3 Comparison baselines
Feed the *same* raw file to other lossless codecs (the input is declared as `yuv420p` so FFmpeg performs no conversion):
```
ffmpeg -f rawvideo -pix_fmt yuv420p -s 1360x744 -r 60 -i minecraft_1360x744.yuv -c:v utvideo -pred median ut.avi
ffmpeg -f rawvideo -pix_fmt yuv420p -s 1360x744 -r 60 -i minecraft_1360x744.yuv -c:v ffv1 -level 3 ffv1.mkv
```
Report RCV1 vs FRAPS (§1.3) vs Ut Video vs FFV1: ratio on real frames, ratio overall, encode ms/frame.

### 11.4 Required automated tests
- **Unit:** bit writer/reader round trip; Huffman build (Kraft equality, max length ≤ 12, canonical order); decode-LUT correctness; MED against a brute-force definition for all 2²⁴ (a, b, c) triples (fast enough to run).
- **SIMD equivalence:** for every kernel and ISA, random inputs × widths 1–130 × random strides must equal scalar output exactly.
- **Round trip:** every corpus item × {LEFT, MED} × {skip on/off} × {S = 1, 4, 8, max} × {threads 1, 2, 4} → bit-exact (A1).
- **Near-lossless:** NEAR 1–3 → max error ≤ NEAR (A2); a mixed sequence alternating NEAR per frame (simulating the rate controller) still decodes correctly.
- **Determinism (A3):** hash packets across thread counts and ISAs; all hashes equal.
- **Allocation (A11):** replace global `operator new`/`malloc` hooks in the test binary with counters; the counter must not change during encode/decode calls.
- **Bounds (A13):** every packet ≤ `rcv_max_packet_size`.
- **Frame-type logic:** keyframe interval honoured; identical frame → DUP; fully changed frame → I; P after seek → `RCV_ERR_NO_REFERENCE`.
- **Fuzzing (A12):** libFuzzer on `rcv_decode_frame` seeded with valid packets, under ASan + UBSan.

### 11.5 Benchmark harness (`rcv_bench`)
Per corpus and configuration, report: mean / p50 / p99 / max encode ms per frame; per-stage times (skip, predict + histogram, table build, entropy encode, assembly); decode ms/frame; compression ratio (real frames and overall); MB/s written at the recorded frame rate. Pin to a fixed core layout and run with the machine on AC power and a high-performance power plan; record CPU model and clock in the report.

---

## 12. Interfaces to the rest of the recorder (defined in the Implementation Plan)

| Component | What it gives the codec / takes from it |
|---|---|
| Capture hook (Present / SwapBuffers) | GPU copy of the back buffer; FRAPS-style frame-rate lock; calls `rcv_encode_duplicate` when no new frame |
| GPU converter | Scales to the output size (≤ back-buffer size) and converts to YUV 4:2:0 full-range BT.601 (NV12 or I420) before readback |
| Readback ring | Asynchronous staging textures with fences; hands `rcv_frame_in` to the encoder thread |
| Encoder thread | Calls `rcv_encode_frame`; owns the codec object |
| Rate controller | Watches the compressed-packet queue depth; sets `near` per frame (e.g. < 50% full → 0; 50–80% → 1–2; > 80% → drop frames and log) |
| Disk writer | Large aligned unbuffered writes; container muxing (AVI OpenDML, keyframe index from `is_keyframe`) |
| Hardware fallback | Separate path (QuickSync / NVENC / AMF), chosen at start from a disk benchmark; not part of RCV1 |

---

## 13. Build milestones (in order)

| M | Deliverable | Must pass before moving on |
|---|---|---|
| M1 | Project skeleton, CMake, C API stubs, `format.h`, bit I/O, scalar Huffman, **scalar lossless I-frame YUV420 encoder + decoder**, single-threaded, `.rcv` test container, `rcv_cli encode/decode/verify` | A1 on all synthetic vectors + Minecraft subset; unit tests green |
| M2 | Benchmark harness; first compression report vs FRAPS | A4 (≥ 2.49:1 real-frame median); report produced |
| M3 | SSE4.1 + AVX2 prediction/histogram kernels; CPU dispatch; optimised Huffman encode/decode loops | SIMD equivalence tests; A6 single-thread target measured and reported |
| M4 | Thread pool; slice-parallel encode/decode | A3 determinism; A6 two-thread target measured |
| M5 | Temporal skip (Phase A), P-frames, DUP, `rcv_encode_duplicate`, keyframe logic, in-place reference update | Frame-type tests; A8, A10; A1 still green |
| M6 | GBR format (BGRA input, reversible transform, BGRA output) | A1 on RGB vectors |
| M7 | Near-lossless mode, per-frame NEAR switching | A2, A7 |
| M8 | Hardening: full validation rules (§6.5), CRC-32C, `rcv_max_packet_size`, allocation test, fuzzing | A11, A12, A13 |
| M9 | Final report: all criteria A1–A13 with measured numbers; Ut Video / FFV1 / FRAPS comparison table | All criteria met or gaps documented with causes |

---

## 14. Possible v2 extensions (do not build in v1)
- Zero-run symbols or rANS entropy coding for very flat content (Huffman can't go below 1 bit per sample, which caps compression in flat areas at 8:1).
- Gradient predictor or per-chunk automatic predictor choice.
- Per-block (rather than whole-block-exact) skip with residual coding against the previous frame.
- FFmpeg decoder patch or VfW codec so recordings open directly in editors.
- 4:4:4 YUV, 10-bit input.

---

## Appendix A — Reference pseudo-code

### A.1 Encoder, lossless chunk (one plane, one slice)
```
count = 0; hist[256] = {0}
for row j in slice rows:
    for col i in 0..plane_w−1:
        if block_of(i, j) is skipped: continue
        pred = predict(i, j)            // §5.4 rules, neighbours from reference/working buffer
        sym  = (x[j][i] − pred) & 0xFF
        resid[count++] = sym; hist[sym]++
choose mode (§5.6.2)
if HUFFMAN: lengths = build_limited(hist, 12); codes = canonical(lengths)
            write mode, 128-byte length table, then for k in 0..count−1: put_bits(codes[resid[k]], lengths[resid[k]])
            flush + pad to 4 bytes
```
(The SIMD version computes `sym` for whole rows at once and skips over skipped blocks using the bitmap.)

### A.2 Decoder, chunk
```
read mode
for row j in slice rows:
    for col i:
        if block_of(i, j) is skipped: continue            // value already in reference
        sym  = next_symbol()                               // LUT / single / raw
        pred = predict(i, j)                               // from already-decoded neighbours
        if near == 0: x = (pred + sym) & 0xFF
        else:         q = (int8)sym; x = clamp(pred + q*(2n+1), 0, 255)
        ref[j][i] = x
```

### A.3 Length-limited Huffman (deterministic)
```
f = hist (copy)
loop:
    lengths = huffman_lengths(f)      // ties broken by lower symbol value first
    if max(lengths) <= 12: return lengths
    for s in 0..255: if f[s] > 0: f[s] = (f[s] + 1) >> 1
```
Tie-breaking must be fully specified in code (e.g. a stable sort by (count, symbol)) so every build produces the same table.

---
*End of codec specification. Next document: Implementation Plan (capture hook, GPU conversion, readback ring, rate controller, container and disk writer, hardware fallback).*
