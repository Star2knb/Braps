// BGRA -> NV12 on the host (recorder plan §8.2): the AVX2 and scalar versions agree byte for byte, and both
// agree with the floating-point BT.601 formula the GPU shader uses to within one level.
#include <chrono>
#include <cmath>
#include <vector>

#include "rec/convert.h"
#include "testfw.h"

using namespace rec;

namespace {

std::vector<uint8_t> random_bgra(uint32_t w, uint32_t h, uint32_t seed) {
    std::vector<uint8_t> v(size_t(w) * h * 4);
    uint32_t x = seed * 2654435761u + 1;
    for (uint8_t& b : v) {
        x = x * 1664525u + 1013904223u;
        b = uint8_t(x >> 24);
    }
    return v;
}

struct Planes {
    std::vector<uint8_t> y, uv;
};

Planes convert(void (*fn)(const uint8_t*, size_t, uint32_t, uint32_t, uint8_t*, size_t, uint8_t*, size_t), const std::vector<uint8_t>& bgra, uint32_t w,
               uint32_t h) {
    Planes p;
    p.y.assign(size_t(w) * h, 0xCD);
    p.uv.assign(size_t(w) * h / 2, 0xCD);
    fn(bgra.data(), size_t(w) * 4, w, h, p.y.data(), w, p.uv.data(), w);
    return p;
}

int clamp255(double v) { return v < 0 ? 0 : v > 255 ? 255 : int(v + 0.5); }

}  // namespace

TEST_CASE("convert: the AVX2 version gives the same bytes as the scalar one, for any even size") {
    const uint32_t sizes[][2] = {{2, 2}, {16, 2}, {18, 4}, {34, 6}, {64, 8}, {100, 10}, {1280, 720}, {1366, 746}};
    uint32_t seed = 1;
    for (const auto& s : sizes) {
        const std::vector<uint8_t> src = random_bgra(s[0], s[1], seed++);
        const Planes a = convert(bgra_to_nv12, src, s[0], s[1]);
        const Planes b = convert(bgra_to_nv12_scalar, src, s[0], s[1]);
        REQUIRE(a.y == b.y);
        REQUIRE(a.uv == b.uv);
    }
}

TEST_CASE("convert: close to the floating-point BT.601 full-range formula") {
    const uint32_t w = 64, h = 32;
    const std::vector<uint8_t> src = random_bgra(w, h, 77);
    const Planes p = convert(bgra_to_nv12, src, w, h);
    int worst_y = 0, worst_c = 0;
    for (uint32_t row = 0; row < h; ++row)
        for (uint32_t col = 0; col < w; ++col) {
            const uint8_t* px = &src[(size_t(row) * w + col) * 4];
            const double y = 0.299 * px[2] + 0.587 * px[1] + 0.114 * px[0];
            worst_y = (std::max)(worst_y, std::abs(clamp255(y) - int(p.y[size_t(row) * w + col])));
        }
    for (uint32_t row = 0; row < h; row += 2)
        for (uint32_t col = 0; col < w; col += 2) {
            double r = 0, g = 0, b = 0;
            for (uint32_t dy = 0; dy < 2; ++dy)
                for (uint32_t dx = 0; dx < 2; ++dx) {
                    const uint8_t* px = &src[(size_t(row + dy) * w + col + dx) * 4];
                    b += px[0] / 4.0;
                    g += px[1] / 4.0;
                    r += px[2] / 4.0;
                }
            const double cb = 128 - 0.168736 * r - 0.331264 * g + 0.5 * b;
            const double cr = 128 + 0.5 * r - 0.418688 * g - 0.081312 * b;
            worst_c = (std::max)(worst_c, std::abs(clamp255(cb) - int(p.uv[size_t(row / 2) * w + col])));
            worst_c = (std::max)(worst_c, std::abs(clamp255(cr) - int(p.uv[size_t(row / 2) * w + col + 1])));
        }
    CHECK(worst_y <= 1);
    CHECK(worst_c <= 1);
}

TEST_CASE("convert: black, white and grey stay neutral, saturated colours clamp") {
    const uint32_t w = 16, h = 2;
    auto solid = [&](uint8_t b, uint8_t g, uint8_t r) {
        std::vector<uint8_t> v(size_t(w) * h * 4);
        for (size_t i = 0; i < v.size(); i += 4) {
            v[i] = b;
            v[i + 1] = g;
            v[i + 2] = r;
            v[i + 3] = 255;
        }
        return convert(bgra_to_nv12, v, w, h);
    };
    Planes p = solid(0, 0, 0);
    CHECK(p.y[0] == 0 && p.uv[0] == 128 && p.uv[1] == 128);
    p = solid(255, 255, 255);
    CHECK(p.y[0] == 255 && p.uv[0] == 128 && p.uv[1] == 128);
    p = solid(100, 100, 100);
    CHECK(p.y[0] == 100 && p.uv[0] == 128 && p.uv[1] == 128);
    p = solid(255, 0, 0);  // pure blue: Cb 255.5 clamps to 255
    CHECK(p.uv[0] == 255);
    p = solid(0, 0, 255);  // pure red: Cr 255.5 clamps to 255
    CHECK(p.uv[1] == 255);
}

TEST_CASE("convert: 1280x720 takes well under 2 ms (optimised builds)") {
    const uint32_t w = 1280, h = 720;
    const std::vector<uint8_t> src = random_bgra(w, h, 5);
    Planes p;
    p.y.resize(size_t(w) * h);
    p.uv.resize(size_t(w) * h / 2);
    double best_ms = 1e9;
    for (int i = 0; i < 20; ++i) {
        const auto t0 = std::chrono::steady_clock::now();
        bgra_to_nv12(src.data(), size_t(w) * 4, w, h, p.y.data(), w, p.uv.data(), w);
        best_ms = (std::min)(best_ms, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
    }
    std::printf("      convert 1280x720: %.2f ms best of 20 (%s)\n", best_ms, bgra_to_nv12_uses_avx2() ? "AVX2" : "scalar");
#ifdef NDEBUG
    CHECK(best_ms < 2.0);  // only meaningful with the optimiser on
#endif
}
