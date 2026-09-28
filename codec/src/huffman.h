// Length-limited canonical Huffman coding (codec plan §5.6, Appendix A.3).
#pragma once

#include <cstdint>

namespace rcv {

constexpr int kLutBits = 12;
constexpr int kLutSize = 1 << kLutBits;

struct HuffDecEntry {
    uint8_t sym;
    uint8_t len;
};

// Code lengths (max 12) for a histogram with at least two non-zero bins.
// Deterministic: ties are broken by symbol value, leaves before internal nodes.
void huff_build_lengths(const uint32_t hist[256], uint8_t len[256]);

// Canonical codes from lengths. Returns false unless every length is <= 12 and the
// code is complete (Kraft sum exactly 1, which needs at least two symbols).
bool huff_canonical_codes(const uint8_t len[256], uint16_t code[256]);

// 12-bit peek table. Same validity rules as huff_canonical_codes.
bool huff_build_decode_lut(const uint8_t len[256], HuffDecEntry lut[kLutSize]);

// 128-byte table: byte k = len[2k] | len[2k+1] << 4 (§6.4).
void huff_pack_lengths(const uint8_t len[256], uint8_t out[128]);
void huff_unpack_lengths(const uint8_t in[128], uint8_t len[256]);

}  // namespace rcv
