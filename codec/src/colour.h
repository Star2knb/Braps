// Reversible RGB transform for the GBR format (codec plan §4.4) and BGRA packing.
#pragma once

#include <cstdint>

namespace rcv {

// One row of n BGRA/BGRX pixels -> planes G, (B - G) & 0xFF, (R - G) & 0xFF. Alpha is ignored.
// SSE2 (the x64 baseline) for 16 pixels at a time; identical to the scalar reference.
void bgra_to_gbr_row(const uint8_t* bgra, int n, uint8_t* g, uint8_t* bg, uint8_t* rg);
void bgra_to_gbr_row_scalar(const uint8_t* bgra, int n, uint8_t* g, uint8_t* bg, uint8_t* rg);

// Inverse: G = P0, B = (P1 + G) & 0xFF, R = (P2 + G) & 0xFF, alpha = 255.
void gbr_to_bgra_row(const uint8_t* g, const uint8_t* bg, const uint8_t* rg, int n, uint8_t* bgra);
void gbr_to_bgra_row_scalar(const uint8_t* g, const uint8_t* bg, const uint8_t* rg, int n, uint8_t* bgra);

}  // namespace rcv
