#include "predict.h"

namespace rcv {

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
