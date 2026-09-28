// Near-lossless quantisation (codec plan §5.5). Baseline code only: no SIMD translation unit
// includes this header.
#pragma once

#include <cstdint>

namespace rcv {

constexpr int kMaxNear = 3;

// Tables for one NEAR value n in 1..3 (step = 2n + 1).
struct NearTables {
    int step = 1;
    int8_t q[511];    // index e + 255 (e = x - pred): q = sign(e) * ((|e| + n) / step), |q| <= 85
    int16_t qstep[511];  // index e + 255: q * step, so the encoder's reconstruction needs no multiply
    int16_t dq[256];  // index = symbol: (int8_t)symbol * step. Defined for every byte, so a corrupt
                      // stream can't index out of range.
};

void init_near_tables(int n, NearTables* t);

inline int clamp255(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

// Reference definitions, used by the tests: quantise e for NEAR n, and reconstruct.
inline int near_quantise(int e, int n) {
    const int step = 2 * n + 1;
    return e >= 0 ? (e + n) / step : -((-e + n) / step);
}
inline int near_reconstruct(int pred, int q, int n) { return clamp255(pred + q * (2 * n + 1)); }

}  // namespace rcv
