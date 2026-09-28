#include "skipmap.h"

#include <emmintrin.h>

#include <cstring>

namespace rcv {
namespace {

int block_width(const Geometry& g, int p) { return 1 << g.block_shift[p]; }

// n bytes equal? n is 16 or 8 except for a partial edge block.
bool span_equal(const uint8_t* a, const uint8_t* b, int n) {
    if (n == 16) {
        const __m128i va = _mm_loadu_si128(reinterpret_cast<const __m128i*>(a));
        const __m128i vb = _mm_loadu_si128(reinterpret_cast<const __m128i*>(b));
        return _mm_movemask_epi8(_mm_cmpeq_epi8(va, vb)) == 0xFFFF;
    }
    if (n == 8) {
        uint64_t va, vb;
        std::memcpy(&va, a, 8);
        std::memcpy(&vb, b, 8);
        return va == vb;
    }
    return std::memcmp(a, b, size_t(n)) == 0;
}

// Every |a - b| <= tol for 16 bytes: |a - b| via saturating subtracts both ways.
bool within16(__m128i va, __m128i vb, __m128i tol) {
    const __m128i d = _mm_or_si128(_mm_subs_epu8(va, vb), _mm_subs_epu8(vb, va));
    return _mm_movemask_epi8(_mm_cmpeq_epi8(_mm_subs_epu8(d, tol), _mm_setzero_si128())) == 0xFFFF;
}

// n bytes within `tol` of each other (near-lossless skip).
bool span_within(const uint8_t* a, const uint8_t* b, int n, int tol) {
    const __m128i t = _mm_set1_epi8(char(tol));
    if (n == 16)
        return within16(_mm_loadu_si128(reinterpret_cast<const __m128i*>(a)),
                        _mm_loadu_si128(reinterpret_cast<const __m128i*>(b)), t);
    if (n == 8)  // upper halves are zero in both, so they compare equal
        return within16(_mm_loadl_epi64(reinterpret_cast<const __m128i*>(a)),
                        _mm_loadl_epi64(reinterpret_cast<const __m128i*>(b)), t);
    for (int x = 0; x < n; ++x) {
        const int d = int(a[x]) - int(b[x]);
        if (d > tol || d < -tol) return false;
    }
    return true;
}

// NV12 chroma against planar reference Cb/Cr, within `tol`.
bool nv12_span_within(const uint8_t* uv, const uint8_t* u, const uint8_t* v, int n, int tol) {
    if (n == 8) {
        const __m128i ref = _mm_unpacklo_epi8(_mm_loadl_epi64(reinterpret_cast<const __m128i*>(u)),
                                              _mm_loadl_epi64(reinterpret_cast<const __m128i*>(v)));
        return within16(_mm_loadu_si128(reinterpret_cast<const __m128i*>(uv)), ref, _mm_set1_epi8(char(tol)));
    }
    for (int x = 0; x < n; ++x) {
        const int d0 = int(uv[2 * x]) - int(u[x]), d1 = int(uv[2 * x + 1]) - int(v[x]);
        if (d0 > tol || d0 < -tol || d1 > tol || d1 < -tol) return false;
    }
    return true;
}

// n chroma samples of an interleaved CbCr (NV12) row against planar reference Cb and Cr.
bool nv12_span_equal(const uint8_t* uv, const uint8_t* u, const uint8_t* v, int n) {
    if (n == 8) {
        const __m128i ref = _mm_unpacklo_epi8(_mm_loadl_epi64(reinterpret_cast<const __m128i*>(u)),
                                              _mm_loadl_epi64(reinterpret_cast<const __m128i*>(v)));
        const __m128i src = _mm_loadu_si128(reinterpret_cast<const __m128i*>(uv));
        return _mm_movemask_epi8(_mm_cmpeq_epi8(src, ref)) == 0xFFFF;
    }
    for (int x = 0; x < n; ++x)
        if (uv[2 * x] != u[x] || uv[2 * x + 1] != v[x]) return false;
    return true;
}

}  // namespace

int coded_row_width(const Geometry& g, int p, const uint8_t* row_flags) {
    const int bw = block_width(g, p);
    int w = 0;
    for (int b = 0; b < g.blocks_x; ++b) {
        if (row_flags[b]) continue;
        const int x0 = b * bw;
        w += (g.plane_w[p] - x0) < bw ? (g.plane_w[p] - x0) : bw;
    }
    return w;
}

size_t coded_chunk_samples(const Geometry& g, int p, int s, const uint8_t* flags) {
    if (!flags) return g.chunk_samples(p, s);
    size_t n = 0;
    const int shift = g.block_shift[p];
    for (int br = g.slice_block_row[s]; br < g.slice_block_row[s + 1]; ++br) {
        const int y0 = br << shift;
        const int y1 = ((br + 1) << shift) < g.plane_h[p] ? ((br + 1) << shift) : g.plane_h[p];
        n += size_t(y1 - y0) * size_t(coded_row_width(g, p, flags + size_t(br) * size_t(g.blocks_x)));
    }
    return n;
}

void pack_skip_map(const Geometry& g, const uint8_t* flags, uint8_t* out) {
    const size_t blocks = size_t(g.blocks_x) * size_t(g.blocks_y);
    std::memset(out, 0, skip_map_size(g));
    for (size_t i = 0; i < blocks; ++i)
        if (flags[i]) out[i >> 3] = uint8_t(out[i >> 3] | (1u << (i & 7)));
}

bool unpack_skip_map(const Geometry& g, const uint8_t* in, uint8_t* flags) {
    const size_t blocks = size_t(g.blocks_x) * size_t(g.blocks_y);
    for (size_t i = 0; i < blocks; ++i) flags[i] = uint8_t((in[i >> 3] >> (i & 7)) & 1);
    if (blocks & 7) {  // unused high bits of the last byte
        if (in[blocks >> 3] >> (blocks & 7)) return false;
    }
    for (size_t k = (blocks + 7) >> 3; k < skip_map_size(g); ++k)
        if (in[k]) return false;
    return true;
}

int compare_block_row(const Geometry& g, const rcv_frame_in* in, rcv_input_layout layout, uint8_t* const ref_plane[3],
                      const ptrdiff_t ref_stride[3], int by, uint8_t* unchanged, int tolerance) {
    const int nb = g.blocks_x;
    std::memset(unchanged, 1, size_t(nb));
    int remaining = nb;  // blocks not yet known to differ
    const bool nv12 = layout == RCV_IN_NV12;
    for (int p = 0; p < 3; ++p) {
        if (nv12 && p == 2) break;  // NV12: Cb and Cr are compared together with p == 1
        const int bw = block_width(g, p), w = g.plane_w[p];
        const int y0 = by << g.block_shift[p];
        const int y1 = (y0 + bw) < g.plane_h[p] ? (y0 + bw) : g.plane_h[p];
        for (int y = y0; y < y1; ++y) {
            const uint8_t* r = ref_plane[p] + y * ref_stride[p];
            if (p == 0 || !nv12) {
                const uint8_t* s = in->plane[p] + ptrdiff_t(y) * in->stride[p];
                for (int b = 0, x0 = 0; b < nb; ++b, x0 += bw) {
                    if (!unchanged[b]) continue;
                    const int n = (w - x0) < bw ? (w - x0) : bw;
                    if (tolerance ? !span_within(s + x0, r + x0, n, tolerance) : !span_equal(s + x0, r + x0, n)) {
                        unchanged[b] = 0;
                        if (--remaining == 0) return 0;
                    }
                }
            } else {
                const uint8_t* uv = in->plane[1] + ptrdiff_t(y) * in->stride[1];
                const uint8_t* v = ref_plane[2] + y * ref_stride[2];
                for (int b = 0, x0 = 0; b < nb; ++b, x0 += bw) {
                    if (!unchanged[b]) continue;
                    const int n = (w - x0) < bw ? (w - x0) : bw;
                    if (tolerance ? !nv12_span_within(uv + 2 * x0, r + x0, v + x0, n, tolerance)
                                  : !nv12_span_equal(uv + 2 * x0, r + x0, v + x0, n)) {
                        unchanged[b] = 0;
                        if (--remaining == 0) return 0;
                    }
                }
            }
        }
    }
    return remaining;
}

}  // namespace rcv
