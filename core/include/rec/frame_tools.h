// Small tools for looking at captured frames: a checksum, the test app's barcode, NV12 -> RGB and a
// PNG writer (recorder plan §14.1 and a way to inspect what the hook captured).
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <vector>

namespace rec {

// 64-bit hash of a block of bytes (multiply-xor over 8-byte words); for telling frames apart and for
// checking that the same frame arrives the same way, not a cryptographic hash.
uint64_t hash_bytes(const void* data, size_t size, uint64_t seed = 0x9E3779B97F4A7C15ull);

// The frame counter that rec_testapp draws as 32 black/white blocks across the top sixth of its
// window (most significant bit left). `y` is the luma plane of the captured frame (out_w x out_h);
// src_w x src_h is the game's window size, from which the letterboxed position of the picture is
// derived the same way the capture shader does it. False if a block is neither clearly black nor
// clearly white (not the test pattern).
bool decode_barcode(const uint8_t* y, uint32_t stride, uint32_t out_w, uint32_t out_h, uint32_t src_w, uint32_t src_h, uint32_t* value);

// The test app's colour bars, checked against the conversion in the capture shader: the largest
// difference seen between the captured value and BT.601 full-range YUV of the colour the app drew.
struct ColorError {
    int y = 0, cb = 0, cr = 0;
};
// Exact BT.601 full-range Y, Cb, Cr of colour bar `bar` (0-7) in the test app's frame number `frame`.
void testapp_bar_yuv(uint32_t frame, int bar, double* y, double* cb, double* cr);
// `frame` is the barcode value of the captured frame. Compares the middle of each of the 8 bars (low
// in the picture, clear of the moving square) and raises *max_error where it is larger. False if the
// frame is too small to check.
bool check_testapp_colors(const uint8_t* y, uint32_t y_stride, const uint8_t* uv, uint32_t uv_stride, uint32_t out_w, uint32_t out_h,
                          uint32_t src_w, uint32_t src_h, uint32_t frame, ColorError* max_error);

// BT.601 full-range NV12 (Y plane, interleaved Cb Cr plane) to packed 8-bit RGB (w * h * 3 bytes).
void nv12_to_rgb(const uint8_t* y, uint32_t y_stride, const uint8_t* uv, uint32_t uv_stride, uint32_t w, uint32_t h, std::vector<uint8_t>* rgb);

// Writes an uncompressed (stored-block) PNG. False on I/O errors.
bool write_png_rgb(const std::filesystem::path& path, uint32_t w, uint32_t h, const uint8_t* rgb);

// Percentile (0..100) of values, by sorting a copy; 0 for an empty set.
double percentile(std::vector<double> values, double pct);

}  // namespace rec
