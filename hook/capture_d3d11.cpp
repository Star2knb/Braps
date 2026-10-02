#include "capture_d3d11.h"

#include <d3d11_4.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cstring>

#include "capture_core.h"
#include "hlog.h"
#include "hook_state.h"
#include "ps_nv12.h"
#include "vs_main.h"

using Microsoft::WRL::ComPtr;

namespace rec::hook {
namespace {

// One GPU read-back slot: the NV12 texture of one captured frame on its way to the host.
struct Staging {
    ComPtr<ID3D11Texture2D> tex;
    ComPtr<ID3D11Query> query;
    bool pending = false;  // copy issued, frame not yet delivered
    bool mapped = false;   // mapped and handed to the copy worker
    proto::SlotHeader* slot = nullptr;
    D3D11_MAPPED_SUBRESOURCE map{};
    FrameInfo info;
};

struct Capture : Backend {
    IDXGISwapChain* swap_chain = nullptr;  // the chain being presented (valid during submit)

    // GPU objects, all on the game's device.
    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext1> ctx;
    ComPtr<ID3D11Multithread> mt;  // the device lock, if the game's device has one
    ComPtr<ID3DDeviceContextState> state;
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    ComPtr<ID3D11SamplerState> sampler;
    ComPtr<ID3D11RasterizerState> raster;
    ComPtr<ID3D11BlendState> blend;
    ComPtr<ID3D11DepthStencilState> depth;
    ComPtr<ID3D11Buffer> cb;
    ComPtr<ID3D11Texture2D> copy_tex;
    ComPtr<ID3D11ShaderResourceView> srv;
    ComPtr<ID3D11Texture2D> nv12_tex;
    ComPtr<ID3D11RenderTargetView> nv12_rtv;
    Staging staging[kMaxStaging];
    uint32_t st_next = 0;    // next staging slot to fill
    uint32_t st_oldest = 0;  // oldest pending one
    uint32_t st_pending = 0;
    DXGI_FORMAT src_format = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT src_resolve_format = DXGI_FORMAT_UNKNOWN;
    UINT src_w = 0, src_h = 0, src_samples = 0;

    proto::Layout layout() const override { return proto::Layout::Nv12; }
    uint32_t slot_bytes(uint32_t w, uint32_t h) const override { return proto::nv12_slot_bytes(w, h); }
    void service() override;
    void submit(const FrameInfo& info, void* target) override;
    uint32_t pending() const override { return st_pending; }
    void release() override;
};

// The device lock while our draw sequence runs, so that a game presenting from a different thread than
// the one it renders on can't have its context calls interleaved with ours. Released by
// capture_on_exception if the sequence is cut short by an exception.
ID3D11Multithread* g_lock_held = nullptr;

Capture* g_cap = nullptr;  // heap, never destroyed by the CRT: COM objects must not be released during process teardown

Capture& cap() {
    if (!g_cap) g_cap = new Capture;
    return *g_cap;
}

// Gives back whatever is mapped, then every GPU object.
void release_gpu(Capture& c) {
    core_stop_worker();
    for (uint32_t i = 0; i < kMaxStaging; ++i) {
        Staging& s = c.staging[i];
        if (s.mapped) {
            if (c.ctx && s.tex) c.ctx->Unmap(s.tex.Get(), 0);
            if (s.slot) s.slot->state.store(uint32_t(proto::SlotState::Free), std::memory_order_release);
            s.mapped = false;
        }
    }
    for (Staging& s : c.staging) s = Staging{};
    c.st_next = c.st_oldest = c.st_pending = 0;
    c.nv12_rtv.Reset();
    c.nv12_tex.Reset();
    c.srv.Reset();
    c.copy_tex.Reset();
    c.cb.Reset();
    c.depth.Reset();
    c.blend.Reset();
    c.raster.Reset();
    c.sampler.Reset();
    c.ps.Reset();
    c.vs.Reset();
    c.state.Reset();
    c.mt.Reset();
    c.ctx.Reset();
    c.dev.Reset();
    c.src_format = DXGI_FORMAT_UNKNOWN;
    c.src_w = c.src_h = c.src_samples = 0;
}

void Capture::release() { release_gpu(*this); }

// The first failure disables capture until the host stops this recording; measuring goes on.
void fail(Capture& c, HRESULT hr, const char* stage) {
    if (core_mark_failed()) {
        if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET)
            log_event(Ev::DeviceRemoved, "%s hr=0x%08lX", stage, static_cast<unsigned long>(hr));
        else
            log_event(Ev::CaptureFailed, "%s hr=0x%08lX", stage, static_cast<unsigned long>(hr));
        release_gpu(c);
    }
}

// ---- GPU objects ---------------------------------------------------------------------------------
bool make_texture(Capture& c, UINT w, UINT h, DXGI_FORMAT format, D3D11_USAGE usage, UINT bind, UINT cpu, ComPtr<ID3D11Texture2D>* out,
                  const char* stage) {
    D3D11_TEXTURE2D_DESC d{};
    d.Width = w;
    d.Height = h;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = format;
    d.SampleDesc.Count = 1;
    d.Usage = usage;
    d.BindFlags = bind;
    d.CPUAccessFlags = cpu;
    const HRESULT hr = c.dev->CreateTexture2D(&d, nullptr, out->GetAddressOf());
    if (FAILED(hr)) {
        fail(c, hr, stage);
        return false;
    }
    return true;
}

// Everything that depends on the device and the output size, not on the back buffer.
bool create_device_objects(Capture& c) {
    ID3D11Device* dev = c.dev.Get();
    ComPtr<ID3D11Device1> dev1;
    HRESULT hr = dev->QueryInterface(IID_PPV_ARGS(&dev1));
    if (FAILED(hr)) return fail(c, hr, "device is not D3D11.1"), false;
    ComPtr<ID3D11DeviceContext> immediate;
    dev->GetImmediateContext(&immediate);
    hr = immediate.As(&c.ctx);
    if (FAILED(hr)) return fail(c, hr, "context is not D3D11.1"), false;
    immediate.As(&c.mt);  // optional: absent on a single-threaded device

    // Our own pipeline state, swapped in around our draw, so the game's state is never disturbed.
    const D3D_FEATURE_LEVEL level = dev->GetFeatureLevel();
    const UINT flags = (dev->GetCreationFlags() & D3D11_CREATE_DEVICE_SINGLETHREADED) ? D3D11_1_CREATE_DEVICE_CONTEXT_STATE_SINGLETHREADED : 0;
    D3D_FEATURE_LEVEL chosen{};
    hr = dev1->CreateDeviceContextState(flags, &level, 1, D3D11_SDK_VERSION, __uuidof(ID3D11Device), &chosen, &c.state);
    if (FAILED(hr)) return fail(c, hr, "CreateDeviceContextState"), false;

    if (FAILED(hr = dev->CreateVertexShader(g_capture_vs, sizeof(g_capture_vs), nullptr, &c.vs)) ||
        FAILED(hr = dev->CreatePixelShader(g_capture_ps_nv12, sizeof(g_capture_ps_nv12), nullptr, &c.ps)))
        return fail(c, hr, "create shaders"), false;

    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    D3D11_BLEND_DESC bd{};
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    D3D11_DEPTH_STENCIL_DESC dd{};  // depth and stencil off
    if (FAILED(hr = dev->CreateSamplerState(&sd, &c.sampler)) || FAILED(hr = dev->CreateRasterizerState(&rd, &c.raster)) ||
        FAILED(hr = dev->CreateBlendState(&bd, &c.blend)) || FAILED(hr = dev->CreateDepthStencilState(&dd, &c.depth)))
        return fail(c, hr, "create states"), false;

    // The NV12 render target (W x 3H/2, one byte per texel) and the staging ring it is copied into.
    const UINT rows = core().out_h * 3 / 2;
    if (!make_texture(c, core().out_w, rows, DXGI_FORMAT_R8_UNORM, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET, 0, &c.nv12_tex, "create NV12 target"))
        return false;
    if (FAILED(hr = dev->CreateRenderTargetView(c.nv12_tex.Get(), nullptr, &c.nv12_rtv))) return fail(c, hr, "create render target"), false;
    for (uint32_t i = 0; i < core().staging_n; ++i) {
        Staging& s = c.staging[i];
        if (!make_texture(c, core().out_w, rows, DXGI_FORMAT_R8_UNORM, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, &s.tex, "create staging texture"))
            return false;
        D3D11_QUERY_DESC qd{D3D11_QUERY_EVENT, 0};
        if (FAILED(hr = dev->CreateQuery(&qd, &s.query))) return fail(c, hr, "create query"), false;
    }

    // Set the constant parts of our state once: swap it in, set, swap back.
    ComPtr<ID3DDeviceContextState> previous;
    c.ctx->SwapDeviceContextState(c.state.Get(), &previous);
    c.ctx->IASetInputLayout(nullptr);
    c.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    c.ctx->VSSetShader(c.vs.Get(), nullptr, 0);
    c.ctx->PSSetShader(c.ps.Get(), nullptr, 0);
    ID3D11SamplerState* samplers[] = {c.sampler.Get()};
    c.ctx->PSSetSamplers(0, 1, samplers);
    c.ctx->RSSetState(c.raster.Get());
    const D3D11_VIEWPORT vp{0, 0, float(core().out_w), float(rows), 0, 1};
    c.ctx->RSSetViewports(1, &vp);
    c.ctx->OMSetBlendState(c.blend.Get(), nullptr, 0xFFFFFFFFu);
    c.ctx->OMSetDepthStencilState(c.depth.Get(), 0);
    c.ctx->SwapDeviceContextState(previous.Get(), nullptr);
    return true;
}

// What the shader reads from the back buffer: a typeless copy viewed as plain UNORM, so the values
// are the encoded ones the display shows (an sRGB view would linearise them).
bool source_formats(DXGI_FORMAT f, DXGI_FORMAT* copy_format, DXGI_FORMAT* view_format, bool* hdr) {
    *hdr = false;
    switch (f) {
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
        *copy_format = DXGI_FORMAT_B8G8R8A8_TYPELESS;
        *view_format = DXGI_FORMAT_B8G8R8A8_UNORM;
        return true;
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        *copy_format = DXGI_FORMAT_R8G8B8A8_TYPELESS;
        *view_format = DXGI_FORMAT_R8G8B8A8_UNORM;
        return true;
    case DXGI_FORMAT_R10G10B10A2_UNORM:
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
        *copy_format = DXGI_FORMAT_R10G10B10A2_TYPELESS;
        *view_format = DXGI_FORMAT_R10G10B10A2_UNORM;
        return true;
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
        *copy_format = *view_format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        *hdr = true;
        return true;
    default: return false;
    }
}

// Everything that depends on the back buffer's size and format (made again if the game resizes).
bool create_source_objects(Capture& c, const D3D11_TEXTURE2D_DESC& bb) {
    DXGI_FORMAT copy_format, view_format;
    bool hdr;
    if (!source_formats(bb.Format, &copy_format, &view_format, &hdr)) {
        log_event(Ev::UnsupportedFormat, "back buffer format %u", unsigned(bb.Format));
        fail(c, E_FAIL, "unsupported back buffer format");
        return false;
    }
    if (c.src_w && (c.src_w != bb.Width || c.src_h != bb.Height))
        log_event(Ev::BackbufferResized, "%ux%u -> %ux%u", c.src_w, c.src_h, bb.Width, bb.Height);
    if (hdr && c.src_format != bb.Format) log_event(Ev::HdrApproximated, "float back buffer clamped to SDR");

    c.srv.Reset();
    c.copy_tex.Reset();
    c.cb.Reset();
    if (!make_texture(c, bb.Width, bb.Height, copy_format, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, &c.copy_tex, "create copy texture"))
        return false;
    D3D11_SHADER_RESOURCE_VIEW_DESC vd{};
    vd.Format = view_format;
    vd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    vd.Texture2D.MipLevels = 1;
    HRESULT hr = c.dev->CreateShaderResourceView(c.copy_tex.Get(), &vd, &c.srv);
    if (FAILED(hr)) return fail(c, hr, "create view"), false;

    // Letterbox: the picture keeps its aspect ratio inside the output.
    const float scale = (std::min)(float(core().out_w) / float(bb.Width), float(core().out_h) / float(bb.Height));
    const float pw = float(bb.Width) * scale, ph = float(bb.Height) * scale;
    const float params[8] = {(float(core().out_w) - pw) * 0.5f, (float(core().out_h) - ph) * 0.5f, pw, ph, hdr ? 1.0f : 0.0f, float(core().out_h), 0, 0};
    D3D11_BUFFER_DESC cbd{};
    cbd.ByteWidth = sizeof(params);
    cbd.Usage = D3D11_USAGE_IMMUTABLE;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    D3D11_SUBRESOURCE_DATA init{params, 0, 0};
    hr = c.dev->CreateBuffer(&cbd, &init, &c.cb);
    if (FAILED(hr)) return fail(c, hr, "create constants"), false;

    c.src_format = bb.Format;
    c.src_resolve_format = (bb.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS || bb.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS ||
                            bb.Format == DXGI_FORMAT_R10G10B10A2_TYPELESS)
                               ? view_format
                               : bb.Format;
    c.src_w = bb.Width;
    c.src_h = bb.Height;
    c.src_samples = bb.SampleDesc.Count;
    return true;
}

// ---- Delivering a finished frame into the shared ring ---------------------------------------------
enum class Readback { NotReady, Started, Dropped, Error };

// Stage 1, once the GPU has finished the copy: take a ring slot, map the staging texture and hand the
// mapped memory to the worker. Never waits.
Readback begin_readback(Capture& c, Staging& s, uint32_t index) {
    const uint64_t t0 = qpc();
    BOOL done = FALSE;
    const HRESULT q = c.ctx->GetData(s.query.Get(), &done, sizeof(done), D3D11_ASYNC_GETDATA_DONOTFLUSH);
    if (FAILED(q)) return fail(c, q, "query"), Readback::Error;
    if (q != S_OK || !done) return Readback::NotReady;

    proto::SlotHeader* slot = core_acquire_slot();
    if (!slot) return Readback::Dropped;
    const HRESULT hr = c.ctx->Map(s.tex.Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &s.map);
    if (FAILED(hr)) {
        slot->state.store(uint32_t(proto::SlotState::Free), std::memory_order_release);
        if (hr == DXGI_ERROR_WAS_STILL_DRAWING) return Readback::NotReady;
        return fail(c, hr, "map"), Readback::Error;
    }
    s.slot = slot;
    s.mapped = true;
    core_post_copy(index, reinterpret_cast<uint8_t*>(slot) + proto::nv12_y_offset(), static_cast<const uint8_t*>(s.map.pData), s.map.RowPitch,
                   core().out_w, core().out_h * 3 / 2);
    s.info.begin_us = us_since(t0);
    s.info.cost_us += s.info.begin_us;
    core().ph_begin += to_us(qpc() - t0);
    return Readback::Started;
}

// Stage 2, once the worker is done: unmap and publish the slot to the host.
void finish_readback(Capture& c, Staging& s) {
    const uint64_t t0 = qpc();
    c.ctx->Unmap(s.tex.Get(), 0);
    s.mapped = false;
    core_publish(s.slot, s.info, proto::Layout::Nv12, core().out_w, core().out_w, t0);
    s.slot = nullptr;
}

}  // namespace

// Moves the oldest frames along: GPU finished -> copy started -> copy finished -> published.
void Capture::service() {
    Capture& c = *this;
    while (c.st_pending > 0 && !core().failed) {
        const uint32_t index = c.st_oldest;
        Staging& s = c.staging[index];
        if (!s.mapped) {
            const Readback r = begin_readback(c, s, index);
            if (r == Readback::NotReady || r == Readback::Error) return;
            if (r == Readback::Dropped) {
                s.pending = false;
                c.st_oldest = (c.st_oldest + 1) % core().staging_n;
                --c.st_pending;
                continue;
            }
        }
        const uint32_t job = core_job_state(index);
        if (job == 1) return;  // the worker is still copying: next Present
        if (job == 3) return fail(c, E_FAIL, "copy worker fault");
        core_job_clear(index);
        finish_readback(c, s);
        s.pending = false;
        c.st_oldest = (c.st_oldest + 1) % core().staging_n;
        --c.st_pending;
    }
}

// ---- Capturing a frame ---------------------------------------------------------------------------
void Capture::submit(const FrameInfo& info, void* target) {
    Capture& c = *this;
    IDXGISwapChain* sc = static_cast<IDXGISwapChain*>(target);
    const uint64_t t_setup = qpc();
    ComPtr<ID3D11Device> game_dev;
    HRESULT hr = sc->GetDevice(IID_PPV_ARGS(&game_dev));
    if (FAILED(hr)) return fail(c, hr, "GetDevice");
    if (game_dev.Get() != c.dev.Get()) {
        // First frame of the recording, or the game made a new device.
        release_gpu(c);
        c.dev = game_dev;
        if (!core_start_worker()) return fail(c, HRESULT_FROM_WIN32(GetLastError()), "restart copy worker");
        if (!create_device_objects(c)) return;
    }
    ComPtr<ID3D11Texture2D> bb;
    hr = sc->GetBuffer(0, IID_PPV_ARGS(&bb));
    if (FAILED(hr)) return fail(c, hr, "GetBuffer");
    D3D11_TEXTURE2D_DESC desc{};
    bb->GetDesc(&desc);
    if (!c.copy_tex || desc.Width != c.src_w || desc.Height != c.src_h || desc.Format != c.src_format || desc.SampleDesc.Count != c.src_samples) {
        if (!create_source_objects(c, desc)) return;
    }

    Staging& s = c.staging[c.st_next];
    if (s.pending) {  // every staging slot is still waiting for the GPU or the host
        g.ctl->gpu_backlog_skips.fetch_add(1, std::memory_order_relaxed);
        ++core().warn_backlog;
        return;
    }
    const uint64_t t_issue = qpc();

    if (c.mt) {
        g_lock_held = c.mt.Get();
        c.mt->Enter();
    }
    if (desc.SampleDesc.Count > 1)
        c.ctx->ResolveSubresource(c.copy_tex.Get(), 0, bb.Get(), 0, c.src_resolve_format);
    else
        c.ctx->CopyResource(c.copy_tex.Get(), bb.Get());

    ComPtr<ID3DDeviceContextState> previous;
    c.ctx->SwapDeviceContextState(c.state.Get(), &previous);
    ID3D11Buffer* buffers[] = {c.cb.Get()};
    ID3D11ShaderResourceView* views[] = {c.srv.Get()};
    ID3D11RenderTargetView* rtv = c.nv12_rtv.Get();
    c.ctx->PSSetConstantBuffers(0, 1, buffers);
    c.ctx->PSSetShaderResources(0, 1, views);
    c.ctx->OMSetRenderTargets(1, &rtv, nullptr);
    c.ctx->Draw(3, 0);
    ID3D11ShaderResourceView* no_view[] = {nullptr};
    c.ctx->PSSetShaderResources(0, 1, no_view);
    c.ctx->OMSetRenderTargets(0, nullptr, nullptr);
    c.ctx->SwapDeviceContextState(previous.Get(), nullptr);

    c.ctx->CopyResource(s.tex.Get(), c.nv12_tex.Get());
    c.ctx->End(s.query.Get());
    if (c.mt) {
        g_lock_held = nullptr;
        c.mt->Leave();
    }

    const uint64_t t_done = qpc();
    Core& k = core();
    k.ph_setup += to_us(t_issue - t_setup);
    k.ph_issue += to_us(t_done - t_issue);
    ++k.ph_submits;
    s.pending = true;
    s.mapped = false;
    s.info = info;
    s.info.cost_us = uint32_t(to_us(t_done - t_setup));
    s.info.setup_us = uint32_t(to_us(t_issue - t_setup));
    s.info.issue_us = uint32_t(to_us(t_done - t_issue));
    c.st_next = (c.st_next + 1) % k.staging_n;
    ++c.st_pending;
    k.last_tick = int64_t(info.tick);
}

uint64_t capture_on_present(IDXGISwapChain* sc, uint64_t now, uint32_t frame_time_us) {
    return core_on_present(cap(), sc, now, frame_time_us);
}

void capture_on_exception() {
    if (ID3D11Multithread* held = g_lock_held) {
        g_lock_held = nullptr;
        held->Leave();
    }
}

void capture_shutdown() {
    if (!g_cap) return;
    core_shutdown(*g_cap);
}

}  // namespace rec::hook
