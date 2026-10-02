#include "capture_gl.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

#include "capture_core.h"
#include "gl_api.h"
#include "hlog.h"
#include "hook_state.h"

namespace rec::hook {
namespace {

using namespace gl;

constexpr GLenum GL_FRAMEBUFFER_SRGB = 0x8DB9;

// One read-back buffer: a pixel-pack buffer with the BGRA frame of one captured tick on its way to the host.
struct Pbo {
    GLuint buf = 0;
    GLsync fence = nullptr;
    bool pending = false;  // read issued, frame not yet delivered
    bool mapped = false;   // mapped and handed to the copy worker
    proto::SlotHeader* slot = nullptr;
    void* ptr = nullptr;
    FrameInfo info;
};

// The game's GL state that our calls touch, saved before and put back after.
struct SavedState {
    GLint read_fbo = 0, draw_fbo = 0, pack_buffer = 0, renderbuffer = 0, read_buffer = GL_BACK;
    bool read_buffer_changed = false;
    GLboolean scissor = GL_FALSE, srgb = GL_FALSE;
    GLboolean mask[4] = {GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE};
    GLint pack[6] = {0, 0, 0, 0, 4, 0};  // row length, skip rows, skip pixels, alignment... see kPackEnums
};

// Pack parameters glReadPixels honours; the values we need are 0 (4 for the alignment).
constexpr GLenum kPackEnums[6] = {GL_PACK_ROW_LENGTH, GL_PACK_SKIP_ROWS, GL_PACK_SKIP_PIXELS, GL_PACK_ALIGNMENT, GL_PACK_SWAP_BYTES, GL_PACK_LSB_FIRST};
constexpr GLint kPackWanted[6] = {0, 0, 0, 4, 0, 0};

struct GlCapture : Backend {
    Functions f;
    bool funcs_loaded = false;

    HGLRC ctx = nullptr;  // the context our objects belong to
    bool ready = false;
    GLuint out_fbo = 0, out_rb = 0;  // the output-size target we blit into
    GLuint res_fbo = 0, res_rb = 0;  // resolve target for a multisampled default framebuffer
    int src_w = 0, src_h = 0;        // the window's client size
    int res_w = 0, res_h = 0;
    int dx0 = 0, dy0 = 0, dx1 = 0, dy1 = 0;  // blit destination: letterboxed, rows flipped
    Pbo pbo[kMaxStaging];
    uint32_t st_next = 0, st_oldest = 0, st_pending = 0;

    proto::Layout layout() const override { return proto::Layout::Bgra; }
    uint32_t slot_bytes(uint32_t w, uint32_t h) const override { return proto::bgra_slot_bytes(w, h); }
    void service() override;
    void submit(const FrameInfo& info, void* target) override;
    uint32_t pending() const override { return st_pending; }
    void release() override;
};

GlCapture* g_gl = nullptr;  // heap, never destroyed by the CRT: GL objects must not be touched during process teardown

GlCapture& gl_capture() {
    if (!g_gl) g_gl = new GlCapture;
    return *g_gl;
}

// ---- Function loading ----------------------------------------------------------------------------
bool load_functions(Functions& f, const char** missing) {
    HMODULE m = GetModuleHandleW(L"opengl32.dll");
    if (!m) {
        *missing = "opengl32.dll";
        return false;
    }
    f.wglGetCurrentContext = reinterpret_cast<decltype(f.wglGetCurrentContext)>(GetProcAddress(m, "wglGetCurrentContext"));
    f.wglGetProcAddress = reinterpret_cast<decltype(f.wglGetProcAddress)>(GetProcAddress(m, "wglGetProcAddress"));
    if (!f.wglGetCurrentContext || !f.wglGetProcAddress) {
        *missing = "wglGetProcAddress";
        return false;
    }
    auto sym = [&](const char* name) -> FARPROC {
        if (FARPROC p = GetProcAddress(m, name)) return p;  // GL 1.1: exported
        PROC q = f.wglGetProcAddress(name);                 // newer: the driver's table for the current context
        const auto v = reinterpret_cast<intptr_t>(q);
        if (!q || v == 1 || v == 2 || v == 3 || v == -1) return nullptr;  // some drivers return small integers for "not found"
        return reinterpret_cast<FARPROC>(q);
    };
#define REC_LOAD(n)                                                              \
    do {                                                                         \
        f.n = reinterpret_cast<decltype(f.n)>(sym("gl" #n));                     \
        if (!f.n) {                                                              \
            *missing = "gl" #n;                                                  \
            return false;                                                        \
        }                                                                        \
    } while (0)
    REC_LOAD(GetIntegerv);
    REC_LOAD(GetBooleanv);
    REC_LOAD(IsEnabled);
    REC_LOAD(Enable);
    REC_LOAD(Disable);
    REC_LOAD(ColorMask);
    REC_LOAD(PixelStorei);
    REC_LOAD(ReadPixels);
    REC_LOAD(ReadBuffer);
    REC_LOAD(GetString);
    REC_LOAD(GetError);
    REC_LOAD(GenFramebuffers);
    REC_LOAD(DeleteFramebuffers);
    REC_LOAD(BindFramebuffer);
    REC_LOAD(FramebufferRenderbuffer);
    REC_LOAD(CheckFramebufferStatus);
    REC_LOAD(GenRenderbuffers);
    REC_LOAD(DeleteRenderbuffers);
    REC_LOAD(BindRenderbuffer);
    REC_LOAD(RenderbufferStorage);
    REC_LOAD(BlitFramebuffer);
    REC_LOAD(GenBuffers);
    REC_LOAD(DeleteBuffers);
    REC_LOAD(BindBuffer);
    REC_LOAD(BufferData);
    REC_LOAD(MapBufferRange);
    REC_LOAD(UnmapBuffer);
    REC_LOAD(FenceSync);
    REC_LOAD(ClientWaitSync);
    REC_LOAD(DeleteSync);
    REC_LOAD(ClearBufferfv);
#undef REC_LOAD
    return true;
}

void fail_gl(GlCapture& c, const char* stage, unsigned code) {
    if (core_mark_failed()) {
        log_event(Ev::CaptureFailed, "%s gl=0x%04X", stage, code);
        c.release();
    }
}

// ---- State ---------------------------------------------------------------------------------------
void save_state(const Functions& f, SavedState* s) {
    f.GetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &s->read_fbo);
    f.GetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &s->draw_fbo);
    f.GetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &s->pack_buffer);
    f.GetIntegerv(GL_RENDERBUFFER_BINDING, &s->renderbuffer);
    s->scissor = f.IsEnabled(GL_SCISSOR_TEST);
    s->srgb = f.IsEnabled(GL_FRAMEBUFFER_SRGB);
    f.GetBooleanv(GL_COLOR_WRITEMASK, s->mask);
    for (int i = 0; i < 6; ++i) f.GetIntegerv(kPackEnums[i], &s->pack[i]);
}

// What a blit and a read-back need: no scissor, no colour mask, no sRGB conversion, tightly packed rows.
void prepare_state(const Functions& f, const SavedState& s) {
    if (s.scissor) f.Disable(GL_SCISSOR_TEST);
    if (s.srgb) f.Disable(GL_FRAMEBUFFER_SRGB);
    if (!s.mask[0] || !s.mask[1] || !s.mask[2] || !s.mask[3]) f.ColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    for (int i = 0; i < 6; ++i)
        if (s.pack[i] != kPackWanted[i]) f.PixelStorei(kPackEnums[i], kPackWanted[i]);
}

void restore_state(const Functions& f, const SavedState& s) {
    for (int i = 0; i < 6; ++i)
        if (s.pack[i] != kPackWanted[i]) f.PixelStorei(kPackEnums[i], s.pack[i]);
    if (!s.mask[0] || !s.mask[1] || !s.mask[2] || !s.mask[3]) f.ColorMask(s.mask[0], s.mask[1], s.mask[2], s.mask[3]);
    if (s.srgb) f.Enable(GL_FRAMEBUFFER_SRGB);
    if (s.scissor) f.Enable(GL_SCISSOR_TEST);
    f.BindBuffer(GL_PIXEL_PACK_BUFFER, GLuint(s.pack_buffer));
    f.BindRenderbuffer(GL_RENDERBUFFER, GLuint(s.renderbuffer));
    if (s.read_buffer_changed) {  // the default framebuffer's read buffer
        f.BindFramebuffer(GL_READ_FRAMEBUFFER, 0);
        f.ReadBuffer(GLenum(s.read_buffer));
    }
    f.BindFramebuffer(GL_READ_FRAMEBUFFER, GLuint(s.read_fbo));
    f.BindFramebuffer(GL_DRAW_FRAMEBUFFER, GLuint(s.draw_fbo));
}

// ---- Objects (context current, state prepared) -------------------------------------------------------
bool make_target(GlCapture& c, GLuint* fbo, GLuint* rb, int w, int h, bool keep_fbo) {
    const Functions& f = c.f;
    if (!keep_fbo) f.GenFramebuffers(1, fbo);
    if (!*rb) f.GenRenderbuffers(1, rb);
    f.BindRenderbuffer(GL_RENDERBUFFER, *rb);
    f.RenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, w, h);
    f.BindFramebuffer(GL_DRAW_FRAMEBUFFER, *fbo);
    f.FramebufferRenderbuffer(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, *rb);
    return f.CheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
}

// The output target, black all over (the letterbox bars are never drawn over), and the read-back buffers.
bool create_objects(GlCapture& c) {
    const Functions& f = c.f;
    Core& k = core();
    while (f.GetError() != GL_NO_ERROR) {  // errors left by the game would be taken for ours
    }
    if (!make_target(c, &c.out_fbo, &c.out_rb, int(k.out_w), int(k.out_h), false)) {
        fail_gl(c, "output framebuffer incomplete", f.CheckFramebufferStatus(GL_DRAW_FRAMEBUFFER));
        return false;
    }
    const GLfloat black[4] = {0, 0, 0, 1};
    f.ClearBufferfv(GL_COLOR, 0, black);
    const GLsizeiptr bytes = GLsizeiptr(k.out_w) * GLsizeiptr(k.out_h) * 4;
    for (uint32_t i = 0; i < k.staging_n; ++i) {
        f.GenBuffers(1, &c.pbo[i].buf);
        f.BindBuffer(GL_PIXEL_PACK_BUFFER, c.pbo[i].buf);
        f.BufferData(GL_PIXEL_PACK_BUFFER, bytes, nullptr, GL_STREAM_READ);
    }
    const GLenum e = f.GetError();
    if (e != GL_NO_ERROR) {
        fail_gl(c, "create objects", e);
        return false;
    }
    return true;
}

// The window changed size (or this is the first frame): where the picture goes in the output.
void place_picture(GlCapture& c) {
    const Core& k = core();
    const float scale = (std::min)(float(k.out_w) / float(c.src_w), float(k.out_h) / float(c.src_h));
    const int pw = (std::max)(1, (std::min)(int(k.out_w), int(float(c.src_w) * scale + 0.5f)));
    const int ph = (std::max)(1, (std::min)(int(k.out_h), int(float(c.src_h) * scale + 0.5f)));
    const int px = (int(k.out_w) - pw) / 2, py = (int(k.out_h) - ph) / 2;
    c.dx0 = px;
    c.dx1 = px + pw;
    c.dy0 = py + ph;  // the rows go in flipped: GL's rows start at the bottom, the video's at the top
    c.dy1 = py;
}

// ---- Delivering a finished frame into the shared ring ---------------------------------------------
enum class Readback { NotReady, Started, Dropped, Error };

void drop_fence(const Functions& f, Pbo& p) {
    if (p.fence) f.DeleteSync(p.fence);
    p.fence = nullptr;
}

// Stage 1, once the GPU has finished the read: take a ring slot, map the buffer and hand the mapped
// memory to the worker. Never waits.
Readback begin_readback(GlCapture& c, Pbo& p, uint32_t index) {
    const Functions& f = c.f;
    const uint64_t t0 = qpc();
    const GLenum w = f.ClientWaitSync(p.fence, 0, 0);
    if (w == GL_WAIT_FAILED) return fail_gl(c, "fence wait", w), Readback::Error;
    if (w != GL_ALREADY_SIGNALED && w != GL_CONDITION_SATISFIED) return Readback::NotReady;

    proto::SlotHeader* slot = core_acquire_slot();
    if (!slot) {
        drop_fence(f, p);
        return Readback::Dropped;
    }
    GLint previous = 0;
    f.GetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &previous);
    f.BindBuffer(GL_PIXEL_PACK_BUFFER, p.buf);
    const Core& k = core();
    void* ptr = f.MapBufferRange(GL_PIXEL_PACK_BUFFER, 0, GLsizeiptr(k.out_w) * GLsizeiptr(k.out_h) * 4, GL_MAP_READ_BIT);
    f.BindBuffer(GL_PIXEL_PACK_BUFFER, GLuint(previous));
    if (!ptr) {
        slot->state.store(uint32_t(proto::SlotState::Free), std::memory_order_release);
        return fail_gl(c, "map buffer", f.GetError()), Readback::Error;
    }
    p.slot = slot;
    p.ptr = ptr;
    p.mapped = true;
    core_post_copy(index, reinterpret_cast<uint8_t*>(slot) + proto::bgra_offset(), static_cast<const uint8_t*>(ptr), k.out_w * 4, k.out_w * 4, k.out_h);
    p.info.begin_us = us_since(t0);
    p.info.cost_us += p.info.begin_us;
    core().ph_begin += to_us(qpc() - t0);
    return Readback::Started;
}

// Stage 2, once the worker is done: unmap and publish the slot to the host.
void finish_readback(GlCapture& c, Pbo& p) {
    const Functions& f = c.f;
    const uint64_t t0 = qpc();
    GLint previous = 0;
    f.GetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &previous);
    f.BindBuffer(GL_PIXEL_PACK_BUFFER, p.buf);
    f.UnmapBuffer(GL_PIXEL_PACK_BUFFER);
    f.BindBuffer(GL_PIXEL_PACK_BUFFER, GLuint(previous));
    p.mapped = false;
    p.ptr = nullptr;
    drop_fence(f, p);
    core_publish(p.slot, p.info, proto::Layout::Bgra, core().out_w * 4, 0, t0);
    p.slot = nullptr;
}

}  // namespace

// Gives back whatever is mapped and every GL object, if the game's context is current on this thread
// (it is, inside a swap). Otherwise (detach, from the control thread) the objects are abandoned: the
// context belongs to the game, and calling GL without it would hit whatever context is current.
void GlCapture::release() {
    core_stop_worker();
    const bool can_gl = funcs_loaded && ctx && f.wglGetCurrentContext() == ctx;
    for (Pbo& p : pbo) {
        if (p.mapped) {
            if (can_gl) {
                f.BindBuffer(GL_PIXEL_PACK_BUFFER, p.buf);
                f.UnmapBuffer(GL_PIXEL_PACK_BUFFER);
                f.BindBuffer(GL_PIXEL_PACK_BUFFER, 0);
            }
            if (p.slot) p.slot->state.store(uint32_t(proto::SlotState::Free), std::memory_order_release);
        }
        if (can_gl) {
            if (p.fence) f.DeleteSync(p.fence);
            if (p.buf) f.DeleteBuffers(1, &p.buf);
        }
        p = Pbo{};
    }
    if (can_gl) {
        if (out_fbo) f.DeleteFramebuffers(1, &out_fbo);
        if (out_rb) f.DeleteRenderbuffers(1, &out_rb);
        if (res_fbo) f.DeleteFramebuffers(1, &res_fbo);
        if (res_rb) f.DeleteRenderbuffers(1, &res_rb);
    }
    out_fbo = out_rb = res_fbo = res_rb = 0;
    st_next = st_oldest = st_pending = 0;
    src_w = src_h = res_w = res_h = 0;
    ready = false;
    ctx = nullptr;
}

// Moves the oldest frames along: GPU finished -> copy started -> copy finished -> published.
void GlCapture::service() {
    GlCapture& c = *this;
    if (!ready || st_pending == 0) return;
    if (f.wglGetCurrentContext() != ctx) return;  // another context is current: submit() deals with it
    while (c.st_pending > 0 && !core().failed) {
        const uint32_t index = c.st_oldest;
        Pbo& p = c.pbo[index];
        if (!p.mapped) {
            const Readback r = begin_readback(c, p, index);
            if (r == Readback::NotReady || r == Readback::Error) return;
            if (r == Readback::Dropped) {
                p.pending = false;
                c.st_oldest = (c.st_oldest + 1) % core().staging_n;
                --c.st_pending;
                continue;
            }
        }
        const uint32_t job = core_job_state(index);
        if (job == 1) return;  // the worker is still copying: next swap
        if (job == 3) return fail_gl(c, "copy worker fault", 0);
        core_job_clear(index);
        finish_readback(c, p);
        p.pending = false;
        c.st_oldest = (c.st_oldest + 1) % core().staging_n;
        --c.st_pending;
    }
}

// ---- Capturing a frame ---------------------------------------------------------------------------
void GlCapture::submit(const FrameInfo& info, void* target) {
    GlCapture& c = *this;
    Core& k = core();
    HDC hdc = static_cast<HDC>(target);
    const uint64_t t_setup = qpc();

    if (!funcs_loaded) {
        const char* missing = "";
        if (!load_functions(f, &missing)) {
            if (core_mark_failed()) {
                log_event(Ev::UnsupportedGlVersion, "missing %s", missing);
                release();
            }
            return;
        }
        funcs_loaded = true;
    }
    const HGLRC current = f.wglGetCurrentContext();
    if (!current) return;  // nothing to draw with on this thread
    HWND window = WindowFromDC(hdc);
    RECT rc{};
    if (!window || !GetClientRect(window, &rc)) return;
    const int sw = rc.right - rc.left, sh = rc.bottom - rc.top;
    if (sw < 16 || sh < 16) return;  // minimised

    if (ready && current != ctx) {  // the game swaps with a different context: our objects belong to the old one
        log_event(Ev::GlContextChanged, "context %p -> %p", static_cast<void*>(ctx), static_cast<void*>(current));
        release();
        if (!core_start_worker()) return fail_gl(c, "restart copy worker", 0);
    }
    if (ready && c.pbo[c.st_next].pending) {  // every read-back buffer is still waiting for the GPU or the host
        g.ctl->gpu_backlog_skips.fetch_add(1, std::memory_order_relaxed);
        ++k.warn_backlog;
        return;
    }

    SavedState st;
    save_state(f, &st);
    prepare_state(f, st);

    if (!ready) {
        ctx = current;
        if (!core_start_worker()) {
            restore_state(f, st);
            return fail_gl(c, "start copy worker", 0);
        }
        if (!create_objects(c)) {
            restore_state(f, st);
            return;
        }
        ready = true;
    }
    if (sw != src_w || sh != src_h) {
        if (src_w) log_event(Ev::BackbufferResized, "%dx%d -> %dx%d", src_w, src_h, sw, sh);
        src_w = sw;
        src_h = sh;
        place_picture(c);
    }

    // The default framebuffer is the source. Its sample count is that of the draw framebuffer, so both are the default for the query.
    f.BindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    f.BindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    f.GetIntegerv(GL_READ_BUFFER, &st.read_buffer);
    if (st.read_buffer != GLint(GL_BACK)) {
        f.ReadBuffer(GL_BACK);
        st.read_buffer_changed = true;
    }
    GLint samples = 0;
    f.GetIntegerv(GL_SAMPLES, &samples);

    const uint64_t t_issue = qpc();
    GLuint source_fbo = 0;
    if (samples > 1) {  // a multisampled default framebuffer can't be scaled in one blit: resolve it first
        if (!res_fbo || res_w != src_w || res_h != src_h) {
            const bool again = res_fbo != 0;
            if (!make_target(c, &res_fbo, &res_rb, src_w, src_h, again)) {
                restore_state(f, st);
                return fail_gl(c, "resolve framebuffer incomplete", f.CheckFramebufferStatus(GL_DRAW_FRAMEBUFFER));
            }
            res_w = src_w;
            res_h = src_h;
        }
        f.BindFramebuffer(GL_DRAW_FRAMEBUFFER, res_fbo);
        f.BlitFramebuffer(0, 0, src_w, src_h, 0, 0, src_w, src_h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        source_fbo = res_fbo;
    }
    f.BindFramebuffer(GL_READ_FRAMEBUFFER, source_fbo);
    f.BindFramebuffer(GL_DRAW_FRAMEBUFFER, out_fbo);
    f.BlitFramebuffer(0, 0, src_w, src_h, dx0, dy0, dx1, dy1, GL_COLOR_BUFFER_BIT, GL_LINEAR);

    Pbo& p = c.pbo[c.st_next];
    f.BindFramebuffer(GL_READ_FRAMEBUFFER, out_fbo);
    f.BindBuffer(GL_PIXEL_PACK_BUFFER, p.buf);
    f.ReadPixels(0, 0, GLsizei(k.out_w), GLsizei(k.out_h), GL_BGRA, GL_UNSIGNED_BYTE, nullptr);
    p.fence = f.FenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    restore_state(f, st);
    if (k.ph_submits < 3) {  // the first frames: did our calls raise an error?
        const GLenum e = f.GetError();
        if (e != GL_NO_ERROR) log_text(Level::Debug, "GL error 0x%04X after the capture sequence", unsigned(e));
    }

    const uint64_t t_done = qpc();
    k.ph_setup += to_us(t_issue - t_setup);
    k.ph_issue += to_us(t_done - t_issue);
    ++k.ph_submits;
    p.pending = true;
    p.mapped = false;
    p.info = info;
    p.info.cost_us = uint32_t(to_us(t_done - t_setup));
    p.info.setup_us = uint32_t(to_us(t_issue - t_setup));
    p.info.issue_us = uint32_t(to_us(t_done - t_issue));
    c.st_next = (c.st_next + 1) % k.staging_n;
    ++c.st_pending;
    k.last_tick = int64_t(info.tick);
}

uint64_t capture_gl_on_present(HDC hdc, uint64_t now, uint32_t frame_time_us) {
    return core_on_present(gl_capture(), hdc, now, frame_time_us);
}

void capture_gl_shutdown() {
    if (!g_gl) return;
    core_shutdown(*g_gl);
}

}  // namespace rec::hook
