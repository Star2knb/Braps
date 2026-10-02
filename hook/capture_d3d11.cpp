#include "capture_d3d11.h"

#include <d3d11_4.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cstring>

#include "hlog.h"
#include "hook_state.h"
#include "ps_nv12.h"
#include "vs_main.h"

using Microsoft::WRL::ComPtr;

namespace rec::hook {
namespace {

constexpr uint32_t kMaxStaging = 8;
constexpr uint64_t kWarnIntervalSeconds = 1;  // W1201 / W1202 are summarised at most once a second

// One GPU read-back slot: the NV12 texture of one captured frame on its way to the host.
struct Staging {
    ComPtr<ID3D11Texture2D> tex;
    ComPtr<ID3D11Query> query;
    bool pending = false;  // copy issued, frame not yet delivered
    bool mapped = false;   // mapped and handed to the copy worker
    proto::SlotHeader* slot = nullptr;
    D3D11_MAPPED_SUBRESOURCE map{};
    uint64_t present_qpc = 0;
    uint64_t tick = 0;
    uint64_t submit_index = 0;  // value of Capture::present_index when the copy was issued
    uint32_t frame_time_us = 0;
    uint32_t pacing_wait_us = 0, pacing_error_us = 0;  // lock mode: how long the game was held, how late the capture began
    uint32_t cost_us = 0;  // hook time spent on this frame so far (the copy worker's time is not ours)
    uint32_t setup_us = 0, issue_us = 0, begin_us = 0;  // the parts of it, for the log of slow frames
};

// Mapped staging memory is slow to read (the driver maps its pages on demand), so the render thread
// maps, a worker thread copies into the shared ring, and the next Present unmaps and publishes. The
// worker never touches D3D, only memory.
struct Job {
    std::atomic<uint32_t> state{0};  // 0 idle, 1 requested, 2 done, 3 faulted
    uint8_t* dst = nullptr;
    const uint8_t* src = nullptr;
    uint32_t src_pitch = 0;
};

struct Capture {
    // Recording (from the control block).
    bool active = false;
    bool failed = false;
    uint32_t generation = 0, fps = 0, out_w = 0, out_h = 0, staging_n = 0, ring_slots = 0, slot_bytes = 0;
    uint64_t t0 = 0;
    int64_t last_tick = -1;
    bool lock = false;           // lock mode: hold the game to the tick grid
    bool anchored = false;       // the first tick has been chosen
    int64_t next_k = 0;          // lock mode: the next tick to capture
    uint64_t grid0 = 0;          // lock mode: QPC time of tick 0; follows the game's phase (see capture_on_present)
    uint64_t grid_ref = 0;       // ... where it started: grid0 never moves more than one tick later than this
    HANDLE pace_timer = nullptr; // high-resolution timer for the wait
    HANDLE ring_map = nullptr;
    HANDLE sem = nullptr;
    proto::FrameRingHeader* ring = nullptr;
    uint32_t ring_next = 0;
    uint32_t seq = 0;
    uint64_t present_index = 0;

    // Copy worker.
    HANDLE worker = nullptr;
    HANDLE worker_wake = nullptr;
    std::atomic<bool> worker_quit{false};
    Job jobs[kMaxStaging];

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

    // Where the render thread's time goes (microseconds summed over frames), logged at DEBUG.
    uint64_t ph_setup = 0, ph_issue = 0, ph_begin = 0, ph_finish = 0;
    uint32_t ph_submits = 0, ph_reads = 0;
    uint32_t slow_logged = 0;

    // Rate-limited warnings.
    uint64_t last_warn_qpc = 0;
    uint32_t warn_backlog = 0, warn_ring = 0;
};

// The device lock while our draw sequence runs, so that a game presenting from a different thread than
// the one it renders on can't have its context calls interleaved with ours. Released by
// capture_on_exception if the sequence is cut short by an exception.
ID3D11Multithread* g_lock_held = nullptr;

Capture* g_cap = nullptr;  // heap, never destroyed by the CRT: COM objects must not be released during process teardown
std::atomic<bool> g_active{false};
std::atomic<bool> g_busy{false};

Capture& cap() {
    if (!g_cap) g_cap = new Capture;
    return *g_cap;
}

uint64_t to_us(uint64_t ticks) { return ticks * 1000000ull / uint64_t(g.qpc_freq); }
uint32_t us_since(uint64_t t0) { return uint32_t(to_us(qpc() - t0)); }

// ---- The copy worker -----------------------------------------------------------------------------
// Copies rows out of mapped memory. If the memory becomes invalid (device removed under us) the
// exception is caught here and reported, so the game's process never dies of it.
bool safe_copy(const Job& j, uint32_t row_bytes, uint32_t rows) {
    __try {
        if (j.src_pitch == row_bytes) {
            std::memcpy(j.dst, j.src, size_t(row_bytes) * rows);
        } else {
            for (uint32_t r = 0; r < rows; ++r) std::memcpy(j.dst + size_t(r) * row_bytes, j.src + size_t(r) * j.src_pitch, row_bytes);
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

DWORD WINAPI copy_worker(LPVOID param) {
    Capture& c = *static_cast<Capture*>(param);
    for (;;) {
        WaitForSingleObject(c.worker_wake, INFINITE);
        bool did;
        do {
            did = false;
            for (uint32_t i = 0; i < kMaxStaging; ++i) {
                Job& j = c.jobs[i];
                if (j.state.load(std::memory_order_acquire) != 1) continue;
                const bool ok = safe_copy(j, c.out_w, c.out_h * 3 / 2);
                j.state.store(ok ? 2 : 3, std::memory_order_release);
                did = true;
            }
        } while (did);
        if (c.worker_quit.load(std::memory_order_acquire)) break;
    }
    return 0;
}

bool start_worker(Capture& c) {
    if (c.worker) return true;
    c.worker_quit = false;
    for (Job& j : c.jobs) j.state = 0;
    c.worker_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!c.worker_wake) return false;
    c.worker = CreateThread(nullptr, 0, copy_worker, &c, 0, nullptr);
    return c.worker != nullptr;
}

// Stops the worker and gives back whatever was mapped for it.
void stop_worker(Capture& c) {
    if (c.worker) {
        c.worker_quit = true;
        SetEvent(c.worker_wake);
        WaitForSingleObject(c.worker, 3000);
        CloseHandle(c.worker);
        c.worker = nullptr;
    }
    if (c.worker_wake) CloseHandle(c.worker_wake);
    c.worker_wake = nullptr;
    for (uint32_t i = 0; i < kMaxStaging; ++i) {
        Staging& s = c.staging[i];
        if (s.mapped) {
            if (c.ctx && s.tex) c.ctx->Unmap(s.tex.Get(), 0);
            if (s.slot) s.slot->state.store(uint32_t(proto::SlotState::Free), std::memory_order_release);
            s.mapped = false;
        }
        c.jobs[i].state = 0;
    }
}

void release_gpu(Capture& c) {
    stop_worker(c);
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

void log_phases(const Capture& c) {
    if (c.ph_submits) log_text(Level::Debug, "per frame us: setup %llu issue %llu", c.ph_setup / c.ph_submits, c.ph_issue / c.ph_submits);
    if (c.ph_reads) log_text(Level::Debug, "per frame us: begin %llu finish %llu", c.ph_begin / c.ph_reads, c.ph_finish / c.ph_reads);
}

void end_recording(Capture& c) {
    log_phases(c);
    release_gpu(c);
    if (c.pace_timer) CloseHandle(c.pace_timer);
    c.pace_timer = nullptr;
    if (c.ring) UnmapViewOfFile(c.ring);
    if (c.ring_map) CloseHandle(c.ring_map);
    if (c.sem) CloseHandle(c.sem);
    c.ring = nullptr;
    c.ring_map = c.sem = nullptr;
    c.active = c.failed = false;
    g_active.store(false, std::memory_order_release);
    g.ctl->capture_state.store(uint32_t(proto::CaptureState::Off), std::memory_order_release);
}

// The first failure disables capture until the host stops this recording; measuring goes on.
void fail(Capture& c, HRESULT hr, const char* stage) {
    proto::ControlBlock* ctl = g.ctl;
    if (!c.failed) {
        c.failed = true;
        ctl->capture_error_code.store(event_number(Ev::CaptureFailed));
        ctl->capture_state.store(uint32_t(proto::CaptureState::Error), std::memory_order_release);
        if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET)
            log_event(Ev::DeviceRemoved, "%s hr=0x%08lX", stage, static_cast<unsigned long>(hr));
        else
            log_event(Ev::CaptureFailed, "%s hr=0x%08lX", stage, static_cast<unsigned long>(hr));
        release_gpu(c);
    }
    ctl->capture_errors.fetch_add(1, std::memory_order_relaxed);
}

void warn_rate_limited(Capture& c, uint64_t now) {
    if (!c.warn_backlog && !c.warn_ring) return;
    if (now - c.last_warn_qpc < kWarnIntervalSeconds * uint64_t(g.qpc_freq)) return;
    c.last_warn_qpc = now;
    if (c.warn_backlog) log_event(Ev::GpuBacklog, "%u frames skipped (GPU behind)", c.warn_backlog);
    if (c.warn_ring) log_event(Ev::RingFull, "%u frames dropped (host behind)", c.warn_ring);
    c.warn_backlog = c.warn_ring = 0;
}

// ---- Starting a recording ------------------------------------------------------------------------
bool begin_recording(Capture& c) {
    proto::ControlBlock* ctl = g.ctl;
    c.active = true;
    g_active.store(true, std::memory_order_release);
    c.failed = false;
    c.generation = ctl->rec_generation.load(std::memory_order_acquire);
    c.fps = ctl->rec_fps;
    c.out_w = ctl->rec_out_w;
    c.out_h = ctl->rec_out_h;
    c.staging_n = ctl->rec_staging_slots;
    c.ring_slots = ctl->rec_frame_slots;
    c.slot_bytes = ctl->rec_slot_bytes;
    c.t0 = ctl->rec_t0_qpc;
    c.last_tick = -1;
    c.lock = ctl->rec_lock != 0;
    c.anchored = false;
    c.next_k = 0;
    c.ring_next = 0;
    c.seq = 0;
    c.present_index = 0;
    c.ph_setup = c.ph_issue = c.ph_begin = c.ph_finish = 0;
    c.ph_submits = c.ph_reads = 0;
    c.slow_logged = 0;
    c.warn_backlog = c.warn_ring = 0;
    c.last_warn_qpc = qpc();

    const bool sane = c.fps >= 1 && c.fps <= 1000 && c.out_w >= 16 && c.out_h >= 16 && c.out_w <= 8192 && c.out_h <= 5460 &&
                      !(c.out_w & 1) && !(c.out_h & 1) && c.staging_n >= 2 && c.staging_n <= kMaxStaging && c.ring_slots >= 2 &&
                      c.ring_slots <= 256 && c.slot_bytes == proto::nv12_slot_bytes(c.out_w, c.out_h);
    if (!sane) {
        fail(c, E_INVALIDARG, "recording settings");
        return false;
    }
    wchar_t name[64];
    proto::frame_ring_name(name, 64, GetCurrentProcessId(), c.generation, false);
    c.ring_map = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, name);
    if (!c.ring_map) {
        fail(c, HRESULT_FROM_WIN32(GetLastError()), "open frame ring");
        return false;
    }
    c.ring = static_cast<proto::FrameRingHeader*>(
        MapViewOfFile(c.ring_map, FILE_MAP_ALL_ACCESS, 0, 0, proto::frame_ring_bytes(c.ring_slots, c.slot_bytes)));
    if (!c.ring || c.ring->magic != proto::kFrameRingMagic || c.ring->slot_count != c.ring_slots || c.ring->slot_bytes != c.slot_bytes ||
        c.ring->out_w != c.out_w || c.ring->out_h != c.out_h) {
        fail(c, c.ring ? E_INVALIDARG : HRESULT_FROM_WIN32(GetLastError()), "map frame ring");
        return false;
    }
    proto::frame_ring_name(name, 64, GetCurrentProcessId(), c.generation, true);
    c.sem = OpenSemaphoreW(SEMAPHORE_MODIFY_STATE | SYNCHRONIZE, FALSE, name);
    if (!c.sem) {
        fail(c, HRESULT_FROM_WIN32(GetLastError()), "open frame semaphore");
        return false;
    }
    if (!start_worker(c)) {
        fail(c, HRESULT_FROM_WIN32(GetLastError()), "start copy worker");
        return false;
    }
    if (c.lock) c.pace_timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);  // null: spin instead
    ctl->capture_state.store(uint32_t(proto::CaptureState::Capturing), std::memory_order_release);
    return true;
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
    const UINT rows = c.out_h * 3 / 2;
    if (!make_texture(c, c.out_w, rows, DXGI_FORMAT_R8_UNORM, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET, 0, &c.nv12_tex, "create NV12 target"))
        return false;
    if (FAILED(hr = dev->CreateRenderTargetView(c.nv12_tex.Get(), nullptr, &c.nv12_rtv))) return fail(c, hr, "create render target"), false;
    for (uint32_t i = 0; i < c.staging_n; ++i) {
        Staging& s = c.staging[i];
        if (!make_texture(c, c.out_w, rows, DXGI_FORMAT_R8_UNORM, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, &s.tex, "create staging texture"))
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
    const D3D11_VIEWPORT vp{0, 0, float(c.out_w), float(rows), 0, 1};
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
    const float scale = (std::min)(float(c.out_w) / float(bb.Width), float(c.out_h) / float(bb.Height));
    const float pw = float(bb.Width) * scale, ph = float(bb.Height) * scale;
    const float params[8] = {(float(c.out_w) - pw) * 0.5f, (float(c.out_h) - ph) * 0.5f, pw, ph, hdr ? 1.0f : 0.0f, float(c.out_h), 0, 0};
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

    proto::ControlBlock* ctl = g.ctl;
    proto::SlotHeader* slot = proto::ring_slot(c.ring, c.ring_next);
    uint32_t expected = uint32_t(proto::SlotState::Free);
    if (!slot->state.compare_exchange_strong(expected, uint32_t(proto::SlotState::Writing), std::memory_order_acq_rel)) {
        ctl->ring_full_drops.fetch_add(1, std::memory_order_relaxed);
        ++c.warn_ring;
        return Readback::Dropped;
    }
    const HRESULT hr = c.ctx->Map(s.tex.Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &s.map);
    if (FAILED(hr)) {
        slot->state.store(uint32_t(proto::SlotState::Free), std::memory_order_release);
        if (hr == DXGI_ERROR_WAS_STILL_DRAWING) return Readback::NotReady;
        return fail(c, hr, "map"), Readback::Error;
    }
    s.slot = slot;
    s.mapped = true;
    Job& j = c.jobs[index];
    j.dst = reinterpret_cast<uint8_t*>(slot) + proto::nv12_y_offset();
    j.src = static_cast<const uint8_t*>(s.map.pData);
    j.src_pitch = s.map.RowPitch;
    j.state.store(1, std::memory_order_release);
    SetEvent(c.worker_wake);
    s.begin_us = us_since(t0);
    s.cost_us += s.begin_us;
    c.ph_begin += to_us(qpc() - t0);
    return Readback::Started;
}

// Stage 2, once the worker is done: unmap and publish the slot to the host.
void finish_readback(Capture& c, Staging& s) {
    const uint64_t t0 = qpc();
    c.ctx->Unmap(s.tex.Get(), 0);
    s.mapped = false;

    proto::SlotHeader* slot = s.slot;
    slot->seq = ++c.seq;
    slot->tick = s.tick;
    slot->present_qpc = s.present_qpc;
    slot->capture_done_qpc = qpc();
    slot->width = uint16_t(c.out_w);
    slot->height = uint16_t(c.out_h);
    slot->layout = uint8_t(proto::Layout::Nv12);
    slot->flags = 0;
    slot->stride0 = c.out_w;
    slot->stride1 = c.out_w;
    slot->game_frame_time_us = s.frame_time_us;
    slot->readback_frames = uint32_t(c.present_index - s.submit_index);
    slot->pacing_wait_us = s.pacing_wait_us;
    slot->pacing_error_us = uint16_t((std::min)(s.pacing_error_us, 65535u));
    const uint32_t finish_us = us_since(t0);
    s.cost_us += finish_us;
    slot->hook_cost_us = s.cost_us;
    if (s.cost_us > 1000 && c.slow_logged < 12) {  // the tail of the cost distribution: where did the time go?
        ++c.slow_logged;
        log_text(Level::Debug, "slow %u us: setup %u issue %u begin %u fin %u", s.cost_us, s.setup_us, s.issue_us, s.begin_us, finish_us);
    }
    slot->state.store(uint32_t(proto::SlotState::Ready), std::memory_order_release);
    ReleaseSemaphore(c.sem, 1, nullptr);
    c.ring_next = (c.ring_next + 1) % c.ring_slots;
    g.ctl->frames_captured.fetch_add(1, std::memory_order_relaxed);
    c.ph_finish += to_us(qpc() - t0);
    ++c.ph_reads;
    s.slot = nullptr;
}

// Moves the oldest frames along: GPU finished -> copy started -> copy finished -> published.
void service_readbacks(Capture& c) {
    while (c.st_pending > 0 && !c.failed) {
        const uint32_t index = c.st_oldest;
        Staging& s = c.staging[index];
        if (!s.mapped) {
            const Readback r = begin_readback(c, s, index);
            if (r == Readback::NotReady || r == Readback::Error) return;
            if (r == Readback::Dropped) {
                s.pending = false;
                c.st_oldest = (c.st_oldest + 1) % c.staging_n;
                --c.st_pending;
                continue;
            }
        }
        Job& j = c.jobs[index];
        const uint32_t state = j.state.load(std::memory_order_acquire);
        if (state == 1) return;  // the worker is still copying: next Present
        if (state == 3) return fail(c, E_FAIL, "copy worker fault");
        j.state.store(0, std::memory_order_relaxed);
        finish_readback(c, s);
        s.pending = false;
        c.st_oldest = (c.st_oldest + 1) % c.staging_n;
        --c.st_pending;
    }
}

// ---- Capturing a frame ---------------------------------------------------------------------------
void submit_frame(Capture& c, IDXGISwapChain* sc, uint64_t now, int64_t tick, uint32_t frame_time_us, uint32_t wait_us, uint32_t error_us) {
    const uint64_t t_setup = qpc();
    ComPtr<ID3D11Device> dev;
    HRESULT hr = sc->GetDevice(IID_PPV_ARGS(&dev));
    if (FAILED(hr)) return fail(c, hr, "GetDevice");
    if (dev.Get() != c.dev.Get()) {
        // First frame of the recording, or the game made a new device.
        release_gpu(c);
        c.dev = dev;
        if (!start_worker(c)) return fail(c, HRESULT_FROM_WIN32(GetLastError()), "restart copy worker");
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
        ++c.warn_backlog;
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
    c.ph_setup += to_us(t_issue - t_setup);
    c.ph_issue += to_us(t_done - t_issue);
    ++c.ph_submits;
    s.pending = true;
    s.mapped = false;
    s.present_qpc = now;
    s.tick = uint64_t(tick);
    s.frame_time_us = frame_time_us;
    s.pacing_wait_us = wait_us;
    s.pacing_error_us = error_us;
    s.submit_index = c.present_index;
    s.cost_us = uint32_t(to_us(t_done - t_setup));
    s.setup_us = uint32_t(to_us(t_issue - t_setup));
    s.issue_us = uint32_t(to_us(t_done - t_issue));
    c.st_next = (c.st_next + 1) % c.staging_n;
    ++c.st_pending;
    c.last_tick = tick;
    g.ctl->last_capture_tick.store(uint64_t(tick), std::memory_order_relaxed);
}

// Lock mode (§6.2): hold the game until `target` (QPC), with a high-resolution timer for the bulk of the
// wait and a spin for the last stretch. Returns the QPC ticks spent.
uint64_t pace_until(Capture& c, uint64_t target) {
    const uint64_t start = qpc();
    const int64_t remaining = int64_t(target) - int64_t(start);
    const int64_t freq = g.qpc_freq;
    if (remaining <= 0 || remaining > freq / 4) return 0;  // already there, or implausibly far (clock jump): don't hold the game
    const int64_t slack = freq / 5000;                     // wake 0.2 ms early and spin the rest
    if (c.pace_timer && remaining > freq / 1000 + slack) {
        LARGE_INTEGER due;
        due.QuadPart = -((remaining - slack) * 10000000ll / freq);  // 100 ns units, relative
        if (SetWaitableTimer(c.pace_timer, &due, 0, nullptr, nullptr, FALSE)) WaitForSingleObject(c.pace_timer, 300);
    }
    while (qpc() < target) YieldProcessor();
    return qpc() - start;
}

struct BusyGuard {
    bool owner;
    BusyGuard() : owner(!g_busy.exchange(true, std::memory_order_acquire)) {}
    ~BusyGuard() {
        if (owner) g_busy.store(false, std::memory_order_release);
    }
};

}  // namespace

uint64_t capture_on_present(IDXGISwapChain* sc, uint64_t now, uint32_t frame_time_us) {
    proto::ControlBlock* ctl = g.ctl;
    const auto want = proto::SessionState(ctl->host_state.load(std::memory_order_acquire));
    if (want == proto::SessionState::Idle && !g_active.load(std::memory_order_acquire)) return 0;  // the common case: one load

    BusyGuard guard;
    if (!guard.owner) return 0;  // another thread is capturing; skip this one
    Capture& c = cap();

    // The host stopping, or gone (E1401), ends new captures.
    const bool host_gone = proto::HookState(ctl->hook_state.load(std::memory_order_relaxed)) == proto::HookState::Idle;
    const bool recording = want == proto::SessionState::Recording && !host_gone;

    if (c.active && recording && ctl->rec_generation.load(std::memory_order_acquire) != c.generation) end_recording(c);  // a new recording
    if (!c.active) {
        if (!recording) return 0;
        begin_recording(c);
    }
    ++c.present_index;
    if (c.failed) {
        if (!recording) end_recording(c);
        return 0;
    }

    service_readbacks(c);
    uint64_t held = 0;
    if (recording && now >= c.t0) {
        const uint64_t freq = uint64_t(g.qpc_freq);
        auto tick_time = [&](int64_t k) { return (c.lock ? c.grid0 : c.t0) + uint64_t(k) * freq / c.fps; };
        int64_t tick = -1;
        uint32_t error_us = 0;
        if (!c.lock) {
            // Free mode: the first Present at or after each tick; a second Present in the same tick is not captured.
            tick = int64_t(((now - c.t0) * c.fps + freq / 2) / freq);
            if (tick <= c.last_tick) tick = -1;
        } else if (!c.anchored) {
            // Lock mode, first frame: it is tick 0 and the grid starts at this moment, so the grid has the
            // game's phase from the start. From here on the game is held to the grid.
            c.anchored = true;
            c.grid0 = c.grid_ref = now;
            g.ctl->grid0_qpc.store(c.grid0, std::memory_order_release);
            tick = 0;
            c.next_k = 1;
        } else {
            const int64_t k = c.next_k;
            if (now < tick_time(k)) {
                held = pace_until(c, tick_time(k));  // early: wait for the tick
                now = qpc();
            }
            // Decide the tick after the wait: the timer can oversleep, and a hitch can leave the game a tick or more behind.
            tick = k;
            if (now >= tick_time(k + 1)) {
                tick = int64_t((now - c.grid0) * c.fps / freq);  // the ticks in between get DUPs from the host
                if (tick < k) tick = k;
            }
            const uint64_t late = now > tick_time(tick) ? now - tick_time(tick) : 0;
            // Follow the game's phase: a game that is regularly a little late means the grid is ahead of it. Move
            // the grid halfway toward the frame, so the game soon arrives just ahead of it (and is held for the
            // difference). Only later, never earlier; and never more than one tick from where the grid started, so
            // the video stays tied to the wall clock (a game slower than the grid gets DUPs, not a slowed-down timeline).
            const uint64_t room = c.grid_ref + freq / c.fps - c.grid0;
            const uint64_t shift = (std::min)(late / 2, room);
            if (shift) {
                c.grid0 += shift;
                g.ctl->grid0_qpc.store(c.grid0, std::memory_order_release);
            }
            error_us = uint32_t(late * 1000000ull / freq);
            c.next_k = tick + 1;
        }
        if (tick >= 0) submit_frame(c, sc, now, tick, frame_time_us, uint32_t(held * 1000000ull / freq), error_us);
    }
    if (!recording) {
        ctl->capture_state.store(uint32_t(proto::CaptureState::Draining), std::memory_order_release);
        if (c.st_pending == 0) end_recording(c);
    }
    warn_rate_limited(c, now);
    return held;
}

void capture_on_exception() {
    if (ID3D11Multithread* held = g_lock_held) {
        g_lock_held = nullptr;
        held->Leave();
    }
}

void capture_shutdown() {
    if (!g_cap) return;
    if (g_cap->active) end_recording(*g_cap);
    else release_gpu(*g_cap);
}

}  // namespace rec::hook
