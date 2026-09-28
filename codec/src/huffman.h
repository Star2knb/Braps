// Length-limited canonical Huffman coding (codec plan §5.6, Appendix A.3).
#pragma once

#include <cstddef>
#include <cstdint>

namespace rcv {

constexpr int kLutBits = 12;
constexpr int kLutSize = 1 << kLutBits;

// Decode table indexed by the next 12 bits: separate arrays so each lookup is a single byte load.
struct HuffDecTable {
    uint8_t sym[kLutSize];
    uint8_t len[kLutSize];
};

// Code lengths (max 12) for a histogram with at least two non-zero bins.
// Deterministic: ties are broken by symbol value, leaves before internal nodes.
void huff_build_lengths(const uint32_t hist[256], uint8_t len[256]);

// Canonical codes from lengths. Returns false unless every length is <= 12 and the
// code is complete (Kraft sum exactly 1, which needs at least two symbols).
bool huff_canonical_codes(const uint8_t len[256], uint16_t code[256]);

// 12-bit peek table. Same validity rules as huff_canonical_codes (a complete code fills every entry).
bool huff_build_decode_lut(const uint8_t len[256], HuffDecTable* lut);

// Encoder table: each code left-aligned in a 64-bit word (top `len` bits), plus its length.
struct HuffEncTable {
    uint64_t code[256];
    uint8_t len[256];
};
void huff_make_enc_table(const uint8_t len[256], const uint16_t code[256], HuffEncTable* t);

// Fast MSB-first Huffman writer; output is byte-identical to BitWriter. It stores 8 bytes at a
// time, so it may write (garbage) anywhere in [dst, limit), but never past `limit`; the caller
// guarantees the bitstream itself ends at or before `limit`. Returns the bitstream's byte length
// (last byte zero-padded). Same source compiled twice: baseline, and AVX2 level (BMI2 shifts).
using HuffWriteFn = size_t (*)(const uint8_t* syms, size_t count, const HuffEncTable& t, uint8_t* dst,
                               uint8_t* limit);
size_t huff_write_scalar(const uint8_t* syms, size_t count, const HuffEncTable& t, uint8_t* dst, uint8_t* limit);
size_t huff_write_avx2(const uint8_t* syms, size_t count, const HuffEncTable& t, uint8_t* dst, uint8_t* limit);

// 128-byte table: byte k = len[2k] | len[2k+1] << 4 (§6.4).
void huff_pack_lengths(const uint8_t len[256], uint8_t out[128]);
void huff_unpack_lengths(const uint8_t in[128], uint8_t len[256]);

}  // namespace rcv
