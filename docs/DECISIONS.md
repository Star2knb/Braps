# Decisions log

Decisions made while implementing `RCV1_Codec_Plan.md` and `Recorder_Implementation_Planv2.md`
where the plans were silent, ambiguous, or deliberately deviated from. Newest last.

## Toolchain and layout

**D-001 — Visual Studio 2026 instead of 2022.** The development PC has Visual Studio Community 2026
(18.9, MSVC 19.51 / toolset 14.51). Plans say "MSVC 2022"; the newer compiler is a superset for our
purposes (C++20, same intrinsics, same ABI family). Builds use CMake + Ninja from the VS install.

**D-002 — Repository layout.** Follows the recorder plan §18: the codec lives in `codec/` inside the
`rec` repo (root `CMakeLists.txt` adds subdirectories). `build.bat [debug|release|clang-debug]`
sets up the x64 developer environment and builds a CMake preset into `build/<preset>/`.
V2 is developed on branch `braps-v2` of github.com/Star2knb/Braps, branched from `main`; the V1
recorder is kept unchanged under `legacy/` for reference and is not built.

**D-003 — Test framework.** A ~60-line in-house harness (`codec/tests/testfw.h`) instead of doctest,
so M1 needs no downloads. Same capabilities we use (named cases, CHECK/REQUIRE, filter by name).
Can be swapped for doctest later without touching test bodies much.

## Codec API (additions to plan §7)

**D-004 — Extra functions.** `rcv_encoder_config_init` (a zeroed config would select the LEFT
predictor and disable skip, which are not the documented defaults), `rcv_parse_sequence_header` +
`rcv_sequence_info` (tools and the host need the dimensions), `rcv_status_string`, and the `RCV_API`
export macro for the DLL build.

**D-005 — Output buffer contract.** `rcv_encode_frame` requires `out_capacity >= rcv_max_packet_size`
and returns `RCV_ERR_BUFFER_TOO_SMALL` otherwise, even if the actual packet would have fit. This keeps
the encoder free of mid-frame overflow checks and makes the failure deterministic.

**D-006 — Defaults.** `display_width/height = 0` means "same as coded". `keyframe_interval = 0` means 120.

**D-007 — Decode without output.** `rcv_decode_frame` with `plane == NULL` or `plane[0] == NULL`
decodes into the reference only (useful for seeking/benchmarks).

## Bitstream strictness (decoder, plan §6.5)

**D-008 — Complete Huffman codes only.** A HUFFMAN chunk's lengths must satisfy Kraft with
*equality* (sum of 2^-L = 1). The encoder always produces complete codes (Huffman trees are full,
including after the frequency-halving length limit), so this rejects nothing valid and guarantees
every 12-bit LUT entry is defined.

**D-009 — Exact sizes.** The decoder rejects: chunk directory sizes that don't sum *exactly* to the
rest of the payload; HUFFMAN chunks whose size isn't exactly `132 + align4(bytes of bitstream used)`;
RAW chunks whose size isn't `4 + align4(samples)`; non-zero reserved bytes in chunk headers;
unknown flag bits; a non-zero CRC field when the CRC flag is clear; NEAR != 0 without the
near-lossless flag (and vice versa); frame format/size different from the sequence header.

**D-010 — frames_since_I / frame number.** An I-frame sets `frames_since_I = 1`; every later packet
(P or DUP) increments it, so with interval K the next forced I-frame is packet K after the last one.
The informational frame number counts every packet, DUP included.

## M1 scope notes

**D-011 — Implemented early because trivial:** DUP encode/decode (`rcv_encode_duplicate`), scalar
CRC-32C, NV12 input and NV12 output. **Not yet implemented, return `RCV_ERR_UNSUPPORTED`:** GBR
format and BGRA output (M6), P-frames (M5), near-lossless (M7). `num_threads`, `isa` and
`enable_skip` are accepted and ignored until M3–M5.

**D-012 — Two-pass decode in M1.** Each chunk is entropy-decoded into a symbol buffer, then
reconstructed. Simpler to validate; fusing the passes is an M3 optimisation. *(Superseded by D-023.)*

**D-013 — `rcv_cli verify` compares bytes directly** (reports differing frames and max error per plane)
instead of computing MD5s; it is a strictly stronger check for a round trip.

## M2

**D-014 — `rcv_encode_params.near` renamed to `near_level`.** `<windows.h>` defines `near` (and `far`)
as empty macros, so the plan's field name made `rcv.h` fail to compile after `windows.h` - which is
exactly how the capture DLL and host will include it. The codec plan §7 is updated to match, and a
test compiles `rcv.h` after `windows.h`.

**D-015 — New Minecraft corpus; A4 measured against FRAPS on the same clip.** The original FRAPS clip
(plan §1.3) was deleted. A new FRAPS recording of the same setup (1360x744 yuvj420p, 60 fps, 1,597
frames, i5-7200U) is the corpus. On it FRAPS's real-frame median is 2.832:1 (not 2.49:1: different
scene, far fewer duplicates). A4 ("FRAPS parity") is therefore checked against 2.832 on identical
frames; the plan's 2.49 remains a floor. Corpus frames are extracted with `-pix_fmt yuvj420p`
(no range conversion) into `corpus/` (git-ignored). Known quirk: FFmpeg's FRAPS decoder drops the
8-byte duplicate packets and its constant-frame-rate logic fills each gap by repeating the *next*
frame, so in 17 places a new frame appears one slot early (then repeats). Frame contents are exact;
only those duplicate positions move by one slot.

**D-016 — "Real frames" in reports.** rcv_bench codes a frame identical to its predecessor as a DUP
packet (what the recorder host will emit, and what the M5 auto-DUP will do). Ratios are taken over
real frames: for a codec that stores duplicates as marker packets (<= 64 bytes: FRAPS, RCV1 DUP) the
frames it coded in full; for a codec without markers (Ut Video, FFV1) the frames that differ from
their predecessor. This measures each codec on what it actually stored and is immune to the
one-slot shift in D-015 (FRAPS: 1,443 coded frames; identical-frame detection: 1,441).

**D-017 — Stage profiler.** `src/profile.h` gives rcv_bench per-stage encoder times (load,
predict + histogram, table build, entropy write, other) through an internal, non-exported hook.
With no sink attached it costs one branch per stage.

**D-018 — External baselines.** Ut Video (`-pred median`) and FFV1 (`-level 3`, other options default)
are run by FFmpeg with `-threads 1` on the raw corpus declared as `yuv420p` (no conversion), piped as
NUT into ffprobe to collect per-frame packet sizes (`codec/bench/compare_baselines.ps1`). Their
encode time is FFmpeg's user CPU time divided by frames, so it includes small demux/mux overhead
and is approximate.

## M3

**D-019 — ISA levels and detection.** `rcv_encoder_create` resolves the level once: AUTO picks the
best supported; an explicit level the machine can't run returns `RCV_ERR_UNSUPPORTED`. The AVX2
level also requires BMI1/BMI2 (the AVX2-built Huffman writer emits `shrx`) and OS-saved YMM state
(XGETBV). Addition to the API: `rcv_cpu_isa()`. `RCV_FORCE_ISA=scalar|sse41|avx2` is read only by
the tests (whole suite re-runs at that level) and `rcv_cli` (when `--isa` is absent).

**D-020 — ISA-specific code has internal linkage.** An inline function or template instantiated in a
`/arch:AVX2` file can be picked by the linker (COMDAT folding) for baseline code and crash a
non-AVX2 CPU. So SIMD files use only file-local helpers, and code compiled at two levels
(`huff_write_impl.inl`) is included into each file inside an anonymous namespace. Kernels are only
reached through function pointers chosen after CPUID.

**D-021 — Encoder may scribble inside a chunk's RAW bound.** The fast writer stores 8 bytes at a
time. It may write anywhere in `[chunk, chunk + 4 + align4(samples))` - a range that always lies in
the packet buffer because every earlier chunk is at most its RAW bound - but never past it. Packet
bytes are unaffected (identical to the reference writer); bytes of `out` beyond `packet_size` are
unspecified.

**D-022 — Huffman length limiting: same algorithm, faster.** The halving procedure of plan §5.6 /
Appendix A.3 is unchanged and produces the same lengths; the keys are sorted once and retries
re-order with an insertion sort.

**D-023 — Decoder: fused, paired, deferred bounds check.** Entropy decoding and reconstruction run in
one pass into the reference frame; neighbouring HUFFMAN chunks of a plane are decoded in lockstep
(two independent dependency chains per core). MED is computed as
`clamp(a + b - c, min(a,b), max(a,b))`, tested equal to the plan's select form for all 2^24 inputs.
Past the end of a chunk the bit reader supplies zeros and its bit count goes negative - possible
only after every byte has been loaded - and that is checked once per chunk, so the per-symbol path
has no bounds branch. The reader never reads past the chunk; invalid streams are still rejected.

## M4

**D-024 — Thread pool.** `num_threads - 1` persistent workers per encoder/decoder plus the calling
thread (plan §8). A dispatch is published as one 64-bit state word: generation (32 bits), job count
(16), next index (16); a job is claimed by compare-exchange on that word, and the index is checked
against the count *from the same word*. Workers spin ~2 µs, then sleep with C++20 `atomic::wait`;
the caller waits only for jobs to finish, never for idle workers, so a worker the game has pushed
off the CPU cannot stall a frame. No allocation per dispatch.
*Bug found and fixed during M4:* the first version kept the job count in a separate atomic. A worker
late for dispatch g could read g's final state word, then the *next* dispatch's larger count
(published just before its state word), and successfully claim a non-existent index of g - running
a bogus job and decrementing the next dispatch's `remaining`, which then never reached zero
(deadlock). It surfaced only in a repeated run at lower priority; the pool test now alternates
tiny/large dispatches 20,000 times per thread count with jobs that yield, and ctest has a timeout.

**D-025 — Encoder jobs.** One job per (plane, slice), luma first: it copies its own input rows into
the reference, predicts and entropy-codes into a private scratch buffer the size of its RAW bound,
using a per-worker residual buffer. The caller then writes the directory and copies the chunks in
fixed order, so packets are identical for any thread count (A3). With a stage profiler attached,
stage times are CPU time summed over workers; `total` is wall time.

**D-026 — Decoder jobs.** One job per pair of neighbouring HUFFMAN chunks (M3's lockstep decode), or
per single chunk otherwise, luma first; two decode tables per worker. If several jobs fail, the
first failure in job order is reported, so errors don't depend on scheduling either.

**D-027 — Thread counts.** 0 = auto: 2 if the machine has >= 4 logical CPUs, else 1 (plan §7); capped
at 32. Applies to both `rcv_encoder_config::num_threads` and `rcv_decoder_create`. The codec never
changes thread priority or affinity itself; `on_worker_start` lets the caller do it (encoder only).
`RCV_FORCE_THREADS=N` re-runs the test suite with N encoder threads.

**D-028 — Second corpus: Warframe.** FRAPS recording, 1280x720 `yuvj420p`, 60 fps, 791 packets
(446 real, 345 duplicate). Noisy, detailed content (FRAPS 1.58:1), as plan §11.2 asks. FFmpeg
decodes 790 frames (it drops one trailing duplicate). The comparison script now names its output
files per corpus (`<corpus>.sizes_*.txt`).

## M5

**D-029 — Frame types.** Implemented exactly as plan §5.2: forced keyframe, `frames_since_I >=
keyframe_interval` or no reference -> I without phase A; skip disabled -> I; every block unchanged ->
DUP (`rcv_encode_frame` itself returns the 8-byte packet and leaves the reference alone); no block
unchanged -> I; otherwise P. `force_keyframe` wins even over an identical frame. An I-frame sets
`frames_since_I = 1`; P and DUP (from either entry point) add one (D-010).

**D-030 — Skip compare (phase A).** One job per slice. Lossless skip = every sample of the block equal
in all three planes. The compare sweeps each block row row-by-row across the full width with SSE2
(the x64 baseline, so no ISA dispatch), keeps a still-unchanged flag per block, skips blocks already
known to differ and stops when all differ - on a changing frame usually after the first row. NV12
chroma is compared by interleaving the reference Cb/Cr on the fly. A fully unchanged frame has to be
read completely (~3 MB at 1360x744); that is memory-bandwidth bound on the i5-7200U (0.44 ms on one
thread, 0.28 ms on two). Per-block hashes of the reference would halve the reads but a collision
would silently skip a changed block, breaking losslessness, so they were rejected.

**D-031 — P-frame encoding.** A row whose blocks are all skipped costs nothing. Otherwise the whole
row is copied into the reference - equivalent to copying only the non-skipped blocks (§5.7) because
a lossless skip means those samples are already identical - residuals are computed for the full row
with the SIMD kernel, and only the non-skipped samples are kept, in raster order. Near-lossless skip
(M7) tolerates differences and will need the per-block copy.

**D-032 — P-frame decoding.** The skip map is strict: bits past the last block and padding bytes must
be zero. Coded sample counts come from geometry + skip map. P-frame chunks are decoded singly (rows of
neighbouring slices have different skip patterns, so no lockstep pairing). A P packet rejected before
decoding starts (header, skip map, directory) leaves the reference intact; one that fails midway has
already overwritten part of it, so the decoder returns `RCV_ERR_NO_REFERENCE` for P/DUP until the next
I-frame.

**D-033 — rcv_bench and temporal skip.** With skip on, every frame goes through `rcv_encode_frame`, so
the encoder's own phase A finds duplicates; with skip off, frames identical to the previous one go
through `rcv_encode_duplicate` (the recorder host's path for timeline gaps), which keeps "noskip" rows
comparable with the M2-M4 reports. `--no-decode` measures the encoder alone, as the recorder runs it.
