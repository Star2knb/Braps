#include "predict.h"

#include <cstring>

namespace rcv {

void histogram_add(const uint8_t* s, size_t n, uint32_t hist[256]) {
    // Four sub-histograms so runs of the same symbol don't serialise on one counter.
    uint32_t h[4][256] = {};
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        uint64_t v;
        std::memcpy(&v, s + i, 8);
        h[0][v & 0xFF]++;
        h[1][(v >> 8) & 0xFF]++;
        h[2][(v >> 16) & 0xFF]++;
        h[3][(v >> 24) & 0xFF]++;
        h[0][(v >> 32) & 0xFF]++;
        h[1][(v >> 40) & 0xFF]++;
        h[2][(v >> 48) & 0xFF]++;
        h[3][v >> 56]++;
    }
    for (; i < n; ++i) h[0][s[i]]++;
    for (int k = 0; k < 256; ++k) hist[k] += h[0][k] + h[1][k] + h[2][k] + h[3][k];
}

size_t residuals_lossless_scalar(const uint8_t* plane, ptrdiff_t stride, int width,
                                 int row_begin, int row_end, int predictor,
                                 uint8_t* out, uint32_t hist[256]) {
    uint8_t* o = out;
    for (int j = row_begin; j < row_end; ++j) {
        const uint8_t* x = plane + j * stride;
        if (j == row_begin) {
            // First row of the slice: 128, then the left neighbour.
            uint8_t pred = 128;
            for (int i = 0; i < width; ++i) {
                const uint8_t s = uint8_t(x[i] - pred);
                *o++ = s;
                hist[s]++;
                pred = x[i];
            }
            continue;
        }
        const uint8_t* up = x - stride;
        {
            const uint8_t s = uint8_t(x[0] - up[0]);
            *o++ = s;
            hist[s]++;
        }
        if (predictor == kPredMed) {
            for (int i = 1; i < width; ++i) {
                const uint8_t s = uint8_t(x[i] - predict_med(x[i - 1], up[i], up[i - 1]));
                *o++ = s;
                hist[s]++;
            }
        } else {
            for (int i = 1; i < width; ++i) {
                const uint8_t s = uint8_t(x[i] - x[i - 1]);
                *o++ = s;
                hist[s]++;
            }
        }
    }
    return size_t(o - out);
}

void residual_row_scalar(const uint8_t* x, const uint8_t* up, int width, int predictor, uint8_t* out) {
    if (!up) {
        uint8_t pred = 128;
        for (int i = 0; i < width; ++i) {
            out[i] = uint8_t(x[i] - pred);
            pred = x[i];
        }
        return;
    }
    out[0] = uint8_t(x[0] - up[0]);
    if (predictor == kPredMed) {
        for (int i = 1; i < width; ++i) out[i] = uint8_t(x[i] - predict_med(x[i - 1], up[i], up[i - 1]));
    } else {
        for (int i = 1; i < width; ++i) out[i] = uint8_t(x[i] - x[i - 1]);
    }
}

void reconstruct_lossless_scalar(uint8_t* plane, ptrdiff_t stride, int width,
                                 int row_begin, int row_end, int predictor,
                                 const uint8_t* syms) {
    const uint8_t* s = syms;
    for (int j = row_begin; j < row_end; ++j) {
        uint8_t* x = plane + j * stride;
        if (j == row_begin) {
            uint8_t pred = 128;
            for (int i = 0; i < width; ++i) {
                x[i] = uint8_t(pred + *s++);
                pred = x[i];
            }
            continue;
        }
        const uint8_t* up = x - stride;
        x[0] = uint8_t(up[0] + *s++);
        if (predictor == kPredMed) {
            for (int i = 1; i < width; ++i) x[i] = uint8_t(predict_med(x[i - 1], up[i], up[i - 1]) + *s++);
        } else {
            for (int i = 1; i < width; ++i) x[i] = uint8_t(x[i - 1] + *s++);
        }
    }
}

}  // namespace rcv
