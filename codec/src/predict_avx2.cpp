// AVX2 lossless residual kernel: must equal residuals_lossless_scalar byte for byte.
// Compiled with /arch:AVX2; only reached through the dispatch table after CPUID + XGETBV.
//
// Self-contained on purpose: this file uses no inline functions or templates shared with other
// translation units, so the linker can never pick an AVX2-compiled copy for baseline code.
#include <immintrin.h>

#include "predict.h"

namespace rcv {
namespace {

uint8_t med_tail(uint8_t a, uint8_t b, uint8_t c) {
    const uint8_t mn = a < b ? a : b;
    const uint8_t mx = a < b ? b : a;
    if (c >= mx) return mn;
    if (c <= mn) return mx;
    return uint8_t(a + b - c);
}

// out[i] = x[i] - x[i-1] for i in [1, width).
void left_row(const uint8_t* x, int width, uint8_t* out) {
    int i = 1;
    for (; i + 32 <= width; i += 32) {
        const __m256i cur = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(x + i));
        const __m256i left = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(x + i - 1));
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(out + i), _mm256_sub_epi8(cur, left));
    }
    for (; i + 16 <= width; i += 16) {
        const __m128i cur = _mm_loadu_si128(reinterpret_cast<const __m128i*>(x + i));
        const __m128i left = _mm_loadu_si128(reinterpret_cast<const __m128i*>(x + i - 1));
        _mm_storeu_si128(reinterpret_cast<__m128i*>(out + i), _mm_sub_epi8(cur, left));
    }
    for (; i < width; ++i) out[i] = uint8_t(x[i] - x[i - 1]);
}

// out[i] = x[i] - MED(left, above, above-left) for i in [1, width), select form of §5.4.
void med_row(const uint8_t* x, const uint8_t* up, int width, uint8_t* out) {
    int i = 1;
    for (; i + 32 <= width; i += 32) {
        const __m256i xv = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(x + i));
        const __m256i a = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(x + i - 1));
        const __m256i b = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(up + i));
        const __m256i c = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(up + i - 1));
        const __m256i mn = _mm256_min_epu8(a, b);
        const __m256i mx = _mm256_max_epu8(a, b);
        const __m256i grad = _mm256_sub_epi8(_mm256_add_epi8(a, b), c);  // exact whenever it is selected
        const __m256i c_ge_mx = _mm256_cmpeq_epi8(_mm256_max_epu8(c, mx), c);
        const __m256i c_le_mn = _mm256_cmpeq_epi8(_mm256_min_epu8(c, mn), c);
        __m256i pred = _mm256_blendv_epi8(grad, mx, c_le_mn);
        pred = _mm256_blendv_epi8(pred, mn, c_ge_mx);  // c >= mx wins, as in the scalar order
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(out + i), _mm256_sub_epi8(xv, pred));
    }
    for (; i < width; ++i) out[i] = uint8_t(x[i] - med_tail(x[i - 1], up[i], up[i - 1]));
}

// Row worker without the trailing vzeroupper, shared by the two entry points below.
void row_avx2(const uint8_t* x, const uint8_t* up, int width, int predictor, uint8_t* out) {
    if (!up) {
        out[0] = uint8_t(x[0] - 128);
        left_row(x, width, out);
        return;
    }
    out[0] = uint8_t(x[0] - up[0]);
    if (predictor == kPredMed)
        med_row(x, up, width, out);
    else
        left_row(x, width, out);
}

}  // namespace

void residual_row_avx2(const uint8_t* x, const uint8_t* up, int width, int predictor, uint8_t* out) {
    row_avx2(x, up, width, predictor, out);
    _mm256_zeroupper();  // callers are baseline (SSE) code
}

size_t residuals_lossless_avx2(const uint8_t* plane, ptrdiff_t stride, int width, int row_begin, int row_end,
                               int predictor, uint8_t* out, uint32_t hist[256]) {
    uint8_t* o = out;
    for (int j = row_begin; j < row_end; ++j, o += width) {
        const uint8_t* x = plane + j * stride;
        row_avx2(x, j == row_begin ? nullptr : x - stride, width, predictor, o);
    }
    _mm256_zeroupper();
    const size_t n = size_t(o - out);
    histogram_add(out, n, hist);
    return n;
}

}  // namespace rcv
