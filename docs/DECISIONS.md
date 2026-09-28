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
reconstructed. Simpler to validate; fusing the passes is an M3 optimisation.

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
