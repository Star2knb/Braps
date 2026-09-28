// RCV1 encoder (codec plan §5, §6, §7, §8).
// Scope so far: lossless YUV420 I-, P- and DUP frames. Phase A (temporal skip compare) runs one job
// per slice; phase B one job per (plane, slice) chunk. Residual kernels are dispatched by CPU
// (scalar / SSE4.1 / AVX2). Output is byte-identical for every thread count and kernel level.
#include <chrono>
#include <cstring>
#include <new>

#include "aligned_buffer.h"
#include "colour.h"
#include "cpu.h"
#include "crc32c.h"
#include "format.h"
#include "huffman.h"
#include "nearlossless.h"
#include "predict.h"
#include "profile.h"
#include "rcv/rcv.h"
#include "skipmap.h"
#include "threadpool.h"

using namespace rcv;

struct rcv_encoder {
    rcv_encoder_config cfg;   // with defaults resolved (num_threads = actual count)
    Geometry geo;
    uint8_t colour;
    uint8_t* ref_mem;         // reference frame = last reconstructed frame (§5.7)
    uint8_t* ref_plane[3];
    ptrdiff_t ref_stride[3];
    size_t max_packet;
    uint32_t frame_number;
    uint32_t frames_since_i;
    bool has_ref;
    rcv_isa isa;              // resolved kernel level
    ResidualFn residuals;
    ResidualRowFn residual_row;
    HuffWriteFn huff_write;
    StageTimes* prof;         // optional, rcv_bench only

    // Threading (§8). Every buffer below is allocated in rcv_encoder_create.
    ThreadPool pool;
    uint8_t* resid_mem;
    uint8_t* resid[kMaxThreads];          // per worker: residual symbols of the chunk being coded
    uint8_t* chunk_mem;
    uint8_t* chunk_buf[3 * kMaxSlices];   // per job: the coded chunk, sized for its RAW bound
    size_t chunk_size[3 * kMaxSlices];
    StageTimes worker_prof[kMaxThreads];  // per worker, merged into *prof after each frame
    const rcv_frame_in* job_in;           // input of the frame being encoded

    // Temporal skip (§5.3), filled by phase A.
    uint8_t* skip_flags;                  // blocks_x * blocks_y, raster order, 1 = unchanged
    uint32_t* row_skipped;                // per block row: number of skipped blocks
    uint32_t slice_skipped[kMaxSlices];
    bool frame_p;                         // the frame being encoded is a P-frame
    bool run_phase_a;                     // the prepare pass also runs the skip compare
    int near;                             // NEAR of the frame being encoded (0 = lossless), §5.5
    NearTables near_tables[kMaxNear + 1]; // [1..3]
    uint8_t* rowbuf_mem;
    uint8_t* rowbuf[kMaxThreads];         // per worker: two row buffers of rowbuf_size bytes (residuals of a
                                          // partially skipped row, or de-interleaved NV12 source rows)
    size_t rowbuf_size;

    // GBR format (§4.4). Each frame's BGRA input is converted into these planes, which the encoder
    // then predicts from directly; afterwards they become the reference by swapping pointers with
    // it (lossless: the reconstruction equals the source), so RGB needs no copy into the reference.
    bool gbr;
    uint8_t* staging_mem;
    uint8_t* staging_plane[3];
    rcv_frame_in staging_in;              // the staging planes as a planar input, for the skip compare
};

void rcv::set_stage_profile(rcv_encoder* enc, StageTimes* sink) {
    if (enc) enc->prof = sink;
}

namespace {

using Clock = std::chrono::steady_clock;

uint32_t elapsed_us(Clock::time_point t0) {
    return uint32_t(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - t0).count());
}

rcv_status validate_config(const rcv_encoder_config* c, Geometry* g) {
    if (!c) return RCV_ERR_INVALID_ARG;
    if (c->format == RCV_FMT_YUV420) {
        if (c->input_layout != RCV_IN_I420 && c->input_layout != RCV_IN_NV12) return RCV_ERR_INVALID_ARG;
    } else if (c->format == RCV_FMT_GBR) {
        if (c->input_layout != RCV_IN_BGRA && c->input_layout != RCV_IN_BGRX) return RCV_ERR_INVALID_ARG;
    } else {
        return RCV_ERR_INVALID_ARG;
    }
    if (c->num_slices > kMaxSlices) return RCV_ERR_INVALID_ARG;
    if (!init_geometry(g, c->format, c->coded_width, c->coded_height, c->num_slices))
        return RCV_ERR_INVALID_ARG;
    if (c->display_width > c->coded_width || c->display_height > c->coded_height)
        return RCV_ERR_INVALID_ARG;
    if (c->predictor > kPredMed || c->colour_matrix > 1 || c->full_range > 1)
        return RCV_ERR_INVALID_ARG;
    if (unsigned(c->isa) > unsigned(RCV_ISA_AVX2)) return RCV_ERR_INVALID_ARG;
    return RCV_OK;
}

rcv_encoder_config resolve_defaults(const rcv_encoder_config& c) {
    rcv_encoder_config r = c;
    if (r.display_width == 0) r.display_width = r.coded_width;
    if (r.display_height == 0) r.display_height = r.coded_height;
    if (r.keyframe_interval == 0) r.keyframe_interval = kDefaultKeyint;
    return r;
}

// Checked on the calling thread before any job runs.
rcv_status validate_input(const rcv_encoder* e, const rcv_frame_in* in) {
    const Geometry& g = e->geo;
    if (e->gbr) {  // BGRA / BGRX: 4 bytes per pixel, alpha ignored
        if (!in->plane[0] || in->stride[0] < 4 * g.width) return RCV_ERR_INVALID_ARG;
        return RCV_OK;
    }
    if (e->cfg.input_layout == RCV_IN_I420) {
        for (int p = 0; p < 3; ++p)
            if (!in->plane[p] || in->stride[p] < g.plane_w[p]) return RCV_ERR_INVALID_ARG;
        return RCV_OK;
    }
    // NV12: Y plane + interleaved CbCr plane (width bytes per chroma row).
    if (!in->plane[0] || !in->plane[1] || in->stride[0] < g.width || in->stride[1] < g.width)
        return RCV_ERR_INVALID_ARG;
    return RCV_OK;
}

// Copies samples [x0, x1) of row y of plane p from the input into the reference buffer (source ==
// reconstruction when lossless). Jobs only ever copy rows of their own slice.
void copy_span(rcv_encoder* e, const rcv_frame_in* in, int p, int y, int x0, int x1) {
    uint8_t* d = e->ref_plane[p] + y * e->ref_stride[p];
    if (e->cfg.input_layout == RCV_IN_I420 || p == 0) {
        std::memcpy(d + x0, in->plane[p] + ptrdiff_t(y) * in->stride[p] + x0, size_t(x1 - x0));
        return;
    }
    // NV12 chroma: Cb = even bytes, Cr = odd bytes of the interleaved plane.
    const int k = p - 1;
    const uint8_t* uv = in->plane[1] + ptrdiff_t(y) * in->stride[1];
    for (int x = x0; x < x1; ++x) d[x] = uv[2 * x + k];
}

void copy_rows(rcv_encoder* e, const rcv_frame_in* in, int p, int r0, int r1) {
    for (int y = r0; y < r1; ++y) copy_span(e, in, p, y, 0, e->geo.plane_w[p]);
}

// Planes the encoder predicts from: the reference (after copying the input into it) for YUV,
// the staging planes (the converted input) for GBR. Both have the same strides.
uint8_t* work_plane(const rcv_encoder* e, int p) { return e->gbr ? e->staging_plane[p] : e->ref_plane[p]; }

ResidualFn residual_kernel(rcv_isa isa) {
    switch (isa) {
    case RCV_ISA_AVX2: return residuals_lossless_avx2;
    case RCV_ISA_SSE41: return residuals_lossless_sse41;
    default: return residuals_lossless_scalar;
    }
}

ResidualRowFn residual_row_kernel(rcv_isa isa) {
    switch (isa) {
    case RCV_ISA_AVX2: return residual_row_avx2;
    case RCV_ISA_SSE41: return residual_row_sse41;
    default: return residual_row_scalar;
    }
}

// Writes one chunk (§6.4) into `out` and returns its size (a multiple of 4).
// `out` is the job's own scratch buffer of the chunk's RAW bound, 4 + align4(count) bytes; bytes
// past the returned size may be scribbled on (they are never copied into the packet).
size_t encode_chunk(uint8_t* out, const uint8_t* syms, size_t count, const uint32_t hist[256], HuffWriteFn write,
                    StageTimer& timer) {
    std::memset(out, 0, kChunkHeaderSize);
    if (count == 0) {
        out[0] = kChunkEmpty;
        timer.lap(&StageTimes::table_ns);
        return kChunkHeaderSize;
    }
    int distinct = 0, last = 0;
    for (int s = 0; s < 256; ++s)
        if (hist[s]) {
            ++distinct;
            last = s;
        }
    if (distinct == 1) {
        out[0] = kChunkSingle;
        out[1] = uint8_t(last);
        timer.lap(&StageTimes::table_ns);
        return kChunkHeaderSize;
    }

    uint8_t len[256];
    huff_build_lengths(hist, len);
    uint64_t bits = 0;
    for (int s = 0; s < 256; ++s) bits += uint64_t(hist[s]) * len[s];
    const size_t huff_size = kChunkHeaderSize + kHuffTableSize + align4(size_t((bits + 7) / 8));
    const size_t raw_size = kChunkHeaderSize + align4(count);

    if (huff_size > raw_size) {
        out[0] = kChunkRaw;
        timer.lap(&StageTimes::table_ns);
        std::memcpy(out + kChunkHeaderSize, syms, count);
        std::memset(out + kChunkHeaderSize + count, 0, raw_size - kChunkHeaderSize - count);
        timer.lap(&StageTimes::entropy_ns);
        return raw_size;
    }

    out[0] = kChunkHuffman;
    huff_pack_lengths(len, out + kChunkHeaderSize);
    uint16_t code[256];
    huff_canonical_codes(len, code);  // always valid for lengths from huff_build_lengths
    HuffEncTable table;
    huff_make_enc_table(len, code, &table);
    timer.lap(&StageTimes::table_ns);
    uint8_t* bitstream = out + kChunkHeaderSize + kHuffTableSize;
    const size_t n = write(syms, count, table, bitstream, out + raw_size);
    std::memset(bitstream + n, 0, align4(n) - n);
    timer.lap(&StageTimes::entropy_ns);
    return huff_size;
}

// Prepare job for slice s, before phase B:
//  - GBR: converts the slice's BGRA rows into the staging planes (§4.4);
//  - phase A (§5.3), if it runs: compares every block of the slice with the reference.
// Only reads the reference; phase B, which may overwrite it, starts after every prepare job.
void prepare_job(void* ctx, int s, int worker) {
    rcv_encoder* e = static_cast<rcv_encoder*>(ctx);
    const Geometry& g = e->geo;
    StageTimer timer(e->prof ? &e->worker_prof[worker] : nullptr);
    if (e->gbr) {
        const uint8_t* src = e->job_in->plane[0];
        const ptrdiff_t ss = e->job_in->stride[0], ds = e->ref_stride[0];
        for (int y = g.slice_row(0, s); y < g.slice_row(0, s + 1); ++y)
            bgra_to_gbr_row(src + y * ss, g.width, e->staging_plane[0] + y * ds, e->staging_plane[1] + y * ds,
                            e->staging_plane[2] + y * ds);
        timer.lap(&StageTimes::load_ns);
    }
    if (!e->run_phase_a) return;
    // GBR compares the converted planes (equal planes <=> equal RGB: the transform is reversible).
    const rcv_frame_in* cmp = e->gbr ? &e->staging_in : e->job_in;
    const rcv_input_layout layout = e->gbr ? RCV_IN_I420 : e->cfg.input_layout;  // GBR staging is planar
    uint32_t skipped = 0;
    for (int by = g.slice_block_row[s]; by < g.slice_block_row[s + 1]; ++by) {
        uint8_t* f = e->skip_flags + size_t(by) * size_t(g.blocks_x);
        // Near-lossless: a block within +-NEAR of the (reconstructed) reference may be skipped;
        // the reference then stays within the error bound of this frame too (§5.3).
        const uint32_t row =
            uint32_t(compare_block_row(g, cmp, layout, e->ref_plane, e->ref_stride, by, f, e->near));
        e->row_skipped[by] = row;
        skipped += row;
    }
    e->slice_skipped[s] = skipped;
    timer.lap(&StageTimes::skip_ns);
}

// P-frame chunk: rows are handled one at a time. A row whose blocks are all skipped costs nothing;
// otherwise the row is copied into the reference (YUV; GBR predicts from its staging planes),
// residuals are computed for the whole row with the SIMD kernel, and only the non-skipped samples
// are kept, in raster order (§5.4). Skipped samples are never coded but still serve as neighbours.
// Copying the whole row equals the plan's "copy the non-skipped blocks" (§5.7) because a lossless
// skip means those samples are already identical; it is one memcpy instead of many small ones.
// (Near-lossless skip, M7, tolerates differences and will need the per-block copy.)
size_t p_chunk_residuals(rcv_encoder* e, int pl, int s, int worker, StageTimer& timer) {
    const Geometry& g = e->geo;
    const int r0 = g.slice_row(pl, s), r1 = g.slice_row(pl, s + 1);
    const int w = g.plane_w[pl], shift = g.block_shift[pl], bw = 1 << shift;
    uint8_t* plane = work_plane(e, pl);
    const ptrdiff_t stride = e->ref_stride[pl];
    uint8_t* out = e->resid[worker];
    uint8_t* row = e->rowbuf[worker];
    size_t n = 0;
    for (int j = r0; j < r1; ++j) {
        const int br = j >> shift;
        const uint32_t nskip = e->row_skipped[br];
        if (nskip == uint32_t(g.blocks_x)) continue;
        const uint8_t* f = e->skip_flags + size_t(br) * size_t(g.blocks_x);
        if (!e->gbr) {
            copy_span(e, e->job_in, pl, j, 0, w);
            timer.lap(&StageTimes::load_ns);
        }
        uint8_t* x = plane + j * stride;
        const uint8_t* up = j == r0 ? nullptr : x - stride;
        if (nskip == 0) {
            e->residual_row(x, up, w, e->cfg.predictor, out + n);
            n += size_t(w);
        } else {
            e->residual_row(x, up, w, e->cfg.predictor, row);
            for (int b = 0; b < g.blocks_x; ++b) {
                if (f[b]) continue;
                const int x0 = b * bw, len = (w - x0) < bw ? (w - x0) : bw;
                std::memcpy(out + n, row + x0, size_t(len));
                n += size_t(len);
            }
        }
        timer.lap(&StageTimes::predict_ns);
    }
    return n;
}

// Source samples of row y of plane p: straight from the input, or de-interleaved (NV12 chroma)
// into `buf`.
const uint8_t* source_row(const rcv_encoder* e, int p, int y, uint8_t* buf) {
    const rcv_frame_in* in = e->job_in;
    if (e->cfg.input_layout == RCV_IN_I420 || p == 0) return in->plane[p] + ptrdiff_t(y) * in->stride[p];
    const uint8_t* uv = in->plane[1] + ptrdiff_t(y) * in->stride[1];
    const int k = p - 1;
    for (int x = 0, w = e->geo.plane_w[p]; x < w; ++x) buf[x] = uv[2 * x + k];
    return buf;
}

// Near-lossless coding of samples [i0, i1) of one row (§5.5). Prediction uses reconstructed
// neighbours - `x` (this row, in the reference) and `up` (the row above, or nullptr for the first
// row of the slice) - so it is serial along the row, like decoding. The quantised error is the
// symbol; the reconstruction, within +-NEAR of the source, is written into the reference, where it
// serves as the neighbour for the following samples and as the next frame's reference.
uint8_t* near_span(const uint8_t* src, uint8_t* x, const uint8_t* up, int i0, int i1, int predictor,
                   const NearTables& t, uint8_t* out) {
    const int8_t* q = t.q + 255;         // symbol for e = x - pred in [-255, 255]
    const int16_t* qs = t.qstep + 255;   // q * step for the same e
    int i = i0;
    if (i == 0 && i < i1) {
        const int pred = up ? up[0] : 128;
        const int e = src[0] - pred;
        *out++ = uint8_t(q[e]);
        x[0] = uint8_t(clamp255(pred + qs[e]));
        i = 1;
    }
    if (i >= i1) return out;
    if (!up || predictor != kPredMed) {  // first row of a slice, or LEFT: predict from the left
        int a = x[i - 1];
        for (; i < i1; ++i) {
            const int e = src[i] - a;
            *out++ = uint8_t(q[e]);
            a = clamp255(a + qs[e]);
            x[i] = uint8_t(a);
        }
    } else {
        int a = x[i - 1], c = up[i - 1];
        for (; i < i1; ++i) {
            const int b = up[i];
            const int pred = predict_med_clamp(a, b, c);
            const int e = src[i] - pred;
            *out++ = uint8_t(q[e]);
            a = clamp255(pred + qs[e]);
            x[i] = uint8_t(a);
            c = b;
        }
    }
    return out;
}

// Two consecutive full rows A (j) and B (j + 1) coded as a wavefront. B at column i needs only A up
// to column i, so both rows advance in the same loop with B one step behind: two independent
// serial chains the CPU overlaps on one core. Output (symbols in raster order: all of A, then all
// of B; reconstruction) is identical to near_span on A, then on B.
// MedA: row A uses MED (it has a row above); MedB: row B uses MED (its row above is A).
template <bool MedA, bool MedB>
uint8_t* near_rows2(const uint8_t* sa, const uint8_t* sb, uint8_t* xa, uint8_t* xb, const uint8_t* upa, int w,
                    const NearTables& t, uint8_t* out) {
    const int8_t* q = t.q + 255;
    const int16_t* qs = t.qstep + 255;
    uint8_t* oa = out;
    uint8_t* ob = out + w;
    int pa = upa ? upa[0] : 128;  // column 0: above (or 128 on a slice's first row) ...
    int e = sa[0] - pa;
    oa[0] = uint8_t(q[e]);
    int ra = clamp255(pa + qs[e]);
    xa[0] = uint8_t(ra);
    e = sb[0] - ra;  // ... and for B the sample above it, A[0]
    ob[0] = uint8_t(q[e]);
    int rb = clamp255(ra + qs[e]);
    xb[0] = uint8_t(rb);
    int ca = MedA ? upa[0] : 0;
    for (int i = 1; i < w; ++i) {
        int predA;
        if (MedA) {
            const int b = upa[i];
            predA = predict_med_clamp(ra, b, ca);
            ca = b;
        } else {
            predA = ra;
        }
        e = sa[i] - predA;
        oa[i] = uint8_t(q[e]);
        const int prev_a = ra;
        ra = clamp255(predA + qs[e]);
        xa[i] = uint8_t(ra);

        int predB;
        if (MedB) {
            predB = predict_med_clamp(rb, ra, prev_a);  // left, above = A[i], above-left = A[i-1]
        } else {
            predB = rb;
        }
        e = sb[i] - predB;
        ob[i] = uint8_t(q[e]);
        rb = clamp255(predB + qs[e]);
        xb[i] = uint8_t(rb);
    }
    return out + 2 * size_t(w);
}

// Near-lossless chunk, I or P: rows whose blocks are all skipped cost nothing (the reference keeps
// the previous reconstruction there); other rows code their non-skipped blocks with near_span.
size_t near_chunk_residuals(rcv_encoder* e, int pl, int s, int worker, StageTimer& timer) {
    const Geometry& g = e->geo;
    const int r0 = g.slice_row(pl, s), r1 = g.slice_row(pl, s + 1);
    const int w = g.plane_w[pl], shift = g.block_shift[pl], bw = 1 << shift;
    const NearTables& t = e->near_tables[e->near];
    uint8_t* plane = e->ref_plane[pl];
    const ptrdiff_t stride = e->ref_stride[pl];
    uint8_t* const out0 = e->resid[worker];
    uint8_t* out = out0;
    const bool med = e->cfg.predictor == kPredMed;
    auto row_skips = [&](int j) { return e->frame_p ? e->row_skipped[j >> shift] : 0u; };
    for (int j = r0; j < r1; ++j) {
        const int br = j >> shift;
        const uint32_t nskip = row_skips(j);
        if (nskip == uint32_t(g.blocks_x)) continue;
        uint8_t* x = plane + j * stride;
        const uint8_t* up = j == r0 ? nullptr : x - stride;
        if (nskip == 0 && j + 1 < r1 && row_skips(j + 1) == 0) {
            // Two full rows: wavefront (see near_rows2).
            const uint8_t* sa = source_row(e, pl, j, e->rowbuf[worker]);
            const uint8_t* sb = source_row(e, pl, j + 1, e->rowbuf[worker] + e->rowbuf_size);
            timer.lap(&StageTimes::load_ns);
            if (!med)
                out = near_rows2<false, false>(sa, sb, x, x + stride, up, w, t, out);
            else if (up)
                out = near_rows2<true, true>(sa, sb, x, x + stride, up, w, t, out);
            else
                out = near_rows2<false, true>(sa, sb, x, x + stride, up, w, t, out);
            timer.lap(&StageTimes::predict_ns);
            ++j;
            continue;
        }
        const uint8_t* src = source_row(e, pl, j, e->rowbuf[worker]);
        timer.lap(&StageTimes::load_ns);
        if (nskip == 0) {
            out = near_span(src, x, up, 0, w, e->cfg.predictor, t, out);
        } else {
            const uint8_t* f = e->skip_flags + size_t(br) * size_t(g.blocks_x);
            for (int b = 0; b < g.blocks_x; ++b)
                if (!f[b]) out = near_span(src, x, up, b * bw, (b + 1) * bw < w ? (b + 1) * bw : w, e->cfg.predictor, t, out);
        }
        timer.lap(&StageTimes::predict_ns);
    }
    return size_t(out - out0);
}

// Phase B job = one (plane, slice) chunk: copy its rows, predict, entropy-code into its own scratch
// buffer. Job j is plane j / S, slice j % S, so the big luma chunks are claimed first (§8).
// Jobs touch disjoint rows of the reference and never read another slice's rows (§4.6).
void encode_job(void* ctx, int job, int worker) {
    rcv_encoder* e = static_cast<rcv_encoder*>(ctx);
    const Geometry& g = e->geo;
    const int S = g.num_slices, pl = job / S, s = job % S;
    StageTimer timer(e->prof ? &e->worker_prof[worker] : nullptr);
    uint32_t hist[256] = {};
    size_t n;
    if (e->near) {
        n = near_chunk_residuals(e, pl, s, worker, timer);
        histogram_add(e->resid[worker], n, hist);
        timer.lap(&StageTimes::predict_ns);
    } else if (e->frame_p) {
        n = p_chunk_residuals(e, pl, s, worker, timer);
        histogram_add(e->resid[worker], n, hist);
        timer.lap(&StageTimes::predict_ns);
    } else {
        const int r0 = g.slice_row(pl, s), r1 = g.slice_row(pl, s + 1);
        if (!e->gbr) {
            copy_rows(e, e->job_in, pl, r0, r1);
            timer.lap(&StageTimes::load_ns);
        }
        n = e->residuals(work_plane(e, pl), e->ref_stride[pl], g.plane_w[pl], r0, r1, e->cfg.predictor,
                         e->resid[worker], hist);
        timer.lap(&StageTimes::predict_ns);
    }
    e->chunk_size[job] = encode_chunk(e->chunk_buf[job], e->resid[worker], n, hist, e->huff_write, timer);
}

// Adds the per-worker stage times of the frame just encoded to the profile sink.
void merge_profile(rcv_encoder* e, Clock::time_point t0, bool coded, bool ran_skip) {
    if (!e->prof) return;
    // Stage times are CPU time summed over workers; total is wall-clock time.
    for (int w = 0; w < e->pool.threads(); ++w) {
        StageTimes& wp = e->worker_prof[w];
        e->prof->skip_ns += wp.skip_ns;
        e->prof->load_ns += wp.load_ns;
        e->prof->predict_ns += wp.predict_ns;
        e->prof->table_ns += wp.table_ns;
        e->prof->entropy_ns += wp.entropy_ns;
        wp = StageTimes{};
    }
    if (ran_skip) e->prof->skip_frames++;
    if (coded) {
        e->prof->total_ns +=
            uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - t0).count());
        e->prof->frames++;
    }
}

void fill_info(rcv_frame_info* info, uint32_t size, uint8_t type, uint32_t blocks_total,
               uint32_t encode_us, uint32_t total_us) {
    if (!info) return;
    std::memset(info, 0, sizeof(*info));
    info->packet_size = size;
    info->frame_type = type;
    info->is_keyframe = type == kFrameI;
    info->blocks_total = blocks_total;
    info->time_encode_us = encode_us;
    info->time_total_us = total_us;
}

}  // namespace

extern "C" {

void rcv_encoder_config_init(rcv_encoder_config* c) {
    if (!c) return;
    std::memset(c, 0, sizeof(*c));
    c->format = RCV_FMT_YUV420;
    c->input_layout = RCV_IN_I420;
    c->colour_matrix = 0;
    c->full_range = 1;
    c->predictor = kPredMed;
    c->enable_skip = 1;
    c->keyframe_interval = kDefaultKeyint;
    c->fps_num = 60;
    c->fps_den = 1;
    c->isa = RCV_ISA_AUTO;
}

size_t rcv_max_packet_size(const rcv_encoder_config* cfg) {
    Geometry g;
    if (validate_config(cfg, &g) != RCV_OK) return 0;
    return max_packet_size(g);
}

void rcv_write_sequence_header(const rcv_encoder_config* cfg, uint8_t out[32]) {
    if (!out) return;
    std::memset(out, 0, kSeqHeaderSize);
    if (!cfg) return;
    const rcv_encoder_config c = resolve_defaults(*cfg);
    SeqHeader h{};
    h.format = uint8_t(c.format);
    h.coded_w = c.coded_width;
    h.coded_h = c.coded_height;
    h.display_w = c.display_width;
    h.display_h = c.display_height;
    h.colour = pack_colour(c.colour_matrix, c.full_range, 0);
    h.fps_num = c.fps_num;
    h.fps_den = c.fps_den;
    h.keyint = c.keyframe_interval;
    write_seq_header(h, out);
}

rcv_status rcv_encoder_create(const rcv_encoder_config* cfg, rcv_encoder** out) {
    if (!out) return RCV_ERR_INVALID_ARG;
    *out = nullptr;
    Geometry g;
    rcv_status st = validate_config(cfg, &g);
    if (st != RCV_OK) return st;
    rcv_isa isa;
    st = resolve_isa(cfg->isa, &isa);  // once, here (§3.3)
    if (st != RCV_OK) return st;

    rcv_encoder* e = new (std::nothrow) rcv_encoder{};
    if (!e) return RCV_ERR_OUT_OF_MEMORY;
    const int threads = resolve_thread_count(cfg->num_threads);
    e->isa = isa;
    e->residuals = residual_kernel(isa);
    e->residual_row = residual_row_kernel(isa);
    e->huff_write = isa == RCV_ISA_AVX2 ? huff_write_avx2 : huff_write_scalar;
    e->cfg = resolve_defaults(*cfg);
    e->cfg.num_threads = uint8_t(threads);
    e->geo = g;
    e->gbr = e->cfg.format == RCV_FMT_GBR;
    e->colour = pack_colour(e->cfg.colour_matrix, e->cfg.full_range, 0);
    for (int n = 1; n <= kMaxNear; ++n) init_near_tables(n, &e->near_tables[n]);
    e->max_packet = max_packet_size(g);

    size_t total = 0;
    for (int p = 0; p < 3; ++p) {
        e->ref_stride[p] = ptrdiff_t(align64(size_t(g.plane_w[p])));
        total += size_t(e->ref_stride[p]) * size_t(g.plane_h[p]);
    }
    if (e->gbr) {
        e->staging_mem = static_cast<uint8_t*>(aligned_alloc64(total));
        if (!e->staging_mem) {
            rcv_encoder_destroy(e);
            return RCV_ERR_OUT_OF_MEMORY;
        }
        uint8_t* sp = e->staging_mem;
        for (int i = 0; i < 3; ++i) {
            e->staging_plane[i] = sp;
            e->staging_in.plane[i] = sp;
            e->staging_in.stride[i] = int32_t(e->ref_stride[i]);
            sp += size_t(e->ref_stride[i]) * size_t(g.plane_h[i]);
        }
    }
    const size_t resid_each = align64(max_chunk_samples(g));
    const int jobs = 3 * g.num_slices;
    size_t chunk_total = 0;
    for (int j = 0; j < jobs; ++j)
        chunk_total += align64(kChunkHeaderSize + align4(g.chunk_samples(j / g.num_slices, j % g.num_slices)));
    const size_t blocks = size_t(g.blocks_x) * size_t(g.blocks_y);
    const size_t row_each = align64(size_t(g.plane_w[0]));
    e->ref_mem = static_cast<uint8_t*>(aligned_alloc64(total));
    e->resid_mem = static_cast<uint8_t*>(aligned_alloc64(resid_each * size_t(threads)));
    e->chunk_mem = static_cast<uint8_t*>(aligned_alloc64(chunk_total));
    e->skip_flags = static_cast<uint8_t*>(aligned_alloc64(align64(blocks)));
    e->row_skipped = static_cast<uint32_t*>(aligned_alloc64(align64(sizeof(uint32_t) * size_t(g.blocks_y))));
    e->rowbuf_size = row_each;
    e->rowbuf_mem = static_cast<uint8_t*>(aligned_alloc64(2 * row_each * size_t(threads)));
    if (!e->ref_mem || !e->resid_mem || !e->chunk_mem || !e->skip_flags || !e->row_skipped || !e->rowbuf_mem) {
        rcv_encoder_destroy(e);
        return RCV_ERR_OUT_OF_MEMORY;
    }
    std::memset(e->ref_mem, 0, total);
    uint8_t* p = e->ref_mem;
    for (int i = 0; i < 3; ++i) {
        e->ref_plane[i] = p;
        p += size_t(e->ref_stride[i]) * size_t(g.plane_h[i]);
    }
    for (int w = 0; w < threads; ++w) {
        e->resid[w] = e->resid_mem + resid_each * size_t(w);
        e->rowbuf[w] = e->rowbuf_mem + 2 * row_each * size_t(w);
    }
    uint8_t* c = e->chunk_mem;
    for (int j = 0; j < jobs; ++j) {
        e->chunk_buf[j] = c;
        c += align64(kChunkHeaderSize + align4(g.chunk_samples(j / g.num_slices, j % g.num_slices)));
    }
    if (!e->pool.start(threads, cfg->on_worker_start, cfg->user)) {
        rcv_encoder_destroy(e);
        return RCV_ERR_OUT_OF_MEMORY;
    }
    *out = e;
    return RCV_OK;
}

void rcv_encoder_destroy(rcv_encoder* e) {
    if (!e) return;
    e->pool.stop();
    aligned_free64(e->ref_mem);
    aligned_free64(e->resid_mem);
    aligned_free64(e->chunk_mem);
    aligned_free64(e->skip_flags);
    aligned_free64(e->row_skipped);
    aligned_free64(e->rowbuf_mem);
    aligned_free64(e->staging_mem);
    delete e;
}

rcv_status rcv_encode_frame(rcv_encoder* e, const rcv_frame_in* in, const rcv_encode_params* params,
                            uint8_t* out, size_t out_capacity, rcv_frame_info* info) {
    const Clock::time_point t0 = Clock::now();
    if (!e || !in || !out) return RCV_ERR_INVALID_ARG;
    if (out_capacity < e->max_packet) return RCV_ERR_BUFFER_TOO_SMALL;
    const uint8_t near_level = params ? params->near_level : 0;
    if (near_level > kMaxNear) return RCV_ERR_INVALID_ARG;
    if (near_level != 0 && e->gbr) return RCV_ERR_INVALID_ARG;  // never for GBR (§4.1)

    const rcv_status st = validate_input(e, in);
    if (st != RCV_OK) return st;

    const Geometry& g = e->geo;
    const int S = g.num_slices;
    const uint32_t blocks = uint32_t(g.blocks_x) * uint32_t(g.blocks_y);
    e->job_in = in;
    e->near = near_level;  // chosen per frame by the caller's rate controller (§5.5)

    // Frame type (§5.2), in order: (1) forced keyframe, keyframe interval reached or no reference
    // -> I without phase A; (2) skip disabled -> I; (3) nothing changed -> DUP; (4) every block
    // changed -> I (same cost, free seek point); (5) otherwise P.
    const bool force_i = (params && params->force_keyframe) || !e->has_ref ||
                         e->frames_since_i >= e->cfg.keyframe_interval || !e->cfg.enable_skip;
    e->run_phase_a = !force_i;
    uint32_t skipped = 0, skip_us = 0;
    if (e->gbr || !force_i) {
        // Prepare pass: GBR conversion and/or phase A. For GBR, time_skip_us includes the conversion.
        const Clock::time_point ts = Clock::now();
        e->pool.run(prepare_job, e, S);
        for (int s = 0; s < S && !force_i; ++s) skipped += e->slice_skipped[s];
        skip_us = elapsed_us(ts);
        if (!force_i && skipped == blocks) {
            e->job_in = nullptr;
            write_dup_packet(out);  // the reference stays as it is
            e->frames_since_i++;
            e->frame_number++;
            merge_profile(e, t0, false, true);
            fill_info(info, uint32_t(kDupPacketSize), kFrameDup, blocks, 0, elapsed_us(t0));
            if (info) {
                info->blocks_skipped = blocks;
                info->time_skip_us = skip_us;
            }
            return RCV_OK;
        }
    }
    e->frame_p = skipped != 0;

    // Phase B (§5.1): every chunk in parallel, each into its own scratch buffer.
    const int jobs = 3 * S;
    e->pool.run(encode_job, e, jobs);
    e->job_in = nullptr;
    if (e->gbr) {
        // The converted frame is the new reconstruction: it becomes the reference (no copy).
        for (int p = 0; p < 3; ++p) {
            uint8_t* t = e->ref_plane[p];
            e->ref_plane[p] = e->staging_plane[p];
            e->staging_plane[p] = t;
            e->staging_in.plane[p] = t;
        }
    }

    // Assembly in fixed order, so the packet never depends on scheduling (A3).
    uint8_t* payload = out + kFrameHeaderSize;
    uint8_t* dir = payload;
    if (e->frame_p) {
        pack_skip_map(g, e->skip_flags, payload);
        dir += skip_map_size(g);
    }
    uint8_t* p = dir + size_t(jobs) * 4;
    for (int j = 0; j < jobs; ++j) {
        put_u32(dir + 4 * size_t(j), uint32_t(e->chunk_size[j]));
        std::memcpy(p, e->chunk_buf[j], e->chunk_size[j]);
        p += e->chunk_size[j];
    }
    const size_t payload_size = size_t(p - payload);

    FrameHeader h{};
    h.type = e->frame_p ? kFrameP : kFrameI;
    h.flags = uint16_t((e->cfg.enable_crc ? kFlagCrc : 0) | (e->near ? kFlagNear : 0));
    h.format = uint8_t(e->cfg.format);
    h.near_level = uint8_t(e->near);
    h.predictor = e->cfg.predictor;
    h.slices = uint8_t(S);
    h.width = uint16_t(g.width);
    h.height = uint16_t(g.height);
    h.colour = e->colour;
    h.frame_number = e->frame_number;
    h.payload_size = uint32_t(payload_size);
    h.crc = e->cfg.enable_crc ? crc32c(payload, payload_size) : 0;
    write_frame_header(h, out);

    e->has_ref = true;
    e->frames_since_i = e->frame_p ? e->frames_since_i + 1 : 1;
    e->frame_number++;

    merge_profile(e, t0, true, !force_i);
    const uint32_t us = elapsed_us(t0);
    fill_info(info, uint32_t(kFrameHeaderSize + payload_size), h.type, blocks, us - skip_us, us);
    if (info) {
        info->blocks_skipped = skipped;
        info->time_skip_us = skip_us;
    }
    return RCV_OK;
}

rcv_status rcv_encode_duplicate(rcv_encoder* e, uint8_t* out, size_t out_capacity, rcv_frame_info* info) {
    const Clock::time_point t0 = Clock::now();
    if (!e || !out) return RCV_ERR_INVALID_ARG;
    if (out_capacity < kDupPacketSize) return RCV_ERR_BUFFER_TOO_SMALL;
    if (!e->has_ref) return RCV_ERR_NO_REFERENCE;
    write_dup_packet(out);
    e->frames_since_i++;
    e->frame_number++;
    const uint32_t blocks = uint32_t(e->geo.blocks_x * e->geo.blocks_y);
    fill_info(info, uint32_t(kDupPacketSize), kFrameDup, blocks, 0, elapsed_us(t0));
    if (info) info->blocks_skipped = blocks;
    return RCV_OK;
}

}  // extern "C"
