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

## M6

**D-034 — GBR encoding without a reference copy.** A prepare pass (one job per slice, the same
dispatch as phase A) converts the BGRA/BGRX input into encoder-owned "staging" planes G, B-G, R-G
(§4.4) and, when phase A runs, compares them with the reference (equal planes <=> equal RGB, the
transform is a bijection). Phase B predicts straight from the staging planes; afterwards staging and
reference swap pointers, because a lossless reconstruction equals the source. YUV input keeps the
copy into the reference (its buffers belong to the caller). For GBR, `time_skip_us` includes the
conversion.

**D-035 — Output layouts.** The codec stores colour metadata but never converts between YUV and RGB
(§4.3): YUV420 streams decode to I420 or NV12, GBR streams to BGRA (alpha = 255). Other combinations
return `RCV_ERR_UNSUPPORTED`.

**D-036 — GBR details.** Colour conversion kernels are SSE2 (x64 baseline, 16 pixels per step, tested
against scalar references), so no ISA dispatch. Odd sizes are allowed for GBR (§4.5 only requires
even sizes for YUV420). Alpha is ignored: BGRX and BGRA with any alpha give identical packets.
Near-lossless with GBR is `RCV_ERR_INVALID_ARG` (never allowed, §4.1), not "unsupported yet".

**D-037 — RGB measurements.** Ratios for RGB use 3 bytes per pixel as the raw size (alpha is not
coded). The RGB test content is the FRAPS recordings converted to BGRA, i.e. upsampled 4:2:0
chroma, which is smoother than a true RGB capture, so RGB compression figures are optimistic until
real RGB captures (OpenGL/D3D9 readback in the recorder) are available. `compare_rgb.ps1` pipes the
frames from the recording through FFmpeg to avoid a 6.5 GB corpus on disk.

## M7

**D-038 — Near-lossless encoding.** Exactly plan §5.5: `q = sign(e)·((|e|+n) div (2n+1))`, reconstruction
`clamp(pred + q·(2n+1))`, symbol `q & 0xFF`; prediction uses reconstructed neighbours, so it is serial
along a row. Implementation: the MED clamp form (D-023), and per-NEAR tables indexed by `e + 255`
giving the symbol and `q·(2n+1)` (no division or multiply on the dependency chain). Two consecutive
full rows are coded as a **wavefront** (row j+1 one sample behind row j, which is all its prediction
needs) so two serial chains overlap on one core; the output is identical to coding the rows one after
the other. Rows with skipped blocks use the single-row path. NEAR is taken per frame from
`rcv_encode_params`.

**D-039 — Near-lossless skip.** Phase A compares with tolerance NEAR (SSE2 saturating differences);
lossless keeps the exact compare. The comparison is with the *reference* (the previous
reconstruction), so a skipped block's samples stay within NEAR of the current frame's source: the
error bound holds on every frame, with no drift along P-frame chains. Skipped blocks keep the
reference samples (no copy).

**D-040 — Decoder reconstruction policy.** All reconstruction loops (single, paired, masked rows) take
a policy: lossless adds modulo 256 (same code as before), near-lossless adds the dequantised value
from a 256-entry table and clamps. The table is defined for every byte, so corrupt symbols can't
index out of range or overflow.

**D-041 — Verifying near-lossless streams.** `rcv_cli verify` allows the largest NEAR seen so far
(header byte 9; a DUP carries none) and reports the maximum error per plane; `rcv_bench --near`
checks every frame against its own NEAR.

## M8

**D-042 — CRC-32C.** The SSE4.2 `crc32` instruction, one 8-byte stream, chosen once at load time
from CPUID (the checksum is identical either way; the table version remains for CPUs without
SSE4.2). Measured 0.16 ms per MB against 2.5 ms for the table, so a 3-stream interleave with CRC
combining isn't worth its complexity, especially as the checksum is off by default.

**D-043 — Reserved fields.** The decoder rejects non-zero reserved bits and bytes: sequence header
colour bit 7, byte 15 and bytes 26–31; frame header colour bit 7 and bytes 18–19 (chunk header
bytes 2–3 already were, D-008 covers code tables). The encoder always writes zeros, and a future
format change bumps the version, so strictness costs nothing and keeps garbage from passing as valid.

**D-044 — Thread start-up belongs to create.** `ThreadPool::start` returns only when every worker is
running and its `on_worker_start` hook has returned. A new thread allocates on its first run (the
CRT's per-thread data, ~2 KB), which otherwise could land inside the first encode or decode call;
the debug allocation test found this (A11). It also guarantees the caller's priority/affinity
settings are in place before the first frame.

**D-045 — Allocation test method (A11).** `test_alloc.cpp` replaces every global `operator new` and
`operator delete` in the test binary with counting versions; the codec allocates only through
`operator new` (`aligned_buffer.h`), so this sees all of its allocations in every build. Debug
builds also install `_CrtSetAllocHook`, which sees every CRT heap allocation from any module and
thread (malloc, `_aligned_malloc`, the CRT's own blocks). Counted: encode I/P/DUP/forced-I/near/
RAW/SINGLE frames, `rcv_encode_duplicate`, rejected calls, decode to every output layout and to none,
corrupt packets, NO_REFERENCE after a reset; 1 and 4 threads, I420/NV12/BGRA input, CRC on and off.

**D-046 — Fuzzing (A12).** `rcv_fuzz` (clang-debug preset only) fuzzes the decoder with libFuzzer,
ASan and UBSan (`-fno-sanitize-recover=all`, so UBSan findings are crashes). Input: a sequence
header, an options byte (decoder threads, output layout, padded strides), then up to 16
length-prefixed packets, so P-frames and DUPs see real references. Sizes above 65,536 samples are
skipped to keep each input fast; every packet and output plane is copied into an exactly-sized heap
block so ASan catches any access past it. The harness also traps if the sequence-header parser and
`rcv_decoder_create` disagree, or on an unexpected status. Seeds come from `rcv_fuzz_seeds` (56
valid streams: both formats, odd and even sizes, both predictors, 1/default/max slices, NEAR 0–3,
CRC, RAW/SINGLE/HUFFMAN chunks, I/P/DUP). Build details: the libFuzzer that ships with Visual
Studio is built for the static CRT, so the target uses `/MT` and disables the STL's ASan container
annotations to match it; CMake links with `lld-link` directly, so the sanitizer runtimes are named
explicitly.

## Recorder

**D-047 — Third-party versions.** Vendored in `third_party/` at pinned versions (details and kept
files in `third_party/README.md`): CLI11 v2.7.2, toml++ v3.4.0, spdlog v1.17.0, MinHook v1.3.4, and
kiero2 at commit `8f57dd9` — the commit reviewed in recorder plan §4.6.1, still `master` on
2026-10-01. Only sources, headers and licences are kept; vendored files are never edited.

**D-048 — x86 builds.** Presets `x86-debug` / `x86-release` use the x64-hosted x86 compiler
(`vcvarsamd64_x86`; `build.bat` picks it from the preset name) into `build/x86-*`. The x86 tree
builds only what must be 32-bit — `rec_common` and `rec_inject32` now, `rec_hook32.dll` and the 32-bit
test app later; the codec stays x64 only. Windows API settings (`UNICODE`, `NOMINMAX`,
`WIN32_LEAN_AND_MEAN`, `_WIN32_WINNT=0x0A00`) are attached to the recorder targets through
`rec_common`, not globally, so the codec and its tools build exactly as before.

**D-049 — Event registry.** All codes live in one X-macro table in `common/include/rec/events.h`
(id, code, name, description); level and subsystem are derived from the code and checked at compile
time. The whole code is the identifier because the plan uses 1301 twice (`I1301` display refresh,
`W1301` pacing error), so hook log records will carry level + number. Three codes beyond the plan's
catalogue: `I7004 command` (every rec invocation, with its arguments), `I7005 doctor_result`,
`I7006 config_changed`.

**D-050 — Host logging details.** One spdlog async logger per subsystem (`hook`, `ipc`, `encoder`,
`writer`, `audio`, `system`, `cli` — the plan's `[writer ]` column), sharing the sinks; one logger
thread at below-normal priority (§3.2). The queue (32,768 messages) blocks when full instead of
dropping: host threads log rarely (warnings, one summary per second), and a silently lost warning
would defeat R9; the hook never uses spdlog (§10.2). Level names are written by a custom formatter
(`INFO `, `WARN `, `ERROR`, `FATAL`). The session log is a distributing sink present in every logger,
filled while a recording is open. spdlog's asynchronous `flush()` only posts a request, so closing
a session or flushing waits on a barrier (a flush posted to a private logger on the same single
queue); the test checks that a 2,000-line burst lands completely in the session file.

**D-051 — Configuration.** `rec.toml` lives in `%LOCALAPPDATA%\rec\` next to the logs (per
machine, since it will cache disk benchmarks per volume); `--config` points elsewhere. A missing
file means defaults; `rec config set/reset` write every key in the plan's order (strings as TOML
literal strings, so Windows paths need no escaping). A bad or unknown entry is reported and its
default kept; only an unparsable file stops a command. Command-line recording options are applied
as config keys, so they go through the same type and range checks.

**D-052 — rec doctor.** Read-only: the output folder is tested with a temporary delete-on-close
file in it or, if it doesn't exist yet, in the nearest existing parent. Disk speed is reported as a
warning until `rec bench-disk` exists (M5), together with the rate the settings need: the plan's
§9 estimate (raw rate ÷ 2.49, the FRAPS ratio; 33 MB/s at 720p60), which also gives the minimum
recording time for the free space, plus the typical rate from the codec's measured Minecraft ratio
(5.7:1). Identical
adapters/encoders listed twice by Windows (seen here with a virtual-display driver) are shown once.

**D-053 — CLI conventions.** Exit codes: 0 success, 1 error, 2 command not implemented yet (the
message names the milestone). Every command is logged (`I7004`) and every failure goes to the log
as well as the console. Recorder tests reuse the codec's test harness and main (D-003), so no test
framework download is needed.
