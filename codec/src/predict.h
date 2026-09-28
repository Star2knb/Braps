// Spatial prediction (codec plan §5.4).
#pragma once

#include <cstddef>
#include <cstdint>

namespace rcv {

enum Predictor : uint8_t { kPredLeft = 0, kPredMed = 1 };

// MED / LOCO-I in its explicit select form (the reference definition).
inline uint8_t predict_med(uint8_t a, uint8_t b, uint8_t c) {
    const uint8_t mn = a < b ? a : b;
    const uint8_t mx = a < b ? b : a;
    if (c >= mx) return mn;
    if (c <= mn) return mx;
    return uint8_t(a + b - c);
}

// The same predictor as clamp(a + b - c, min(a,b), max(a,b)) in full integers: branch-free,
// used by the decoder's serial loop. Tested equal to predict_med for all 2^24 inputs.
inline int predict_med_clamp(int a, int b, int c) {
    const int mn = a < b ? a : b;
    const int mx = a < b ? b : a;
    int g = a + b - c;
    g = g < mn ? mn : g;
    return g > mx ? mx : g;
}

// Lossless residuals for rows [row_begin, row_end) of one plane, using the §5.4 edge rules
// (row_begin is the first row of the slice). Writes one symbol per sample to `out`, adds the
// symbols to `hist`, and returns the number of symbols. All versions give identical output.
using ResidualFn = size_t (*)(const uint8_t* plane, ptrdiff_t stride, int width, int row_begin, int row_end,
                              int predictor, uint8_t* out, uint32_t hist[256]);

size_t residuals_lossless_scalar(const uint8_t* plane, ptrdiff_t stride, int width, int row_begin, int row_end,
                                 int predictor, uint8_t* out, uint32_t hist[256]);
size_t residuals_lossless_sse41(const uint8_t* plane, ptrdiff_t stride, int width, int row_begin, int row_end,
                                int predictor, uint8_t* out, uint32_t hist[256]);
size_t residuals_lossless_avx2(const uint8_t* plane, ptrdiff_t stride, int width, int row_begin, int row_end,
                               int predictor, uint8_t* out, uint32_t hist[256]);

// Adds n symbols to hist using four interleaved sub-histograms (§5.6 step 1). Baseline code,
// deliberately not inline so SIMD translation units call this one copy.
void histogram_add(const uint8_t* syms, size_t n, uint32_t hist[256]);

// Inverse of the residual step: rebuilds the samples in place from `syms` (reference version;
// the decoder uses its own fused loop).
void reconstruct_lossless_scalar(uint8_t* plane, ptrdiff_t stride, int width, int row_begin, int row_end,
                                 int predictor, const uint8_t* syms);

}  // namespace rcv
