#include "huffman.h"

#include <algorithm>
#include <cstring>

namespace rcv {
namespace {

constexpr int kMaxLen = 12;

// Unlimited Huffman lengths via the two-queue method over leaves sorted by (count, symbol).
// No heap use: at most 256 leaves and 255 internal nodes. Requires n >= 2.
void huffman_lengths(const uint64_t f[256], uint8_t len[256]) {
    uint16_t leaves[256];
    int n = 0;
    for (int s = 0; s < 256; ++s)
        if (f[s]) leaves[n++] = uint16_t(s);
    std::sort(leaves, leaves + n, [f](uint16_t a, uint16_t b) {
        return f[a] != f[b] ? f[a] < f[b] : a < b;
    });

    // Nodes 0..n-1 are the sorted leaves, n..2n-2 internal nodes in creation order.
    uint64_t weight[511];
    int parent[511];
    for (int i = 0; i < n; ++i) weight[i] = f[leaves[i]];
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
    for (int i = 0; i < n; ++i) len[leaves[i]] = depth[i];
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
    uint64_t f[256];
    for (int s = 0; s < 256; ++s) f[s] = hist[s];
    for (;;) {
        std::memset(len, 0, 256);
        huffman_lengths(f, len);
        const uint8_t longest = *std::max_element(len, len + 256);
        if (longest <= kMaxLen) return;
        for (int s = 0; s < 256; ++s)
            if (f[s]) f[s] = (f[s] + 1) >> 1;
    }
}

bool huff_canonical_codes(const uint8_t len[256], uint16_t code[256]) {
    uint32_t next_code[kMaxLen + 1];
    if (!canonical_start(len, next_code)) return false;
    for (int s = 0; s < 256; ++s) code[s] = len[s] ? uint16_t(next_code[len[s]]++) : 0;
    return true;
}

bool huff_build_decode_lut(const uint8_t len[256], HuffDecEntry lut[kLutSize]) {
    uint32_t next_code[kMaxLen + 1];
    if (!canonical_start(len, next_code)) return false;
    for (int s = 0; s < 256; ++s) {
        const int l = len[s];
        if (!l) continue;
        const uint32_t c = next_code[l]++;
        const uint32_t first = c << (kLutBits - l);
        const uint32_t last = (c + 1) << (kLutBits - l);
        for (uint32_t i = first; i < last; ++i) lut[i] = HuffDecEntry{uint8_t(s), uint8_t(l)};
    }
    return true;
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
