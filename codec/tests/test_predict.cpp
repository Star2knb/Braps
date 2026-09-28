#include <algorithm>
#include <vector>

#include "predict.h"
#include "testfw.h"

using namespace rcv;

TEST_CASE("predict: MED equals median(a, b, a+b-c) and the clamp form for all 2^24 triples") {
    long mismatches = 0, clamp_mismatches = 0;
    for (int a = 0; a < 256; ++a)
        for (int b = 0; b < 256; ++b)
            for (int c = 0; c < 256; ++c) {
                int v[3] = {a, b, a + b - c};  // unclamped integer gradient
                std::sort(v, v + 3);
                const int med = predict_med(uint8_t(a), uint8_t(b), uint8_t(c));
                if (med != v[1]) ++mismatches;
                if (predict_med_clamp(a, b, c) != med) ++clamp_mismatches;  // decoder's form
            }
    CHECK(mismatches == 0);
    CHECK(clamp_mismatches == 0);
}

TEST_CASE("predict: residuals and reconstruction are inverse (both predictors)") {
    tf::Rng rng(10);
    for (int predictor = 0; predictor < 2; ++predictor) {
        for (int w = 1; w <= 70; ++w) {
            const int h = 1 + int(rng.below(9));
            const ptrdiff_t stride = w + int(rng.below(9));
            std::vector<uint8_t> src(size_t(stride) * h), dst(size_t(stride) * h, 0x55);
            for (auto& v : src) v = rng.byte();
            // Encode two "slices" to exercise the first-row rule at a non-zero row.
            const int split = h / 2;
            std::vector<uint8_t> syms(size_t(w) * h);
            uint32_t hist[256] = {};
            size_t n = residuals_lossless_scalar(src.data(), stride, w, 0, split, predictor, syms.data(), hist);
            n += residuals_lossless_scalar(src.data(), stride, w, split, h, predictor, syms.data() + n, hist);
            CHECK(n == size_t(w) * h);
            uint32_t total = 0;
            for (uint32_t c : hist) total += c;
            CHECK(total == n);

            reconstruct_lossless_scalar(dst.data(), stride, w, 0, split, predictor, syms.data());
            reconstruct_lossless_scalar(dst.data(), stride, w, split, h, predictor, syms.data() + size_t(w) * split);
            for (int y = 0; y < h; ++y)
                CHECK(std::equal(src.begin() + y * stride, src.begin() + y * stride + w, dst.begin() + y * stride));
        }
    }
}

TEST_CASE("predict: edge rules") {
    // 2x2 plane: first row uses 128 then left; second row uses above then MED.
    const uint8_t px[4] = {130, 131, 100, 120};
    uint8_t syms[4];
    uint32_t hist[256] = {};
    residuals_lossless_scalar(px, 2, 2, 0, 2, kPredMed, syms, hist);
    CHECK(syms[0] == 2);                                // 130 - 128
    CHECK(syms[1] == 1);                                // 131 - 130
    CHECK(syms[2] == uint8_t(100 - 130));               // above
    CHECK(syms[3] == uint8_t(120 - predict_med(100, 131, 130)));
}
