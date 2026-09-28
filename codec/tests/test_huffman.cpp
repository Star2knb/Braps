#include <algorithm>
#include <vector>

#include "bitio.h"
#include "huffman.h"
#include "testfw.h"

using namespace rcv;

namespace {

uint32_t kraft_sum(const uint8_t len[256]) {
    uint32_t k = 0;
    for (int s = 0; s < 256; ++s)
        if (len[s]) k += 1u << (12 - len[s]);
    return k;
}

// Checks length limit, completeness, canonical ordering and a full encode/decode round trip.
void check_table(const uint32_t hist[256], tf::Rng& rng) {
    uint8_t len[256];
    huff_build_lengths(hist, len);
    for (int s = 0; s < 256; ++s) {
        CHECK(len[s] <= 12);
        CHECK((len[s] != 0) == (hist[s] != 0));
    }
    CHECK(kraft_sum(len) == 4096);

    uint16_t code[256];
    REQUIRE(huff_canonical_codes(len, code));
    // Canonical: ordered by (length, symbol), left-aligned codes strictly increase.
    std::vector<int> syms;
    for (int s = 0; s < 256; ++s)
        if (len[s]) syms.push_back(s);
    std::sort(syms.begin(), syms.end(), [&](int a, int b) { return len[a] != len[b] ? len[a] < len[b] : a < b; });
    for (size_t i = 1; i < syms.size(); ++i) {
        const uint32_t prev = uint32_t(code[syms[i - 1]]) << (12 - len[syms[i - 1]]);
        const uint32_t cur = uint32_t(code[syms[i]]) << (12 - len[syms[i]]);
        CHECK(prev < cur);
    }

    uint8_t packed[128], unpacked[256];
    huff_pack_lengths(len, packed);
    huff_unpack_lengths(packed, unpacked);
    CHECK(std::equal(len, len + 256, unpacked));

    // Encode random symbols drawn from the used set, decode through the LUT.
    std::vector<uint8_t> msg(5000);
    for (auto& m : msg) m = uint8_t(syms[rng.below(uint32_t(syms.size()))]);
    std::vector<uint8_t> buf(msg.size() * 2 + 16);
    BitWriter bw(buf.data());
    for (uint8_t m : msg) bw.put(code[m], len[m]);
    const size_t bytes = bw.finish();
    static HuffDecEntry lut[kLutSize];
    REQUIRE(huff_build_decode_lut(len, lut));
    BitReader br(buf.data(), buf.data() + bytes);
    for (uint8_t m : msg) {
        if (br.count() < 12) br.refill();
        const HuffDecEntry e = lut[br.peek12()];
        CHECK(e.sym == m);
        CHECK(br.consume(e.len));
    }
}

}  // namespace

TEST_CASE("huffman: two symbols") {
    tf::Rng rng(2);
    uint32_t hist[256] = {};
    hist[7] = 1000;
    hist[200] = 1;
    check_table(hist, rng);
}

TEST_CASE("huffman: uniform 256 symbols gives 8-bit codes") {
    tf::Rng rng(3);
    uint32_t hist[256];
    for (auto& h : hist) h = 77;
    uint8_t len[256];
    huff_build_lengths(hist, len);
    for (int s = 0; s < 256; ++s) CHECK(len[s] == 8);
    check_table(hist, rng);
}

TEST_CASE("huffman: Fibonacci histogram is length-limited to 12") {
    tf::Rng rng(4);
    uint32_t hist[256] = {};
    uint32_t a = 1, b = 1;
    for (int s = 0; s < 30; ++s) {
        hist[s] = a;
        const uint32_t c = a + b;
        a = b;
        b = c;
    }
    check_table(hist, rng);
}

TEST_CASE("huffman: random histograms") {
    tf::Rng rng(5);
    for (int iter = 0; iter < 300; ++iter) {
        uint32_t hist[256] = {};
        const int used = 2 + int(rng.below(255));
        for (int i = 0; i < used; ++i) {
            const uint32_t shape = rng.below(4);
            const uint32_t v = shape == 0 ? 1 + rng.below(3)
                             : shape == 1 ? 1 + rng.below(1000)
                             : shape == 2 ? 1u << rng.below(24)
                                          : 1 + rng.below(100000);
            hist[rng.below(256)] += v;
        }
        int nonzero = 0;
        for (uint32_t h : hist) nonzero += h != 0;
        if (nonzero < 2) continue;
        check_table(hist, rng);
    }
}

TEST_CASE("huffman: deterministic output") {
    tf::Rng rng(6);
    uint32_t hist[256] = {};
    for (int i = 0; i < 256; ++i) hist[i] = 1 + rng.below(50);  // many ties
    uint8_t a[256], b[256];
    huff_build_lengths(hist, a);
    huff_build_lengths(hist, b);
    CHECK(std::equal(a, a + 256, b));
}

TEST_CASE("huffman: invalid tables are rejected") {
    static HuffDecEntry lut[kLutSize];
    uint8_t len[256] = {};
    CHECK(!huff_build_decode_lut(len, lut));          // empty
    len[0] = 1;
    CHECK(!huff_build_decode_lut(len, lut));          // one symbol: incomplete
    len[1] = 1;
    CHECK(huff_build_decode_lut(len, lut));           // complete
    len[2] = 1;
    CHECK(!huff_build_decode_lut(len, lut));          // over-full
    uint8_t long_len[256] = {};
    long_len[0] = 1;
    long_len[1] = 13;
    CHECK(!huff_build_decode_lut(long_len, lut));     // > 12
}
