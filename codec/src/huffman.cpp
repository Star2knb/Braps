#include "huffman.h"

#include <stdlib.h>  // _byteswap_uint64

#include <algorithm>
#include <cstring>

namespace rcv {

#include "huff_write_impl.inl"

namespace {

constexpr int kMaxLen = 12;

// Unlimited Huffman lengths via the two-queue method. `keys` are (count << 8 | symbol), sorted
// ascending, i.e. by count then symbol. No heap use: at most 256 leaves and 255 internal nodes.
// Requires n >= 2. Returns the longest length.
int huffman_lengths(const uint64_t* keys, int n, uint8_t len[256]) {
    // Nodes 0..n-1 are the sorted leaves, n..2n-2 internal nodes in creation order.
    uint64_t weight[511];
    int parent[511];
    for (int i = 0; i < n; ++i) weight[i] = keys[i] >> 8;
    int leaf = 0, inner = n, next = n;
    auto pick = [&]() {
        if (leaf < n && (inner >= next || weight[leaf] <= weight[inner])) return leaf++;
        return inner++;
    };
    while (next < 2 * n - 1) {
        const int a = pick();
        const int b = pick();
        weight[next] = weight[a] + weight[b];
        parent[a] = next;
        parent[b] = next;
        ++next;
    }

    // Parents always have a higher index than their children.
    uint8_t depth[511];
    depth[2 * n - 2] = 0;
    for (int i = 2 * n - 3; i >= 0; --i) depth[i] = uint8_t(depth[parent[i]] + 1);
    int longest = 0;
    for (int i = 0; i < n; ++i) {
        len[keys[i] & 0xFF] = depth[i];
        if (depth[i] > longest) longest = depth[i];
    }
    return longest;
}

// Fills next_code[L] with the first canonical code of each length; false if invalid.
bool canonical_start(const uint8_t len[256], uint32_t next_code[kMaxLen + 1]) {
    int bl_count[kMaxLen + 1] = {};
    for (int s = 0; s < 256; ++s) {
        if (len[s] > kMaxLen) return false;
        bl_count[len[s]]++;
    }
    uint32_t kraft = 0;
    for (int l = 1; l <= kMaxLen; ++l) kraft += uint32_t(bl_count[l]) << (kMaxLen - l);
    if (kraft != (1u << kMaxLen)) return false;

    // Same as the plan's "sort by (length, symbol), code <<= len - prev_len, code++" (§5.6.4).
    uint32_t code = 0;
    bl_count[0] = 0;
    for (int l = 1; l <= kMaxLen; ++l) {
        code = (code + uint32_t(bl_count[l - 1])) << 1;
        next_code[l] = code;
    }
    return true;
}

}  // namespace

void huff_build_lengths(const uint32_t hist[256], uint8_t len[256]) {
    // Sort key (count << 8 | symbol) orders by count, then symbol (plan Appendix A.3).
    uint64_t keys[256];
    int n = 0;
    for (int s = 0; s < 256; ++s)
        if (hist[s]) keys[n++] = (uint64_t(hist[s]) << 8) | uint64_t(s);
    std::sort(keys, keys + n);
    for (;;) {
        std::memset(len, 0, 256);
        if (huffman_lengths(keys, n, len) <= kMaxLen) return;
        // Too long: halve every count (keeping it >= 1) and rebuild. Halving keeps the order
        // except that new ties must be re-ordered by symbol, so insertion sort is almost linear.
        for (int i = 0; i < n; ++i) keys[i] = ((((keys[i] >> 8) + 1) >> 1) << 8) | (keys[i] & 0xFF);
        for (int i = 1; i < n; ++i) {
            const uint64_t k = keys[i];
            int j = i;
            for (; j > 0 && keys[j - 1] > k; --j) keys[j] = keys[j - 1];
            keys[j] = k;
        }
    }
}

bool huff_canonical_codes(const uint8_t len[256], uint16_t code[256]) {
    uint32_t next_code[kMaxLen + 1];
    if (!canonical_start(len, next_code)) return false;
    for (int s = 0; s < 256; ++s) code[s] = len[s] ? uint16_t(next_code[len[s]]++) : 0;
    return true;
}

bool huff_build_decode_lut(const uint8_t len[256], HuffDecTable* lut) {
    uint32_t next_code[kMaxLen + 1];
    if (!canonical_start(len, next_code)) return false;
    for (int s = 0; s < 256; ++s) {
        const int l = len[s];
        if (!l) continue;
        const uint32_t c = next_code[l]++;
        const uint32_t first = c << (kLutBits - l);
        const uint32_t n = 1u << (kLutBits - l);
        std::memset(lut->sym + first, s, n);
        std::memset(lut->len + first, l, n);
    }
    return true;
}

void huff_make_enc_table(const uint8_t len[256], const uint16_t code[256], HuffEncTable* t) {
    for (int s = 0; s < 256; ++s) {
        t->len[s] = len[s];
        t->code[s] = len[s] ? uint64_t(code[s]) << (64 - len[s]) : 0;
    }
}

size_t huff_write_scalar(const uint8_t* syms, size_t count, const HuffEncTable& t, uint8_t* dst, uint8_t* limit) {
    return huff_write_impl(syms, count, t, dst, limit);
}

void huff_pack_lengths(const uint8_t len[256], uint8_t out[128]) {
    for (int k = 0; k < 128; ++k) out[k] = uint8_t((len[2 * k] & 0xF) | ((len[2 * k + 1] & 0xF) << 4));
}

void huff_unpack_lengths(const uint8_t in[128], uint8_t len[256]) {
    for (int k = 0; k < 128; ++k) {
        len[2 * k] = uint8_t(in[k] & 0xF);
        len[2 * k + 1] = uint8_t(in[k] >> 4);
    }
}

}  // namespace rcv
