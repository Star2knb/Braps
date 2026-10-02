// BGRA -> NV12 on the host (recorder plan §8.2): OpenGL frames arrive as BGRA, the encoder takes NV12.
// BT.601 full range, the same coefficients as the GPU shader of the Direct3D path (hook/shaders/capture.hlsl):
//   Y  = 0.299 R + 0.587 G + 0.114 B
//   Cb = 128 - 0.168736 R - 0.331264 G + 0.5 B
//   Cr = 128 + 0.5 R - 0.418688 G - 0.081312 B
// with each chroma sample taken from the average of its 2x2 pixels. Integer arithmetic (15-bit coefficients),
// so the AVX2 and the scalar version give the same bytes. Width and height must be even.
#pragma once

#include <cstddef>
#include <cstdint>

namespace rec {

// Picks the AVX2 version when the CPU has it.
void bgra_to_nv12(const uint8_t* bgra, size_t bgra_stride, uint32_t width, uint32_t height, uint8_t* y, size_t y_stride, uint8_t* uv,
                  size_t uv_stride);

// The reference; also what the AVX2 version falls back to for the last columns.
void bgra_to_nv12_scalar(const uint8_t* bgra, size_t bgra_stride, uint32_t width, uint32_t height, uint8_t* y, size_t y_stride, uint8_t* uv,
                         size_t uv_stride);

// Whether bgra_to_nv12 uses AVX2 on this machine.
bool bgra_to_nv12_uses_avx2();

}  // namespace rec
