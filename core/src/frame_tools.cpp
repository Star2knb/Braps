#include "rec/frame_tools.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace rec {
namespace {

uint32_t crc32(const uint8_t* data, size_t size, uint32_t crc = 0) {
    static uint32_t table[256];
    static bool ready = false;
    if (!ready) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        ready = true;
    }
    crc = ~crc;
    for (size_t i = 0; i < size; ++i) crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

void put_u32_be(std::vector<uint8_t>* out, uint32_t v) {
    for (int s = 24; s >= 0; s -= 8) out->push_back(uint8_t(v >> s));
}

void put_chunk(std::vector<uint8_t>* out, const char type[4], const std::vector<uint8_t>& data) {
    put_u32_be(out, uint32_t(data.size()));
    const size_t start = out->size();
    out->insert(out->end(), type, type + 4);
    out->insert(out->end(), data.begin(), data.end());
    put_u32_be(out, crc32(out->data() + start, out->size() - start));
}

}  // namespace

uint64_t hash_bytes(const void* data, size_t size, uint64_t seed) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    uint64_t h = seed;
    size_t i = 0;
    for (; i + 8 <= size; i += 8) {
        uint64_t w;
        std::memcpy(&w, p + i, 8);
        h = (h ^ w) * 0x100000001B3ull;
        h ^= h >> 29;
    }
    for (; i < size; ++i) h = (h ^ p[i]) * 0x100000001B3ull;
    return h ^ (h >> 32);
}

bool decode_barcode(const uint8_t* y, uint32_t stride, uint32_t out_w, uint32_t out_h, uint32_t src_w, uint32_t src_h, uint32_t* value) {
    if (!y || !src_w || !src_h) return false;
    const double scale = std::min(double(out_w) / src_w, double(out_h) / src_h);
    const double pw = src_w * scale, ph = src_h * scale;
    const double ox = (out_w - pw) * 0.5, oy = (out_h - ph) * 0.5;
    const double cy = oy + ph / 12.0;  // middle of the top sixth
    uint32_t v = 0;
    for (int bit = 0; bit < 32; ++bit) {
        const double cx = ox + pw * (bit + 0.5) / 32.0;
        int sum = 0, n = 0;
        for (int dy = -2; dy <= 2; ++dy)
            for (int dx = -2; dx <= 2; ++dx) {
                const int px = int(cx) + dx, py = int(cy) + dy;
                if (px < 0 || py < 0 || px >= int(out_w) || py >= int(out_h)) continue;
                sum += y[size_t(py) * stride + size_t(px)];
                ++n;
            }
        if (!n) return false;
        const int mean = sum / n;
        if (mean > 64 && mean < 192) return false;  // neither black nor white
        v = (v << 1) | (mean >= 192 ? 1u : 0u);
    }
    *value = v;
    return true;
}

void testapp_bar_yuv(uint32_t frame, int bar, double* y, double* cb, double* cr) {
    // The colour rec_testapp clears the bar to (hue_to_rgb in testapp/main.cpp), as the 8-bit back buffer holds it.
    const float hue = std::fmod(float(frame) * 0.004f + float(bar) / 8.0f, 1.0f);
    const float h6 = hue * 6.0f;
    const float x = 1.0f - std::fabs(std::fmod(h6, 2.0f) - 1.0f);
    float r = 0, g = 0, b = 0;
    if (h6 < 1) { r = 1; g = x; }
    else if (h6 < 2) { r = x; g = 1; }
    else if (h6 < 3) { g = 1; b = x; }
    else if (h6 < 4) { g = x; b = 1; }
    else if (h6 < 5) { r = x; b = 1; }
    else { r = 1; b = x; }
    const int R = int((r * 0.8f + 0.1f) * 255.0f + 0.5f), G = int((g * 0.8f + 0.1f) * 255.0f + 0.5f), B = int((b * 0.8f + 0.1f) * 255.0f + 0.5f);
    *y = 0.299 * R + 0.587 * G + 0.114 * B;
    *cb = 128.0 - 0.168736 * R - 0.331264 * G + 0.5 * B;
    *cr = 128.0 + 0.5 * R - 0.418688 * G - 0.081312 * B;
}

bool check_testapp_colors(const uint8_t* y, uint32_t y_stride, const uint8_t* uv, uint32_t uv_stride, uint32_t out_w, uint32_t out_h,
                          uint32_t src_w, uint32_t src_h, uint32_t frame, ColorError* max_error) {
    if (!y || !uv || !src_w || !src_h || out_w < 64 || out_h < 64) return false;
    const double scale = std::min(double(out_w) / src_w, double(out_h) / src_h);
    const double pw = src_w * scale, ph = src_h * scale;
    const double ox = (out_w - pw) * 0.5, oy = (out_h - ph) * 0.5;
    for (int bar = 0; bar < 8; ++bar) {
        double ey, ecb, ecr;
        testapp_bar_yuv(frame, bar, &ey, &ecb, &ecr);
        const int px = int(ox + pw * (bar + 0.5) / 8.0);
        const int py = int(oy + ph * 0.9);
        if (px < 0 || py < 0 || px >= int(out_w) || py >= int(out_h)) return false;
        const int cy = y[size_t(py) * y_stride + size_t(px)];
        const uint8_t* chroma = uv + size_t(py / 2) * uv_stride + size_t(px / 2) * 2;
        max_error->y = std::max(max_error->y, int(std::lround(std::fabs(cy - ey))));
        max_error->cb = std::max(max_error->cb, int(std::lround(std::fabs(chroma[0] - ecb))));
        max_error->cr = std::max(max_error->cr, int(std::lround(std::fabs(chroma[1] - ecr))));
    }
    return true;
}

void nv12_to_rgb(const uint8_t* y, uint32_t y_stride, const uint8_t* uv, uint32_t uv_stride, uint32_t w, uint32_t h, std::vector<uint8_t>* rgb) {
    rgb->resize(size_t(w) * h * 3);
    for (uint32_t r = 0; r < h; ++r) {
        const uint8_t* yrow = y + size_t(r) * y_stride;
        const uint8_t* uvrow = uv + size_t(r / 2) * uv_stride;
        uint8_t* out = rgb->data() + size_t(r) * w * 3;
        for (uint32_t c = 0; c < w; ++c) {
            const float Y = yrow[c];
            const float cb = float(uvrow[(c / 2) * 2]) - 128.0f;
            const float cr = float(uvrow[(c / 2) * 2 + 1]) - 128.0f;
            auto clamp8 = [](float v) { return uint8_t(v < 0 ? 0 : v > 255 ? 255 : int(v + 0.5f)); };
            out[c * 3 + 0] = clamp8(Y + 1.402f * cr);
            out[c * 3 + 1] = clamp8(Y - 0.344136f * cb - 0.714136f * cr);
            out[c * 3 + 2] = clamp8(Y + 1.772f * cb);
        }
    }
}

bool write_png_rgb(const std::filesystem::path& path, uint32_t w, uint32_t h, const uint8_t* rgb) {
    // Raw scanlines, each prefixed with filter type 0.
    std::vector<uint8_t> raw;
    raw.reserve((size_t(w) * 3 + 1) * h);
    for (uint32_t r = 0; r < h; ++r) {
        raw.push_back(0);
        raw.insert(raw.end(), rgb + size_t(r) * w * 3, rgb + size_t(r + 1) * w * 3);
    }
    // zlib stream of stored (uncompressed) deflate blocks.
    std::vector<uint8_t> z = {0x78, 0x01};
    uint32_t a = 1, b = 0;
    for (uint8_t byte : raw) {
        a = (a + byte) % 65521;
        b = (b + a) % 65521;
    }
    for (size_t pos = 0; pos < raw.size() || pos == 0;) {
        const size_t n = std::min<size_t>(65535, raw.size() - pos);
        const bool last = pos + n >= raw.size();
        z.push_back(last ? 1 : 0);
        z.push_back(uint8_t(n & 0xFF));
        z.push_back(uint8_t(n >> 8));
        z.push_back(uint8_t(~n & 0xFF));
        z.push_back(uint8_t((~n >> 8) & 0xFF));
        z.insert(z.end(), raw.begin() + pos, raw.begin() + pos + n);
        pos += n;
        if (last) break;
    }
    put_u32_be(&z, (b << 16) | a);

    std::vector<uint8_t> png = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    std::vector<uint8_t> ihdr;
    put_u32_be(&ihdr, w);
    put_u32_be(&ihdr, h);
    ihdr.insert(ihdr.end(), {8, 2, 0, 0, 0});  // 8-bit RGB, no interlace
    put_chunk(&png, "IHDR", ihdr);
    put_chunk(&png, "IDAT", z);
    put_chunk(&png, "IEND", {});

    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"wb") != 0 || !f) return false;
    const bool ok = std::fwrite(png.data(), 1, png.size(), f) == png.size();
    return std::fclose(f) == 0 && ok;
}

double percentile(std::vector<double> values, double pct) {
    if (values.empty()) return 0;
    std::sort(values.begin(), values.end());
    const double pos = pct / 100.0 * double(values.size() - 1);
    const size_t lo = size_t(pos);
    const size_t hi = std::min(lo + 1, values.size() - 1);
    return values[lo] + (values[hi] - values[lo]) * (pos - double(lo));
}

}  // namespace rec
