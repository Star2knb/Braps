#include "colour.h"

#include <emmintrin.h>

namespace rcv {
namespace {

// Channel `Shift / 8` (0 = B, 1 = G, 2 = R) of 16 BGRA pixels in v0..v3, as 16 bytes.
template <int Shift>
__m128i channel(__m128i v0, __m128i v1, __m128i v2, __m128i v3) {
    const __m128i m = _mm_set1_epi32(0xFF);
    const __m128i a0 = _mm_and_si128(_mm_srli_epi32(v0, Shift), m);
    const __m128i a1 = _mm_and_si128(_mm_srli_epi32(v1, Shift), m);
    const __m128i a2 = _mm_and_si128(_mm_srli_epi32(v2, Shift), m);
    const __m128i a3 = _mm_and_si128(_mm_srli_epi32(v3, Shift), m);
    // Values are 0..255, so the saturating packs are exact.
    return _mm_packus_epi16(_mm_packs_epi32(a0, a1), _mm_packs_epi32(a2, a3));
}

}  // namespace

void bgra_to_gbr_row_scalar(const uint8_t* s, int n, uint8_t* g, uint8_t* bg, uint8_t* rg) {
    for (int x = 0; x < n; ++x) {
        const uint8_t b = s[4 * x], gg = s[4 * x + 1], r = s[4 * x + 2];
        g[x] = gg;
        bg[x] = uint8_t(b - gg);
        rg[x] = uint8_t(r - gg);
    }
}

void bgra_to_gbr_row(const uint8_t* s, int n, uint8_t* g, uint8_t* bg, uint8_t* rg) {
    int x = 0;
    for (; x + 16 <= n; x += 16) {
        const uint8_t* p = s + 4 * x;
        const __m128i v0 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(p));
        const __m128i v1 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(p + 16));
        const __m128i v2 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(p + 32));
        const __m128i v3 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(p + 48));
        const __m128i B = channel<0>(v0, v1, v2, v3);
        const __m128i G = channel<8>(v0, v1, v2, v3);
        const __m128i R = channel<16>(v0, v1, v2, v3);
        _mm_storeu_si128(reinterpret_cast<__m128i*>(g + x), G);
        _mm_storeu_si128(reinterpret_cast<__m128i*>(bg + x), _mm_sub_epi8(B, G));
        _mm_storeu_si128(reinterpret_cast<__m128i*>(rg + x), _mm_sub_epi8(R, G));
    }
    bgra_to_gbr_row_scalar(s + 4 * x, n - x, g + x, bg + x, rg + x);
}

void gbr_to_bgra_row_scalar(const uint8_t* g, const uint8_t* bg, const uint8_t* rg, int n, uint8_t* d) {
    for (int x = 0; x < n; ++x) {
        d[4 * x] = uint8_t(bg[x] + g[x]);
        d[4 * x + 1] = g[x];
        d[4 * x + 2] = uint8_t(rg[x] + g[x]);
        d[4 * x + 3] = 255;
    }
}

void gbr_to_bgra_row(const uint8_t* g, const uint8_t* bg, const uint8_t* rg, int n, uint8_t* d) {
    int x = 0;
    const __m128i A = _mm_set1_epi8(-1);
    for (; x + 16 <= n; x += 16) {
        const __m128i G = _mm_loadu_si128(reinterpret_cast<const __m128i*>(g + x));
        const __m128i B = _mm_add_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(bg + x)), G);
        const __m128i R = _mm_add_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(rg + x)), G);
        const __m128i bg_lo = _mm_unpacklo_epi8(B, G), bg_hi = _mm_unpackhi_epi8(B, G);
        const __m128i ra_lo = _mm_unpacklo_epi8(R, A), ra_hi = _mm_unpackhi_epi8(R, A);
        uint8_t* o = d + 4 * x;
        _mm_storeu_si128(reinterpret_cast<__m128i*>(o), _mm_unpacklo_epi16(bg_lo, ra_lo));
        _mm_storeu_si128(reinterpret_cast<__m128i*>(o + 16), _mm_unpackhi_epi16(bg_lo, ra_lo));
        _mm_storeu_si128(reinterpret_cast<__m128i*>(o + 32), _mm_unpacklo_epi16(bg_hi, ra_hi));
        _mm_storeu_si128(reinterpret_cast<__m128i*>(o + 48), _mm_unpackhi_epi16(bg_hi, ra_hi));
    }
    gbr_to_bgra_row_scalar(g + x, bg + x, rg + x, n - x, d + 4 * x);
}

}  // namespace rcv
