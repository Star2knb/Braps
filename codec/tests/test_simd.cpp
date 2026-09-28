// SIMD equivalence (codec plan §0 rule 2, §11.4) and ISA determinism (A3).
#include <algorithm>
#include <vector>

#include "bitio.h"
#include "cpu.h"
#include "huffman.h"
#include "predict.h"
#include "test_util.h"

using namespace rcv;

namespace {

struct Kernel {
    rcv_isa isa;
    ResidualFn fn;
    const char* name;
};

std::vector<Kernel> available_kernels() {
    std::vector<Kernel> k = {{RCV_ISA_SCALAR, residuals_lossless_scalar, "scalar"}};
    const rcv_isa best = rcv_cpu_isa();
    if (best >= RCV_ISA_SSE41) k.push_back({RCV_ISA_SSE41, residuals_lossless_sse41, "sse41"});
    if (best >= RCV_ISA_AVX2) k.push_back({RCV_ISA_AVX2, residuals_lossless_avx2, "avx2"});
    return k;
}

// Fills a plane; alphabet mode uses a few values so a == b == c ties are frequent.
void fill_plane(std::vector<uint8_t>& p, tf::Rng& rng, bool alphabet) {
    static const uint8_t kAlpha[] = {0, 1, 2, 127, 128, 129, 254, 255};
    for (auto& v : p) v = alphabet ? kAlpha[rng.below(8)] : rng.byte();
}

}  // namespace

TEST_CASE("simd: CPU dispatch reports a usable level") {
    const rcv_isa best = rcv_cpu_isa();
    CHECK(best >= RCV_ISA_SCALAR && best <= RCV_ISA_AVX2);
    std::printf("    (this CPU: %s)\n", best == RCV_ISA_AVX2 ? "AVX2" : best == RCV_ISA_SSE41 ? "SSE4.1" : "scalar");
    rcv_isa r;
    CHECK(resolve_isa(RCV_ISA_AUTO, &r) == RCV_OK && r == best);
    CHECK(resolve_isa(RCV_ISA_SCALAR, &r) == RCV_OK && r == RCV_ISA_SCALAR);
    CHECK(resolve_isa(rcv_isa(9), &r) == RCV_ERR_INVALID_ARG);
}

TEST_CASE("simd: residual kernels equal scalar (widths 1-130, random strides, both predictors)") {
    const std::vector<Kernel> kernels = available_kernels();
    tf::Rng rng(2024);
    for (int alphabet = 0; alphabet < 2; ++alphabet)
        for (int predictor = 0; predictor < 2; ++predictor)
            for (int w = 1; w <= 130; ++w) {
                const int h = 2 + int(rng.below(6));
                const ptrdiff_t stride = w + int(rng.below(40));
                std::vector<uint8_t> plane(size_t(stride) * h);
                fill_plane(plane, rng, alphabet != 0);
                const int r0 = int(rng.below(uint32_t(h - 1)));  // slice may start mid-plane
                const size_t n = size_t(w) * size_t(h - r0);
                std::vector<uint8_t> ref(n + 64, 0xAB);
                uint32_t ref_hist[256] = {};
                CHECK(residuals_lossless_scalar(plane.data(), stride, w, r0, h, predictor, ref.data(), ref_hist) == n);
                for (const Kernel& k : kernels) {
                    std::vector<uint8_t> out(n + 64, 0xAB);
                    uint32_t hist[256] = {};
                    CHECK(k.fn(plane.data(), stride, w, r0, h, predictor, out.data(), hist) == n);
                    CHECK(std::equal(ref.begin(), ref.end(), out.begin()));  // incl. no writes past n
                    CHECK(std::equal(ref_hist, ref_hist + 256, hist));
                }
            }
}

TEST_CASE("simd: histogram_add equals a plain count") {
    tf::Rng rng(5);
    for (size_t n : {size_t(0), size_t(1), size_t(7), size_t(8), size_t(9), size_t(1000), size_t(65537)}) {
        std::vector<uint8_t> s(n);
        for (auto& v : s) v = rng.below(3) ? uint8_t(0) : rng.byte();  // skewed, like residuals
        uint32_t fast[256] = {}, slow[256] = {};
        fast[3] = slow[3] = 10;  // adds to existing counts
        histogram_add(s.data(), n, fast);
        for (uint8_t v : s) slow[v]++;
        CHECK(std::equal(fast, fast + 256, slow));
    }
}

TEST_CASE("huffman: fast writers are byte-identical to BitWriter, never write past limit") {
    std::vector<HuffWriteFn> writers = {huff_write_scalar};
    if (rcv_cpu_isa() >= RCV_ISA_AVX2) writers.push_back(huff_write_avx2);
    tf::Rng rng(77);
    for (int iter = 0; iter < 400; ++iter) {
        uint32_t hist[256] = {};
        const int used = 2 + int(rng.below(255));
        for (int i = 0; i < used; ++i) hist[rng.below(256)] += 1 + (rng.below(4) ? rng.below(10) : rng.below(100000));
        int nonzero = 0;
        for (uint32_t v : hist) nonzero += v != 0;
        if (nonzero < 2) continue;
        uint8_t len[256];
        uint16_t code[256];
        huff_build_lengths(hist, len);
        REQUIRE(huff_canonical_codes(len, code));
        HuffEncTable table;
        huff_make_enc_table(len, code, &table);
        std::vector<int> syms;
        for (int s = 0; s < 256; ++s)
            if (len[s]) syms.push_back(s);

        const size_t count = rng.below(3000);
        std::vector<uint8_t> msg(count);
        size_t bits = 0;
        for (auto& m : msg) {
            m = uint8_t(syms[rng.below(uint32_t(syms.size()))]);
            bits += len[m];
        }
        const size_t bytes = (bits + 7) / 8;
        std::vector<uint8_t> ref(bytes + 16, 0);
        BitWriter bw(ref.data());
        for (uint8_t m : msg) bw.put(code[m], len[m]);
        CHECK(bw.finish() == bytes);

        // Tight limit on even iterations exercises the byte-by-byte tail path.
        const size_t slack = iter % 2 ? 16 : 0;
        for (HuffWriteFn write : writers) {
            std::vector<uint8_t> out(bytes + slack + 8, 0x5A);
            const size_t n = write(msg.data(), count, table, out.data(), out.data() + bytes + slack);
            CHECK(n == bytes);
            CHECK(std::equal(ref.begin(), ref.begin() + ptrdiff_t(bytes), out.begin()));
            for (size_t k = bytes + slack; k < out.size(); ++k) CHECK(out[k] == 0x5A);  // guard bytes untouched
        }
    }
}

TEST_CASE("simd: packets are identical for every ISA (A3) and decode bit-exact") {
    const std::vector<Kernel> kernels = available_kernels();
    const int sizes[][2] = {{2, 2}, {18, 18}, {34, 20}, {130, 66}, {322, 180}, {1360, 744}};
    uint64_t seed = 900;
    for (const auto& sz : sizes)
        for (tu::Content c : tu::kAllContent)
            for (int pred = 0; pred < 2; ++pred) {
                if (sz[0] == 1360 && c != tu::Content::Natural) continue;  // keep the test quick
                const tu::Yuv f = tu::make_yuv(sz[0], sz[1], c, seed++, 3);
                rcv_encoder_config cfg = tu::config_for(f.w, f.h, pred);
                std::vector<uint8_t> first;
                for (const Kernel& k : kernels) {
                    cfg.isa = k.isa;
                    const std::vector<uint8_t> pkt = tu::encode_one(cfg, f);
                    REQUIRE(!pkt.empty());
                    if (first.empty()) first = pkt;
                    CHECK(pkt == first);
                }
                uint8_t seq[32];
                rcv_write_sequence_header(&cfg, seq);
                rcv_decoder* dec = nullptr;
                REQUIRE(rcv_decoder_create(seq, 1, &dec) == RCV_OK);
                tu::Yuv out = tu::blank_like(f);
                CHECK(tu::decode_into(dec, first, out) == RCV_OK);
                CHECK(tu::same_image(f, out));
                rcv_decoder_destroy(dec);
            }
}
