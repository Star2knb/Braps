// Temporal block skip (codec plan §5.3) and the P-frame skip map (§6.3).
#pragma once

#include <cstddef>
#include <cstdint>

#include "format.h"
#include "rcv/rcv.h"

namespace rcv {

// Samples of plane p covered by the non-skipped blocks of one block row
// (row_flags: blocks_x flags, 1 = skipped). Edge blocks may be partial.
int coded_row_width(const Geometry& g, int p, const uint8_t* row_flags);

// Coded samples in chunk (plane p, slice s). flags == nullptr means an I-frame (every sample).
// Both encoder and decoder derive chunk sizes from this; they are never stored (§6.4).
size_t coded_chunk_samples(const Geometry& g, int p, int s, const uint8_t* flags);

// Packs blocks_x * blocks_y flags into the §6.3 bitmap (raster order, bit k of byte m is block
// 8m + k, 1 = skipped), zero-padded to skip_map_size(g) bytes.
void pack_skip_map(const Geometry& g, const uint8_t* flags, uint8_t* out);

// Inverse of pack_skip_map. Returns false if any padding bit or byte is non-zero.
bool unpack_skip_map(const Geometry& g, const uint8_t* in, uint8_t* flags);

// Skip test for block row `by`: sets unchanged[b] = 1 for every block whose samples all lie within
// `tolerance` of the reference in all three planes (0 = lossless: exactly equal; NEAR n for
// near-lossless, §5.3), 0 otherwise, and returns the number of unchanged blocks.
// Compares row by row across the whole width (sequential memory access, prefetch-friendly),
// skips blocks already known to differ, and stops as soon as every block differs - on a changing
// frame usually after the first row. SSE2 (x64 baseline).
int compare_block_row(const Geometry& g, const rcv_frame_in* in, rcv_input_layout layout, uint8_t* const ref_plane[3],
                      const ptrdiff_t ref_stride[3], int by, uint8_t* unchanged, int tolerance = 0);

}  // namespace rcv
