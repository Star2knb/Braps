// Spatial prediction (codec plan §5.4). Scalar reference kernels.
#pragma once

#include <cstddef>
#include <cstdint>

namespace rcv {

enum Predictor : uint8_t { kPredLeft = 0, kPredMed = 1 };

// MED / LOCO-I in its explicit select form. Do not replace with a wrapping-arithmetic median.
inline uint8_t predict_med(uint8_t a, uint8_t b, uint8_t c) {
    const uint8_t mn = a < b ? a : b;
    const uint8_t mx = a < b ? b : a;
    if (c >= mx) return mn;
    if (c <= mn) return mx;
    return uint8_t(a + b - c);
}

// Lossless residuals for rows [row_begin, row_end) of one plane, using the §5.4 edge rules
// (row_begin is the first row of the slice). Writes one symbol per sample to `out`, adds to
// `hist`, and returns the number of symbols.
size_t residuals_lossless_scalar(const uint8_t* plane, ptrdiff_t stride, int width,
                                 int row_begin, int row_end, int predictor,
                                 uint8_t* out, uint32_t hist[256]);

// Inverse of the above: rebuilds the samples in place from `syms`.
void reconstruct_lossless_scalar(uint8_t* plane, ptrdiff_t stride, int width,
                                 int row_begin, int row_end, int predictor,
                                 const uint8_t* syms);

}  // namespace rcv
