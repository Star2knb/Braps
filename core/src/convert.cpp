#include "rec/convert.h"

#include <windows.h>

#include <immintrin.h>

#include <algorithm>

#if defined(__clang__)
#define REC_TARGET_AVX2 __attribute__((target("avx2")))
#else
#define REC_TARGET_AVX2
#endif

namespace rec {
namespace {

// 15-bit coefficients; each set sums to 32768 (luma) or 0 (chroma), so grey stays grey.
constexpr int kYr = 9798, kYg = 19235, kYb = 3735;
constexpr int kCbR = -5529, kCbG = -10855, kCbB = 16384;
constexpr int kCrR = 16384, kCrG = -13720, kCrB = -2664;
constexpr int kLumaRound = 1 << 14;
constexpr int kChromaRound = 1 << 16;  // chroma works on the sum of four pixels: 15 bits + 2

inline uint8_t clamp8(int v) { return uint8_t(v < 0 ? 0 : v > 255 ? 255 : v); }

// One chroma pair and its four luma values, for the columns the SIMD loop leaves.
void convert_columns(const uint8_t* row0, const uint8_t* row1, uint32_t x0, uint32_t width, uint8_t* y0, uint8_t* y1, uint8_t* uv) {
    for (uint32_t x = x0; x < width; x += 2) {
        int sum_r = 0, sum_g = 0, sum_b = 0;
        for (int dy = 0; dy < 2; ++dy) {
            const uint8_t* src = dy ? row1 : row0;
            uint8_t* dst = dy ? y1 : y0;
            for (int dx = 0; dx < 2; ++dx) {
                const uint8_t* p = src + size_t(x + uint32_t(dx)) * 4;
                const int b = p[0], g = p[1], r = p[2];
                dst[x + uint32_t(dx)] = uint8_t((kYr * r + kYg * g + kYb * b + kLumaRound) >> 15);
                sum_r += r;
                sum_g += g;
                sum_b += b;
            }
        }
        uv[x] = clamp8(128 + ((kCbR * sum_r + kCbG * sum_g + kCbB * sum_b + kChromaRound) >> 17));
        uv[x + 1] = clamp8(128 + ((kCrR * sum_r + kCrG * sum_g + kCrB * sum_b + kChromaRound) >> 17));
    }
}

REC_TARGET_AVX2 inline __m256i coef(int c) { return _mm256_set1_epi32(c); }  // low 16 bits: the signed coefficient; high 16 bits don't matter

// 16 luma bytes from two vectors of eight 32-bit luma values.
REC_TARGET_AVX2 inline void store_luma(uint8_t* dst, __m256i a, __m256i b) {
    __m256i w = _mm256_packus_epi32(a, b);  // lane 0: a0-3 b0-3, lane 1: a4-7 b4-7
    w = _mm256_permute4x64_epi64(w, 0xD8);  // a0-7 | b0-7
    __m256i bytes = _mm256_packus_epi16(w, w);
    bytes = _mm256_permute4x64_epi64(bytes, 0x08);  // a's eight bytes, then b's
    _mm_storeu_si128(reinterpret_cast<__m128i*>(dst), _mm256_castsi256_si128(bytes));
}

struct Channels {
    __m256i b, g, r;
};

REC_TARGET_AVX2 inline Channels split(__m256i pixels) {
    const __m256i ff = _mm256_set1_epi32(0xFF);
    Channels c;
    c.b = _mm256_and_si256(pixels, ff);
    c.g = _mm256_and_si256(_mm256_srli_epi32(pixels, 8), ff);
    c.r = _mm256_and_si256(_mm256_srli_epi32(pixels, 16), ff);
    return c;
}

REC_TARGET_AVX2 inline __m256i luma(const Channels& c) {
    __m256i acc = _mm256_add_epi32(_mm256_madd_epi16(c.r, coef(kYr)), _mm256_madd_epi16(c.g, coef(kYg)));
    acc = _mm256_add_epi32(acc, _mm256_madd_epi16(c.b, coef(kYb)));
    acc = _mm256_add_epi32(acc, _mm256_set1_epi32(kLumaRound));
    return _mm256_srli_epi32(acc, 15);
}

// Sums of 2x2 pixels for 16 columns: the rows added, then neighbouring columns (hadd), then put in column order.
REC_TARGET_AVX2 inline __m256i sums2x2(__m256i a_row0, __m256i a_row1, __m256i b_row0, __m256i b_row1, __m256i order) {
    const __m256i h = _mm256_hadd_epi32(_mm256_add_epi32(a_row0, a_row1), _mm256_add_epi32(b_row0, b_row1));
    return _mm256_permutevar8x32_epi32(h, order);
}

REC_TARGET_AVX2 void convert_avx2(const uint8_t* bgra, size_t bgra_stride, uint32_t width, uint32_t height, uint8_t* y, size_t y_stride, uint8_t* uv,
                                  size_t uv_stride) {
    const __m256i order = _mm256_setr_epi32(0, 1, 4, 5, 2, 3, 6, 7);
    const __m256i chroma_round = _mm256_set1_epi32(kChromaRound);
    const __m256i c128 = _mm256_set1_epi32(128), c0 = _mm256_setzero_si256(), c255 = _mm256_set1_epi32(255);
    for (uint32_t row = 0; row < height; row += 2) {
        const uint8_t* s0 = bgra + size_t(row) * bgra_stride;
        const uint8_t* s1 = s0 + bgra_stride;
        uint8_t* y0 = y + size_t(row) * y_stride;
        uint8_t* y1 = y0 + y_stride;
        uint8_t* uvr = uv + size_t(row / 2) * uv_stride;
        uint32_t x = 0;
        for (; x + 16 <= width; x += 16) {
            const Channels a0 = split(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(s0 + size_t(x) * 4)));
            const Channels b0 = split(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(s0 + size_t(x) * 4 + 32)));
            const Channels a1 = split(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(s1 + size_t(x) * 4)));
            const Channels b1 = split(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(s1 + size_t(x) * 4 + 32)));
            store_luma(y0 + x, luma(a0), luma(b0));
            store_luma(y1 + x, luma(a1), luma(b1));

            const __m256i sr = sums2x2(a0.r, a1.r, b0.r, b1.r, order), sg = sums2x2(a0.g, a1.g, b0.g, b1.g, order),
                          sb = sums2x2(a0.b, a1.b, b0.b, b1.b, order);
            __m256i cb = _mm256_add_epi32(_mm256_madd_epi16(sr, coef(kCbR)), _mm256_madd_epi16(sg, coef(kCbG)));
            cb = _mm256_add_epi32(cb, _mm256_madd_epi16(sb, coef(kCbB)));
            cb = _mm256_add_epi32(_mm256_srai_epi32(_mm256_add_epi32(cb, chroma_round), 17), c128);
            __m256i cr = _mm256_add_epi32(_mm256_madd_epi16(sr, coef(kCrR)), _mm256_madd_epi16(sg, coef(kCrG)));
            cr = _mm256_add_epi32(cr, _mm256_madd_epi16(sb, coef(kCrB)));
            cr = _mm256_add_epi32(_mm256_srai_epi32(_mm256_add_epi32(cr, chroma_round), 17), c128);
            cb = _mm256_max_epi32(_mm256_min_epi32(cb, c255), c0);
            cr = _mm256_max_epi32(_mm256_min_epi32(cr, c255), c0);
            const __m256i pairs = _mm256_or_si256(cb, _mm256_slli_epi32(cr, 8));  // Cb | Cr << 8 in each 32 bits
            __m256i words = _mm256_packus_epi32(pairs, pairs);
            words = _mm256_permute4x64_epi64(words, 0x08);
            _mm_storeu_si128(reinterpret_cast<__m128i*>(uvr + x), _mm256_castsi256_si128(words));
        }
        if (x < width) convert_columns(s0, s1, x, width, y0, y1, uvr);
    }
}

bool detect_avx2() { return IsProcessorFeaturePresent(PF_AVX2_INSTRUCTIONS_AVAILABLE) != 0; }

}  // namespace

bool bgra_to_nv12_uses_avx2() {
    static const bool avx2 = detect_avx2();
    return avx2;
}

void bgra_to_nv12_scalar(const uint8_t* bgra, size_t bgra_stride, uint32_t width, uint32_t height, uint8_t* y, size_t y_stride, uint8_t* uv,
                         size_t uv_stride) {
    for (uint32_t row = 0; row < height; row += 2) {
        const uint8_t* s0 = bgra + size_t(row) * bgra_stride;
        convert_columns(s0, s0 + bgra_stride, 0, width, y + size_t(row) * y_stride, y + size_t(row + 1) * y_stride, uv + size_t(row / 2) * uv_stride);
    }
}

void bgra_to_nv12(const uint8_t* bgra, size_t bgra_stride, uint32_t width, uint32_t height, uint8_t* y, size_t y_stride, uint8_t* uv,
                  size_t uv_stride) {
    if (bgra_to_nv12_uses_avx2())
        convert_avx2(bgra, bgra_stride, width, height, y, y_stride, uv, uv_stride);
    else
        bgra_to_nv12_scalar(bgra, bgra_stride, width, height, y, y_stride, uv, uv_stride);
}

}  // namespace rec
