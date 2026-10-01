# RCV1 — M8 report (hardening)

**Date:** 2026-10-01 · **Codec state:** M8 — full §6.5 validation, strict reserved fields, SSE4.2
CRC-32C, no-allocation guarantee, bounded packets, decoder fuzzing · **Tools:** `rcv_tests`
(72 tests), `rcv_fuzz` + `rcv_fuzz_seeds` (clang-cl)

## Verdict

| Criterion | Target | Measured | Status |
|---|---|---|---|
| A11 No allocation | zero heap allocations in encode/decode calls | 0, counted by replaced `operator new`/`delete` (all builds) and the debug-CRT allocation hook (every CRT heap allocation, any thread); I/P/DUP/forced-I/near/RAW/SINGLE frames, duplicates, rejected calls, decode to every layout, corrupt packets; 1 and 4 threads, I420/NV12/BGRA, CRC on/off | **met** |
| A12 Robustness | ≥ 1 hour libFuzzer + ASan, no crash, hang or out-of-bounds access | **3,851,941 inputs in 60 min** (ASan + UBSan, 10 s hang limit): no crash, no out-of-bounds access, no hang; coverage 821 → 877 edges | **met** |
| A13 Bounded output | every packet ≤ `rcv_max_packet_size` | 1,000 packets (noise I, half-changed noise P, new noise, flat, DUP) over 10 sizes × slices × predictors × CRC × NEAR: none over, guard bytes after the buffer untouched; lossless noise I-frames are exactly the bound minus the skip map (1360×744 too) | **met** |
| §6.5 validation | reject, never crash | every rule tested on its own (below); plus the fuzzer | **met** |

## Validation (§6.5)

`test_validation.cpp` rebuilds valid packets with one field changed (payload size and directory kept
consistent, so the rule under test is the one that fires):

| Rule | Cases |
|---|---|
| magic, version, frame type | (M1 tests) |
| format | differs from the sequence; unknown value; YUV packet in a GBR stream |
| dimensions | width or height differs from the sequence / reference (I and P) |
| slices | 0, 65 and 255 (and > blocks_y); 64 accepted on a 66-block-row frame |
| NEAR | > 3; flag without NEAR; NEAR without flag; NEAR on GBR |
| payload size | wrong value, trailing byte, shorter than the directory or the skip map |
| directory | sizes not multiples of 4 (same total); below the 4-byte chunk header; sum that wraps 32 bits back to the right total; total too large |
| chunks | unknown mode; reserved bytes; symbol byte on HUFFMAN/RAW; wrong RAW/SINGLE size; EMPTY with samples |
| code tables | over-subscribed; incomplete (one symbol removed, single symbol); complete code with lengths of 13 |
| bitstream | ends early; trailing bytes; table only |
| skip map | padding bit; padding byte — and the reference is still valid afterwards |
| reserved fields (new, D-043) | sequence header colour bit 7, byte 15, bytes 26–31; frame header colour bit 7, bytes 18–19 |

## CRC-32C

SSE4.2 `crc32` path, chosen at load time ([D-042](../DECISIONS.md)): **0.16 ms per MB** against
2.5 ms for the table version (16×). Tested equal to the table for every length 0–300 at every
alignment, and against the standard check value.

## Allocation (A11)

The debug run of the new test found a real problem: worker threads were created in `*_create` but
could still be starting up — the CRT allocates ~2 KB of per-thread data on a thread's first run —
during the first encode or decode call. `ThreadPool::start` now returns only when every worker is
running and its `on_worker_start` hook has returned ([D-044](../DECISIONS.md)); that also guarantees
the recorder's priority/affinity settings are applied before the first frame.

## Fuzzing (A12)

`rcv_fuzz` (clang-debug preset, [D-046](../DECISIONS.md)): libFuzzer + ASan + UBSan (UBSan findings
abort). Each input is a stream — sequence header, options (1–2 decoder threads, output layout,
padded strides), up to 16 packets — so P-frames and DUPs decode against real references. Packets and
output planes are copied into exactly-sized heap blocks so ASan sees any access past them. Seeds:
56 valid streams from `rcv_fuzz_seeds` covering both formats, odd and even sizes, both predictors,
1/default/max slices, NEAR 0–3, CRC, RAW/SINGLE/HUFFMAN chunks and I/P/DUP.

**Run (2026-10-01):** one process at below-normal priority for 3,601 s, continuing the corpus of
an earlier run that a laptop restart stopped after 9 minutes (544,000 inputs, also clean).

| | |
|---|---|
| Inputs executed | 3,851,941 (~1,070 per second) |
| Crashes, ASan/UBSan reports, timeouts (> 10 s) | **none** |
| Coverage | 821 → 877 edges; 1,044 new inputs added (corpus 1,770 files) |
| Peak memory | 875 MB |

One input was reported as slow (10 s), at the moment four compile jobs at normal priority were
saturating all four CPUs while the fuzzer ran at below-normal priority; it uses two decoder
threads, so it waited on a starved worker. Replayed afterwards 5,000 times with a 5 s limit, it takes
1.2 ms every time — CPU starvation, not a hang.

Replaying a saved input: `rcv_fuzz.exe <file>`; a new crash would be written to
`corpusuzz_artifacts\` (README, "Fuzzing").

## Tests

72 tests pass: release with every ISA forced (scalar, SSE4.1, AVX2) and 1 and 4 threads, debug
(with the CRT allocation hook), and clang-cl.
