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

## Recorder M1

**D-054 — Output folder.** `rec.exe`, `rec_hook64.dll` and `rec_testapp.exe` are built into
`<build>/bin` (e.g. `build\release\bin`), because the host looks for the hook DLL next to itself.
Codec and test binaries stay where they were. `rec` depends on the hook target, so one build makes
both.

**D-055 — Static CRT in the hook.** `rec_hook64.dll` (and later `rec_hook32.dll`) link the CRT
statically (`/MT`), as do MinHook and kiero2, so the DLL never depends on a Visual C++ runtime that
the game may lack or have in a different version. It therefore cannot link `rec_common` (built `/MD`
for the host); `rec_common_headers` is an interface library with the same include path and Windows
settings, and the hook uses only header-only parts of `common` (events, protocol).

**D-056 — kiero2 integration.** Only the D3D11 backend is compiled (the plan's list is D3D9 / D3D11 /
D3D12 as they arrive; the `Implementation_*` numbers depend on include order, so
`hook/kiero_wrap.hpp` and the `KIERO_IMPL_FIRST_SLOT` in `third_party/CMakeLists.txt` must change
together). `hook/kiero_config.h` is force-included into kiero2's sources to send `KIERO_DBG_MSG` and
`KIERO_ASSERT` to the log ring. `locate` runs under SEH on the install thread; entries 8 (`Present`)
and 22 (`Present1`) are read by index and checked to lie inside the image of `dxgi.dll` (W1106).
`ResizeBuffers` is not hooked yet: nothing holds the back buffer until M2.
*Plan inconsistency:* §4.4 says the D3D11 lookup also covers D3D10 games, but kiero2 needs `d3d11.dll`
to be loaded and §4.1 forbids loading it ourselves, so a D3D10-only game (no `d3d11.dll`) is not
hooked. The hook waits, logging I1103, and the status line says so.

**D-057 — Hook log records.** The hook sends events as text: the 48 bytes of `arg[]` + `tag` hold up
to 47 characters of detail and `code` + `level` identify the event, so the host prints
`I1101 backend_selected D3D11 1280x720 fmt=87 locate=28ms` and counts it like its own events. No
numeric arguments are needed yet. Added `E1107 hook_exception` (an exception inside the Present guard;
capture/measuring is disabled, the game goes on).

**D-058 — Which swap chain.** The first swap chain at least 160 px on each side that presents is
measured (§4.3); presents of others (overlays) are counted in `present_ignored`. If the measured
chain is silent for one second, the next chain to present takes over (the game recreated its swap
chain). `DXGI_PRESENT_TEST` calls are not frames. A nested call (Present calling Present1 through the
vtable) is counted once, using a per-thread depth in a TLS slot from `TlsAlloc` rather than
`thread_local`, because the DLL is loaded and unloaded while the game runs. The detours preserve
`GetLastError`.

**D-059 — Detach.** Disable the hooks, wait for the in-detour counter to reach zero, wait 250 ms (a
thread that has left its last counted instruction still has to execute the `ret` inside our DLL), look
again, then uninitialise MinHook and `FreeLibraryAndExitThread`. If a thread is still inside Present
after 2 s (a hung game) the DLL stays loaded with the hooks off, rather than risk the game. A
re-attach to a still-loaded hook takes over its shared memory (`attach_count`); a second `rec` that is
still alive is refused.

**D-060 — Injection.** `CreateRemoteThread(LoadLibraryW)` as in the plan; `launch` creates the
process suspended and injects before the first thread runs (a launched game that loads Direct3D late
gets I1103 and the deferred install). If injection fails the suspended process is terminated, since
the user asked for a game with the hook. Elevated targets (E1003) are recognised from the token when
`OpenProcess` is denied. 32-bit targets are refused with a message until `rec_inject32` (M7).

**D-061 — Anti-cheat block now, not at M7.** Injection ships with the refusal (E1004), `--force` to
override. Sources: the target's modules, all running processes, loaded kernel drivers, and for
`launch` the file and folder names in the game's folder to two levels. List = `safety.anticheat_blocklist`
plus built-in names (`builtin_anticheat_names()`). Matching: equal names (extension ignored), or for
entries of 6+ characters an occurrence at the start of a name or after a non-alphanumeric character;
plain substring matching flagged `WindscribeService.exe` for "BEService". For a launched game the
modules are scanned again 3 s after start; if an anti-cheat module appeared the hook is removed (E1004).
Consequence of scanning the whole system as the plan says: a machine with an anti-cheat driver or
service running (Vanguard's `vgk`, for example) needs `--force` for every game.

**D-062 — Control block.** Added to the protocol: the measured swap chain's size and format,
`present_ignored`, and `hook_cost_total_ns` (the host derives the average cost per Present from deltas;
the hook keeps no history). Host heartbeat period 250 ms, hook declares the host lost after 3 s.

**D-063 — Commands.** `launch` and `attach` take `--force` and `--duration S` (detach and exit after S
seconds; used by the test scripts). Ctrl+C detaches and exits; the game keeps running either way.
`rec detach` with no arguments removes the hook from every process that has it. `rec list` shows
processes with D3D9/10/11/12, OpenGL or Vulkan loaded (DXGI alone is not enough).

**D-064 — Test app.** `rec_testapp` draws with `ClearView` on rectangles (no shaders yet): a 32-bit
frame-counter barcode and moving colour bars. d3d11.dll and dxgi.dll are delay-loaded so that
`rec launch` can be tested on the deferred install (`--late-load-ms`).

## Recorder M2

**D-065 — Protocol version 2.** The control block grew to 512 bytes: the recording configuration
(tick rate, output size, slot counts, `t0`), the session state the host wants (`host_state`: idle /
recording / stopping) and the state the hook reports (`capture_state`), and the capture counters.
The host writes the configuration and then publishes it with `host_state` (release/acquire). Each
recording has its own frame ring and semaphore, named by `rec_generation`
(`Local\rec_<pid>_f<generation>`), so a new recording never meets the objects of the last one.
A hook of version 1 left in a game is refused by a version 2 host ("not compatible").

**D-066 — NV12 slot layout.** In a ring slot the UV plane follows the Y plane directly (offset
`64 + w*h`, no padding), so the whole frame is one contiguous `w x 3h/2` block, which is how the GPU
pass produces it and how the worker copies it.

**D-067 — One conversion pass.** The plan's two passes (Y, then UV) became one draw into a single
`R8_UNORM` target of `w x 3h/2` texels: rows below `h` are luma, the next `h/2` rows are the
interleaved chroma bytes (Cb on even columns, Cr on odd). Measured: fewer state changes and one
staging copy instead of two cut the GPU-issue time of the hook from ~440 µs to ~280 µs per frame. The
chroma bytes are sampled four times each (twice the chroma work of two passes; negligible on the GPU).
The shader is the BT.601 full-range formula of the codec plan §4.3; on the test app's colour bars the
captured values differ from the formula by at most 1/255 (checked on every frame of the test).

**D-068 — Staging slots: default 6, range 2–8.** The plan's default of 3 gave W1201 about 14 times a
second on a vsync-locked 60 fps game: the GPU finishes our copy 4–5 `Present`s after we issue it (the
flip-model frame queue lets the CPU run up to 3 frames ahead), so one capture per frame needs 5–6
slots in flight. `record.staging_slots` defaults to 6 now; each slot is one NV12 texture (1.4 MB at
720p).

**D-069 — Copy worker thread (the plan's optional optimisation, enabled).** Reading mapped staging
memory costs ~600 µs per 720p frame on this laptop's Intel GPU (the driver maps its pages on demand;
streaming loads did not help), which on the render thread would alone exceed the P2 target. So the
render thread only `Map`s (DO_NOT_WAIT, after the query says the GPU is done), a worker thread copies
into the shared ring, and the next `Present` unmaps and publishes the slot (one more `Present` of
latency). The worker touches only memory, never D3D, and its copy runs under SEH so a mapping that
disappears (device removed) disables capture instead of killing the game. The `hook_cost_us` of a frame
counts the render thread's time only; the worker's ~0.6 ms per frame (about 4% of one core at 60 fps)
runs on another thread inside the game's process and is reported here, not in P2.

**D-070 — Not disturbing the game.** Our draw runs inside our own `ID3DDeviceContextState` swapped in
with `SwapDeviceContextState` (swapping costs ~4 µs) and swapped back, so the game's pipeline state is
untouched. The sequence is also bracketed by `ID3D11Multithread::Enter/Leave` when the device has it,
so a game that presents from a different thread than it renders on can't have its immediate-context
calls interleaved with ours; an exception inside the sequence releases the lock before the hook
disables itself.

**D-071 — Source formats.** The back buffer is copied to a typeless texture and viewed as UNORM, so the
values are the encoded ones the screen shows (an sRGB view would linearise them). B8G8R8A8, R8G8B8A8
(UNORM, sRGB, typeless) and R10G10B10A2 are handled; R16G16B16A16_FLOAT is clamped and sRGB-encoded in
the shader (W1204); an MSAA back buffer is resolved first; anything else disables capture (E1205).
Added `E1211 capture_failed` for failures during capture (stage and HRESULT in the text; a removed
device logs E1209 instead).

**D-072 — One frame per tick, free mode.** The tick of a `Present` is `round((now - t0) * fps / freq)`;
a frame is captured if its tick is greater than the last captured one. That is the plan's free mode
without a lock. When the game runs at the recording rate, jitter of ±1 ms around a tick boundary makes
some ticks get no frame (the host will fill them with DUPs in M3) and none get two; observed 0–8% of
ticks empty depending on the phase between the game and the tick grid. Lock mode (M3) removes that.

**D-073 — Size and rate.** Output size = `record.size` (or the back buffer for `native`), shrunk to fit
inside the back buffer with its aspect ratio, rounded down to even; the picture is letterboxed into it.
Rate = `record.fps` but not above the refresh rate of the monitor the game window is on (I1301 logs
it). The hook republishes the back buffer's size every 32 `Present`s (fullscreen switches change it);
if the game resizes during a recording the picture is rescaled into the same output (I1208), but the
test app's barcode check uses the size at the start, so it reports unreadable frames then (resize
handling proper is M7).

**D-074 — Hotkey and cues.** A thread with a message loop calls `RegisterHotKey` (with `MOD_NOREPEAT`);
if the key is taken it installs a `WH_KEYBOARD_LL` hook on the same thread and logs E7003. Presses within
300 ms of the last one are ignored (W7002), as is a press while stopping. The start, stop and error
sounds are generated in memory (two short sine tones; 22 kHz mono WAV) and played with `PlaySound`, so
no resource files are needed. The F9 press is logged as I7001.

**D-075 — What "recording" means in M2.** Until the encoder and AVI writer (M3), a recording is a
capture test: frames travel hook → ring → host, are hashed, timed and (for `rec_testapp`) checked
against the barcode and the colour bars, and the summary is printed and logged. Nothing is written to
disk. Test aids: `--record-for S`, `--save-frame PNG [--save-frame-index N]`, `rec_testapp --fullscreen`.

## Recorder M3

**D-076 — Pipeline.** Receiver thread (the session's) → timeline → RCV1 encoder (NV12 input, straight
from the ring slot: no copy; the codec's own pool gives 2 threads on a 4-thread CPU) → packet arena
(`record.queue_mb`, default 256 MB, a byte ring in which every packet is contiguous) → writer thread
(above normal priority) → AVI writer → disk writer (two 8 MB sector-aligned buffers, unbuffered and
overlapped, one write in flight while the next fills). The receiver blocks up to 1 s when the arena is
full, then drops the frame (W3104, a DROP row in the telemetry) so the ring doesn't back up into the
game; the real answer to a slow disk is the rate controller (M5). `--encoder hw` and `--format rgb`
are refused with a message naming the milestone that adds them (M9, M4).

**D-077 — Timeline.** The first captured frame's tick is the start of the video (there is nothing to
repeat before it); every later tick gets exactly one output frame. A frame whose tick is ahead of the
next expected one is preceded by DUPs for the missing ticks; a frame whose tick was filled already
arrived too late and is dropped (W2301). A stall (no frame for 250 ms) is filled with DUPs by the host
on its own, but only ticks older than 150 ms, because frames reach the host up to ~100 ms after their
tick; stopping pads DUPs to the stop tick. The codec's own "identical frame" DUPs are counted apart
from the filled ones.

**D-078 — Frames lost to stall latency.** The hook reads finished GPU copies back only when the game
calls Present, and a copy finishes about 5 Presents (80 ms) after it was issued. When the game stops
presenting (a freeze, a loading screen), the last ~5 frames before the stop stay in the pipeline until
it resumes, by which time their ticks have been filled with DUPs, so they are dropped on arrival. The
video therefore freezes ~80 ms before the game did. Serving the read-backs from another thread would
mean using the game's immediate context off its render thread (not thread-safe unless the game turned
on multithread protection), and a second device with shared textures is a much bigger design, so this
stays. `rec verify --testapp` counts these frames apart ("lost to stall latency", at most 8, only just
after a run of 15 or more DUPs) from real losses.

**D-079 — Lock mode.** The pacer runs in the Present hook (plan §6.2): before capturing it holds the
game until the next tick (a high-resolution waitable timer for the bulk, a spin for the last 0.2 ms);
the hold is not counted as the hook's cost. Two changes to the plan's rule, both from measurement:
(1) the tick grid starts at the first captured frame (the plan's grid starts when F9 is pressed): with
the plan's rule a game running at exactly the recording rate, entering at a bad phase, stays late by that
phase for as long as it runs (p99 pacing error up to 15 ms in short recordings), because the pacer only
ever waits for early frames; (2) the grid follows the game's phase by half the lateness of each late
frame, never earlier, never more than one tick from where it started, so the game ends up arriving just
ahead of the grid and is held for the difference, while the video stays tied to the wall clock (without
the cap a game slower than the grid slowed the whole timeline down: 551 frames in 12 s instead of 720).
The tick is decided after the wait, so an oversleeping timer (seen: 65 ms once) or a hitch gives DUPs,
not a frame stamped with the wrong tick. The host follows the grid through `ControlBlock::grid0_qpc`
for its stall and stop arithmetic. Free mode (`--no-lock`) is M2's rule on the F9-time grid.

**D-080 — AVI OpenDML.** Header region (avih, strl with strh/strf+extradata/indx super index,
odml/dmlh, INFO with ISFT and ICMT carrying "game= source=WxH fps=", JUNK) padded to a multiple of 4096
bytes and rewritten once, when the recording ends; further RIFF 'AVIX' blocks every ~1 GB with their
own ix00 index; super index capacity 256 blocks; legacy idx1 for the first block; AVIIF_KEYFRAME from
the codec's `is_keyframe`. The disk writer reopens the file with a second synchronous handle for
read-modify-write of sectors already on disk (header at the end; the RIFF and LIST sizes of a finished
block), allocates 1 GiB ahead, and truncates to the real length at close. The periodic index flush for
crash safety (§11.3) is M7.

**D-081 — Slow writes.** The plan's 50 ms (W4101) and 250 ms (E4102) thresholds describe a write; but
an 8 MiB write at the 100–150 MB/s this laptop's disk does takes 55–80 ms, so the fixed thresholds
warned about every healthy write. A write now counts as slow only if it also ran at less than three
times the rate the recording produces data (after the first second of the recording): a disk stalling
to 25 MB/s while the recording needs 10 MB/s is reported, a disk delivering 100 MB/s is not. The
thresholds stay configurable (`log.slow_write_ms`, `log.very_slow_write_ms`).

**D-082 — Telemetry CSV.** The plan's 19 columns in the plan's order, plus `pacing_error_ms` (how late
after its tick the capture began, lock mode) appended. `present_qpc_us` is microseconds since tick 0
of the grid at the first frame. `map_copy_ms` is empty (not measured per frame), `convert_ms` is 0
(NV12 needs no host conversion), `audio_drift_ms` is empty until M6. `write_latency_ms` is the latency of
the write that carried the frame's bytes; `ring_fill_pct` and `packet_queue_pct` are the fills when the
frame was handled. Frames dropped by the host get a DROP row without an index. CSV rows are written
by the writer thread in blocks.

**D-083 — Summary JSON.** §10.5's fields, plus the file name, the grid and pacing figures; events are
the warnings and errors logged during the recording, by code. `seconds_at_rate_level` is all level 0
until the rate controller (M5); the audio fields are placeholders until M6.

**D-084 — verify and convert.** `rec verify` walks the RIFF structure and checks avih/strh/dmlh
counts, the super index, every ix00 against the chunks, idx1 against the first block; decodes every
frame (key flags must match the packets, the first frame must be an I-frame); with `--testapp` reads
the barcode and counts missing / repeated / out-of-order frames and DUPs that change the picture; and
reads the `.frames.csv` beside the file (ticks consecutive, capture lateness). `rec convert` pipes the
decoded I420 frames to `ffmpeg -f rawvideo -pix_fmt yuvj420p` (full range) and writes H.264 in limited
range, BT.601 tagged, at the file's frame rate (so the DUP-filled timeline survives): `--to mp4|mkv
--crf N --out PATH`. Video only until M6.

**D-085 — Files.** `<Game> YYYY-MM-DD HH-MM-SS-cc.avi` plus `.frames.csv`, `.summary.json`, `.log`
in `record.out_dir`; the name is reserved by creating the .avi exclusively, because two recordings
starting in the same hundredth of a second (two games, two rec instances) once chose the same name.
Frame hashing and the test-pattern checks of M2 now run only for `rec_testapp` (they cost host CPU).

**D-086 — Recordings are written uncompressed on NTFS-compressed drives.** On the development laptop the
whole C: drive is NTFS-compressed (the root has the Compressed attribute, so every folder and new file
inherits it). RCV1 video is compressed already, and NTFS compressing it again in the kernel on every write
cost most of the disk's speed: the same noisy test video (5.3:1, the compression Minecraft gets) was written
at 27 MB/s with 26 very-slow writes (E4102) per 15 s into a compressed folder and at 390 MB/s with none into
one with compression off; flat test content (9:1) hid it (127 MB/s) because NTFS compresses redundant data
quickly. The first fullscreen Minecraft recording on this machine lost 34 frames (W1202) to encoder stalls
of 50–116 ms with the packet queue empty, while a user-mode CPU hog (3 busy processes) could not reproduce
them: the kernel compression work is the likely cause. Now the disk writer switches compression off on every
file it creates (`FSCTL_SET_COMPRESSION`, COMPRESSION_FORMAT_NONE, before the first write) and logs that it
did; `rec doctor` states it; a file in an EFS-encrypted folder is warned about (not changed). The small
companion files (csv, json, log) are left as the folder has them.

**D-087 — Frame ring depth.** `record.frame_slots` now defaults to 16 (was the plan's 8): 267 ms of frames
at 60 fps instead of 133 ms, 22 MB at 720p. The host's encoder can fall behind for a while (a 116 ms stall
used up the old ring); the extra slots cost memory only and nothing for the game.

**D-088 — Encoder priority and power throttling.** The second fullscreen Minecraft recording (disk fixed,
230-488 MB/s) still dropped 92 frames: encode time rose to 10-29 ms average (max 191 ms) for ~12 s while the
game's frame times stayed normal and the packet queue was empty, so the host's encoder threads were being
starved of CPU by the foreground game. The encoder worker threads and the receiver thread now run at
above-normal priority (`record.encoder_priority`, `normal` | `above-normal`, default above-normal; the host
needs about a quarter of one core at 720p60, so it cannot hurt the game noticeably), and `rec.exe` and those
threads opt out of Windows power throttling (EcoQoS), which can otherwise lower a background process's
priority and clock. Verified with the stress script (4 busy processes at above-normal priority on 4 logical
CPUs: encode avg 6.3 vs 5.5 ms unloaded, 0 frames filled). Not verified on the real game yet. If it is still
not enough, the answer is the M5 rate controller: drop frames on purpose (as DUPs) when the CPU is
overloaded, instead of losing them at random.

**D-089 — Capture split into an API-independent core and a backend.** The ring, the lock-mode pacer, the
copy worker and the failure handling moved out of `capture_d3d11.cpp` into `capture_core.cpp`
(`core_on_present`, `core_acquire_slot`, `core_publish`, ...). A backend (`Backend` in `capture_core.h`) only
issues the GPU work and moves finished read-backs along: `service()`, `submit()`, `pending()`, `release()`.
Direct3D 11 and OpenGL are two such backends, so the pacing rules (D-079) exist once. The D3D11 path was
re-verified after the move (35 unit tests, a 10 s test-app recording: pacing, hook cost and colours as
before). One recording runs on one backend (`Core::owner`).

**D-090 — OpenGL capture** (plan §5.2). Inside `wglSwapBuffers` / `SwapBuffers`, before the game's swap: save
the state we touch, blit the default framebuffer into our own RGBA8 renderbuffer FBO at the output size
(the blit scales with GL_LINEAR, letterboxes, and flips the rows, which also fixes GL's bottom-left origin),
`glReadPixels` as BGRA into a pixel-pack buffer, `glFenceSync`, restore. The oldest buffer whose fence is
signalled (timeout 0) is mapped and copied by the copy worker, unmapped and published one Present later: the
same two-stage scheme as D3D11, nothing ever waits for the GPU. Details that matter:
- The blit is made safe against the game's state: scissor test off, colour mask all on, GL_FRAMEBUFFER_SRGB
  off (an sRGB default framebuffer would otherwise be decoded and darken the picture), pack store parameters
  tight; all put back. A multisampled default framebuffer is resolved into a same-size renderbuffer first
  (a multisampled source cannot be scaled in the same blit). The default framebuffer's sample count is
  queried with both framebuffer bindings at 0 (it is the draw framebuffer's).
- Renderbuffers, not textures: no texture-unit binding to disturb. The letterbox bars are cleared once, when
  the target is made, with `glClearBufferfv` (does not touch the game's clear colour).
- GL 3.0+ is decided by the functions being there (`wglGetProcAddress` / opengl32 exports), not by the version
  string: some 2.1 contexts expose ARB_framebuffer_object. A missing function gives E1206 and capture stays off.
- The hook never links opengl32.lib (that would load opengl32.dll into every Direct3D game): everything comes
  from `GetProcAddress` / `wglGetProcAddress`. The source size is the window's client rectangle.
- A different context current at a swap (I1207) abandons the old objects and makes new ones. At detach the
  game's context is not current on our thread, so the objects are abandoned, not deleted (a few MB of GPU
  memory until the process ends, once per recording that was running at detach).
- Known cost: the first captured frame takes 16–34 ms (driver work for the renderbuffer and six 3.7 MB
  pixel-pack buffers, 5.5 ms on D3D11): a one-frame hitch at the start of a recording.

**D-091 — Hooks per API group, and which API wins.** `try_install` hooks Direct3D 10/11/12 when dxgi.dll and
d3d11.dll are loaded and OpenGL when opengl32.dll is; each group once, polled every 100 ms (plus the loader
notification for an early wake-up), so a game that loads its API late, or loads both, is covered. The hook
state is Hooked as soon as one group is. When a process presents through both (a GL game with a D3D overlay),
the first to present is the backend; another API takes over only if the owner has been silent for a second
(`other_api_active`). `SwapBuffers` (gdi32) and `wglSwapBuffers` (opengl32) are both hooked because GLFW, SDL
and LWJGL call the first and older code the second; the TLS depth counter makes a nested call count once.
The statistics and cost accounting both hooks share are in `measure.cpp`.

**D-092 — Host BGRA → NV12.** OpenGL slots are BGRA (`Layout::Bgra`, `bgra_slot_bytes`); the receiver converts
each to NV12 into a preallocated buffer before the encoder (the codec's YUV mode takes I420 or NV12 only).
`bgra_to_nv12` is integer arithmetic with 15-bit BT.601 full-range coefficients (the shader's formula), chroma
from the sum of each 2x2 block, clamped; AVX2 with a scalar fallback that gives the same bytes (tested for
equality on random data of several even sizes and against the float formula to within 1 level). 0.5 ms for
1280x720 in a benchmark, ~1 ms in a recording (cold caches); the plan's target was 1.5 ms. The cost is in the
`convert_ms` column of the telemetry and in the summary. `--format rgb` (codec GBR mode, no conversion) is
still not implemented: the message now says so instead of pointing at M4.

**D-093 — Test app and script.** `rec_testapp --gl` draws the same barcode and colour bars with
`glScissor` + `glClear`; `--wgl`, `--core` (3.3 core profile), `--msaa N`, `--state-check` (sets unusual GL
state just before every swap and exits with an error if the capture left any of it changed: scissor box, colour
mask, pack parameters, framebuffer sRGB, the default framebuffer's read buffer, pack-buffer and framebuffer
bindings) and `--offscreen` (a visible window parked at -20000,-20000 and never activated: the OpenGL tests
need a real client area, a minimised window has none, and they must not disturb a game the user is playing).
d3d11.dll, dxgi.dll and opengl32.dll are all delay-loaded, so a Direct3D run never loads opengl32.dll and an
OpenGL run never loads Direct3D. `tests/m4_opengl.ps1` runs the checks.

**D-094 — `rec bench-disk` and its cache.** The benchmark writes with the recorder's own writer (`DiskFile`, 8 MiB
unbuffered overlapped writes, incompressible data) and reports the sustained speed (bytes over wall time, first
write issued to last finished), the p99 and the worst latency of one write. The result is cached per volume
(keyed by the volume serial number) in `%LOCALAPPDATA%\rec\diskbench.toml`, not in `rec.toml` as the plan says:
`rec config reset` and hand edits of the config should not lose a measurement, and the config is for settings.
The environment variable `REC_DATA_DIR` replaces `%LOCALAPPDATA%\rec` for the config, logs and cache, so tests
and scripts leave the user's own files alone. Startup check (§9): the needed rate is the plan's estimate
(`W x H x 1.5 x fps / 2.49`); if the cached speed is under 1.2x of it, W4001 is logged when the recording starts.
`rec doctor` shows the cached figure (PASS, or WARN under 1.2x, or WARN "not measured").

**D-095 — Rate controller** (plan §9; `rate.cpp`, pure logic, 8 unit tests; `pipeline.cpp` applies it).
Levels from the packet-queue fill: 0 lossless, 1-3 NEAR 1-3 at 40/60/75%, 4 = replace incoming frames with DUPs
at 90% until the queue is under 75%. Up at once; down one level at a time after 2 s below the level's entry
threshold. `rcv-strict`: no levels 1-3 (lossless, or drop). CPU: the frame ring at 50% or more, or the average
encode time over 80% of the frame budget *with the ring at 25% or more*, starts dropping (never NEAR: it costs
more CPU) until the ring is under 25%. The ring guard on the encode-time rule is my addition: with an empty ring
the host is keeping up, and dropping frames then would only lose video. A dropped frame is a DUP written by the
next frame that gets through (or by the end-of-recording padding), counted as `dropped_rate`, shown as a DROP row in
the telemetry; the summary has the seconds spent at each level (`seconds_at_rate_level`). W3101 on a level
change, W3102 when a CPU overload starts (at most once a second, with the number suppressed), W3103 when one
frame takes longer than the frame interval (same rate limit). The status line names the level.

**D-096 — Fault injection** (plan §14.3), all `--debug-*` options of `launch` / `attach`, never in `rec.toml`
(`Config::Debug`), and the console says "FAULT INJECTION" when any is on. Host side: `--debug-throttle-disk MB/s`
and `--debug-write-stall MS EVERY_S` live in `DiskFile` (a simulated disk that is busy until bytes / speed after
the write was submitted, so the writer, and through it the packet queue, behaves as with a real slow disk and the
latencies it reports are the simulated ones), `--debug-encoder-delay MS` sleeps inside the timed part of the
encode, `--debug-fill-disk` makes free space 8 GB minus 1 GB per second, `--debug-no-rate-control` switches the
controller off (diagnosis, and tests that feed the ring in bursts). Hook side, through new `ControlBlock`
fields set before the hook is injected: `--debug-drop-readback N` (one finished read-back in N is dropped and
counted as W1201), `--debug-hook-throw` (a Windows exception inside the guard of the Present hook: E1107, hooks
off, and the host is told capture failed so the recording is finished cleanly), `--debug-device-removed`
(E1209 once, 2 s in; the backend drops everything on the device and makes it again), `--debug-kiero-fail
d3d11|opengl` (E1105 for that API's lookup, the other group installs normally). Protocol version unchanged: the
fields came out of the reserved words.

**D-097 — Monitors** (`monitor.cpp`, once a second from the receiver thread). Free space on the output drive:
W4103 below `log.low_space_gb` (once), E4104 below `log.critical_space_gb`, which stops the recording and
finishes the file (reported to the console through `take_error` like a capture failure). Power: W6101 when AC /
battery / battery saver changes. W6102 when the whole machine is over 95% for 3 s (game and rec shares in the
message, at most every 10 s). W1203 (hook cost over `log.slow_hook_ms`) and W1210 (a gap between two frames over 4 T
or 250 ms) are counted per frame and logged once a second. I6002 is the per-second line in the log. Not done:
I4106 (file splitting) belongs to M7 with the FAT32 handling.
