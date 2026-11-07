// The frame-generation synthesizer: see framegen_synth.h.
#include "temporal_shader_bytecode.h"
#include "framegen_synth.h"
#include "cs_stage_save.h"
#include "gpu_frame_timing.h"
#include "gpu_timing.h"
#include "shader_swap.h"
#include "../common/framegen_synth_report.h"
#include "../common/log.h"

#include <windows.h>
#include <d3d11.h>

#include <atomic>
#include <cmath>
#include <cstring>
#include <vector>

namespace edvr {
namespace {

constexpr int kStagingDepth = 4;
constexpr int kTimerRing = 8;
constexpr int kEntries = 8;      // clear, dilate, splat, source, resolve, resample, luma, score
constexpr int kSrvSlots = 10;    // t0..t9
constexpr int kUavSlots = 8;     // u0..u7

enum Entry : int { kClear = 0, kDilate, kSplat, kSrc, kResolve, kResample, kLuma, kScore };

std::atomic<bool> g_on{false};
std::atomic<bool> g_told{false};   // the armed line has been said since the key last went on

struct Tex {
    ID3D11Texture2D* tex = nullptr;
    ID3D11ShaderResourceView* srv = nullptr;
    ID3D11UnorderedAccessView* uav = nullptr;
};

// One eye's resources and the f = 1 S waiting to be scored.
struct EyeRes {
    bool created = false;
    uint32_t rw = 0, rh = 0, ow = 0, oh = 0, qw = 0, qh = 0;
    DXGI_FORMAT sFormat = DXGI_FORMAT_UNKNOWN;
    Tex depthD, key, src, read;   // render size: the dilated depth, the key grid, the winner's pixel, the read position
    Tex s;                        // output size, the upscaler's format: S
    Tex sLuma, bInfo;             // quarter grid: the kept S luma, B's luma (x) and class (y)
    ID3D11Buffer* stats = nullptr;
    ID3D11UnorderedAccessView* statsUav = nullptr;
    ID3D11Buffer* staging[kStagingDepth] = {};
    bool pending[kStagingDepth] = {};
    uint8_t slotKind[kStagingDepth] = {};      // 0 the f = 0.5 S, 1 the f = 1 S
    bool slotScored[kStagingDepth] = {};
    uint32_t writes = 0;
    uint32_t calls = 0;           // built frames so far: the even ones build f = 0.5
    bool havePending = false;     // an f = 1 S luma is kept, built on frame bFrame
    uint32_t bFrame = 0;
    bool shownValid = false;      // the latest call built the f = 0.5 S into `s`
    uint32_t shownFrame = 0;
};

struct TimerSlot {
    GpuTimer timer;
    bool inUse = false;
    int kind = 0;
};

struct State {
    ID3D11Device* dev = nullptr;   // identity only
    bool failed = false;           // creation failed on this device: stand down until it changes or the key is toggled
    ID3D11ComputeShader* cs[kEntries] = {};
    ID3D11Buffer* cb = nullptr;
    DXGI_FORMAT lumaFormat = DXGI_FORMAT_R16_FLOAT;
    EyeRes eye[2];
    TimerSlot timers[kTimerRing];
    FrameGenSynthWindow win;
    std::vector<float> gpuMs[2];
    int64_t windowTicks = 0;
};
// Never destroyed: the process teardown can run under the loader lock, where releasing D3D objects is not safe (see temporalPassShutdown).
State* const g_statePtr = new State();
State& g = *g_statePtr;

template <class T>
void releaseCom(T*& p) {
    if (p) { p->Release(); p = nullptr; }
}

void releaseTex(Tex& t) {
    releaseCom(t.uav);
    releaseCom(t.srv);
    releaseCom(t.tex);
}

void releaseEye(EyeRes& r) {
    releaseTex(r.depthD);
    releaseTex(r.key);
    releaseTex(r.src);
    releaseTex(r.read);
    releaseTex(r.s);
    releaseTex(r.sLuma);
    releaseTex(r.bInfo);
    releaseCom(r.statsUav);
    releaseCom(r.stats);
    for (auto& s : r.staging) releaseCom(s);
    r = EyeRes{};
}

bool anyResources() {
    if (g.cb) return true;
    for (const EyeRes& r : g.eye) if (r.created) return true;
    for (ID3D11ComputeShader* c : g.cs) if (c) return true;
    return false;
}

void windowReset() {
    g.win = FrameGenSynthWindow{};
    g.gpuMs[0].clear();
    g.gpuMs[1].clear();
}

// The shaders, the constant buffer and the formats: made once per device. The timers are left as they are (a timer with a query in flight is released only by its owner's
// context, gpu_timing.h); the ring simply keeps polling them.
void releaseAll() {
    for (EyeRes& r : g.eye) releaseEye(r);
    for (auto& c : g.cs) releaseCom(c);
    releaseCom(g.cb);
    g.dev = nullptr;
    g.failed = false;
}

bool uavStoreOk(ID3D11Device* dev, DXGI_FORMAT fmt) {
    UINT support = 0;
    const UINT want = D3D11_FORMAT_SUPPORT_TEXTURE2D | D3D11_FORMAT_SUPPORT_SHADER_LOAD | D3D11_FORMAT_SUPPORT_TYPED_UNORDERED_ACCESS_VIEW;
    return SUCCEEDED(dev->CheckFormatSupport(fmt, &support)) && (support & want) == want;
}

bool createCommon(ID3D11Device* dev, ID3D11DeviceContext* ctx) {
    struct Item { const unsigned char* code; size_t size; const char* name; };
    const Item entries[kEntries] = {
        {kFrameGenSynthClearBytecode, sizeof(kFrameGenSynthClearBytecode), "framegen_synth_clear"},
        {kFrameGenSynthDilateBytecode, sizeof(kFrameGenSynthDilateBytecode), "framegen_synth_dilate"},
        {kFrameGenSynthSplatBytecode, sizeof(kFrameGenSynthSplatBytecode), "framegen_synth_splat"},
        {kFrameGenSynthSrcBytecode, sizeof(kFrameGenSynthSrcBytecode), "framegen_synth_src"},
        {kFrameGenSynthResolveBytecode, sizeof(kFrameGenSynthResolveBytecode), "framegen_synth_resolve"},
        {kFrameGenSynthResampleBytecode, sizeof(kFrameGenSynthResampleBytecode), "framegen_synth_resample"},
        {kFrameGenSynthLumaBytecode, sizeof(kFrameGenSynthLumaBytecode), "framegen_synth_luma"},
        {kFrameGenSynthScoreBytecode, sizeof(kFrameGenSynthScoreBytecode), "framegen_synth_score"},
    };
    for (int i = 0; i < kEntries; ++i) {
        g.cs[i] = shaderSwapCreateCs(ctx, entries[i].code, entries[i].size, entries[i].name, "frame generation (synth)");
        if (!g.cs[i]) return false;
    }
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = sizeof(FrameGenSynthCb);
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(dev->CreateBuffer(&bd, nullptr, &g.cb))) return false;
    // The formats the textures are written in: R32_FLOAT and R32_UINT are in every D3D11 part's typed set; the two-float read position and class map need the adapter to store
    // them (every D3D11 part that matters does); the luma is R16_FLOAT where the adapter stores it, else R32_FLOAT, as the scorer's.
    if (!uavStoreOk(dev, DXGI_FORMAT_R32G32_FLOAT) || !uavStoreOk(dev, DXGI_FORMAT_R32_UINT) || !uavStoreOk(dev, DXGI_FORMAT_R32_FLOAT)) return false;
    g.lumaFormat = uavStoreOk(dev, DXGI_FORMAT_R16_FLOAT) ? DXGI_FORMAT_R16_FLOAT : DXGI_FORMAT_R32_FLOAT;
    return true;
}

bool makeTex(ID3D11Device* dev, uint32_t w, uint32_t h, DXGI_FORMAT fmt, Tex& out) {
    D3D11_TEXTURE2D_DESC td{};
    td.Width = w;
    td.Height = h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = fmt;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(dev->CreateTexture2D(&td, nullptr, &out.tex))) return false;
    if (FAILED(dev->CreateShaderResourceView(out.tex, nullptr, &out.srv))) return false;
    return SUCCEEDED(dev->CreateUnorderedAccessView(out.tex, nullptr, &out.uav));
}

// The format the upscaler's output is read in (the SRV's, else the texture's).
DXGI_FORMAT viewFormat(ID3D11ShaderResourceView* v) {
    D3D11_SHADER_RESOURCE_VIEW_DESC d{};
    v->GetDesc(&d);
    if (d.Format != DXGI_FORMAT_UNKNOWN) return d.Format;
    ID3D11Resource* res = nullptr;
    v->GetResource(&res);
    if (!res) return DXGI_FORMAT_UNKNOWN;
    ID3D11Texture2D* t = nullptr;
    DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
    if (SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&t))) && t) {
        D3D11_TEXTURE2D_DESC td{};
        t->GetDesc(&td);
        fmt = td.Format;
        t->Release();
    }
    res->Release();
    return fmt;
}

bool createEye(ID3D11Device* dev, EyeRes& r, uint32_t rw, uint32_t rh, uint32_t ow, uint32_t oh, DXGI_FORMAT sFormat) {
    r = EyeRes{};
    r.rw = rw; r.rh = rh; r.ow = ow; r.oh = oh;
    r.qw = (rw + 3u) / 4u;
    r.qh = (rh + 3u) / 4u;
    r.sFormat = sFormat;
    if (r.qw == 0 || r.qh == 0 || !uavStoreOk(dev, sFormat)) return false;
    bool ok = makeTex(dev, rw, rh, DXGI_FORMAT_R32_FLOAT, r.depthD) && makeTex(dev, rw, rh, DXGI_FORMAT_R32_UINT, r.key) && makeTex(dev, rw, rh, DXGI_FORMAT_R32_UINT, r.src) &&
              makeTex(dev, rw, rh, DXGI_FORMAT_R32G32_FLOAT, r.read) && makeTex(dev, ow, oh, sFormat, r.s) && makeTex(dev, r.qw, r.qh, g.lumaFormat, r.sLuma) &&
              makeTex(dev, r.qw, r.qh, DXGI_FORMAT_R32G32_FLOAT, r.bInfo);
    if (ok) {
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = kFrameGenSynthStatsUints * 4u;
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        bd.StructureByteStride = 4;
        ok = SUCCEEDED(dev->CreateBuffer(&bd, nullptr, &r.stats));
        if (ok) {
            D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
            ud.Format = DXGI_FORMAT_UNKNOWN;
            ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
            ud.Buffer.NumElements = kFrameGenSynthStatsUints;
            ok = SUCCEEDED(dev->CreateUnorderedAccessView(r.stats, &ud, &r.statsUav));
        }
        bd.Usage = D3D11_USAGE_STAGING;
        bd.BindFlags = 0;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        for (int i = 0; i < kStagingDepth && ok; ++i) ok = SUCCEEDED(dev->CreateBuffer(&bd, nullptr, &r.staging[i]));
    }
    if (!ok) {
        releaseEye(r);
        return false;
    }
    r.created = true;
    return true;
}

// The staging slots already copied, oldest first, read without waiting: a slot not done yet is left for the next frame.
void pollReadbacks(ID3D11DeviceContext* ctx, EyeRes& r, uint32_t nextSlot) {
    for (int k = 0; k < kStagingDepth; ++k) {
        const int slot = static_cast<int>((nextSlot + static_cast<uint32_t>(k)) % kStagingDepth);   // the slot written longest ago first
        if (!r.pending[slot]) continue;
        D3D11_MAPPED_SUBRESOURCE m{};
        const HRESULT hr = ctx->Map(r.staging[slot], 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m);
        if (hr == DXGI_ERROR_WAS_STILL_DRAWING) continue;
        r.pending[slot] = false;
        if (FAILED(hr) || !m.pData) continue;
        FrameGenSynthEyeResult res;
        frameGenSynthDecodeStats(static_cast<const uint32_t*>(m.pData), res);
        ctx->Unmap(r.staging[slot], 0);
        frameGenSynthMergeFrame(g.win, r.slotKind[slot], static_cast<uint64_t>(r.rw) * r.rh, res);
        if (r.slotScored[slot]) frameGenSynthMergeScore(g.win, res);
    }
}

void pollTimers(ID3D11DeviceContext* ctx) {
    if (!gpuTimingOwns(ctx)) return;
    for (TimerSlot& t : g.timers) {
        if (!t.inUse) continue;
        double ms = 0.0;
        const GpuTimerPoll p = t.timer.poll(ctx, ms);
        if (p == GpuTimerPoll::Pending) continue;
        t.inUse = false;
        if (p == GpuTimerPoll::Ready) g.gpuMs[t.kind].push_back(static_cast<float>(ms));
    }
}

// A timer pair is begun only inside a frequency scope that is already open (the census's, or the application-frame clock's): without one, GpuTimer::begin would open a scope of
// its own and hold a record of the clock's eight for several frames (gpu_census.cpp, frameWorkFirstCommand).
TimerSlot* beginTimer(ID3D11Device* dev, ID3D11DeviceContext* ctx, int kind) {
    if (!(gpuTimingCensusScopeOpen() || gpuFrameClockLive())) return nullptr;
    if (!gpuTimingAccepts(ctx) && !gpuTimingBind(dev, ctx)) return nullptr;
    for (TimerSlot& t : g.timers) {
        if (t.inUse) continue;
        if (t.timer.begin(dev, ctx)) { t.inUse = true; t.kind = kind; return &t; }
        return nullptr;
    }
    return nullptr;
}

void bindAndDispatch(ID3D11DeviceContext* ctx, int entry, ID3D11ShaderResourceView* const srv[kSrvSlots], ID3D11UnorderedAccessView* const uav[kUavSlots], uint32_t gx, uint32_t gy) {
    ctx->CSSetShader(g.cs[entry], nullptr, 0);
    // The last dispatch's UAVs come off first: a resource still bound as a UAV would silently null the SRV set for it here (the dilated depth, the key grid, the read positions...).
    ID3D11UnorderedAccessView* const noUav[kUavSlots] = {};
    ctx->CSSetUnorderedAccessViews(0, kUavSlots, noUav, nullptr);
    ctx->CSSetShaderResources(0, kSrvSlots, srv);
    ctx->CSSetUnorderedAccessViews(0, kUavSlots, uav, nullptr);
    ctx->Dispatch(gx, gy, 1);
}

}  // namespace

void frameGenSynthConfigure(bool on) {
    g_on.store(on);
    if (!on) g_told.store(false);
}

bool frameGenSynthOn() { return g_on.load(); }

bool frameGenSynthResident() { return anyResources(); }

FrameGenSynthWindow frameGenSynthOpenWindow() { return g.win; }

ID3D11Texture2D* frameGenSynthShown(uint32_t eye, uint32_t frame) {
    if (!g_on.load() || eye > 1) return nullptr;
    const EyeRes& r = g.eye[eye];
    return (r.created && r.shownValid && r.shownFrame == frame) ? r.s.tex : nullptr;
}

void frameGenSynthEyeFrame(ID3D11Device* dev, ID3D11DeviceContext* ctx, uint32_t eye, uint32_t frame, const FrameGenSynthFrame& f) {
    if (!g_on.load()) return;
    if (!dev || !ctx || eye > 1 || !f.tanNow) return;
    if (g.dev && g.dev != dev) releaseAll();   // a new device: everything starts over
    if (g.failed) return;
    if (!g.dev) {
        g.dev = dev;
        if (!createCommon(dev, ctx)) {
            g.failed = true;
            Log::get().note("frame generation (synth): the synthesizer's shaders, constant buffer or texture formats could not be created; it stands down.");
            return;
        }
    }
    EyeRes& r = g.eye[eye];
    r.shownValid = false;
    if (!f.outSrv || !f.depthSrv || !f.mvSrv || !f.depthWritten || f.renderW == 0 || f.renderH == 0 || f.outW == 0 || f.outH == 0) {
        // Nothing to build from: counted, so a window in which the module was called and built nothing says so.
        ++g.win.skipped;
        r.havePending = false;
        return;
    }
    const DXGI_FORMAT sFormat = viewFormat(f.outSrv);
    if (!r.created || r.rw != f.renderW || r.rh != f.renderH || r.ow != f.outW || r.oh != f.outH || r.sFormat != sFormat) {
        releaseEye(r);
        if (!createEye(dev, r, f.renderW, f.renderH, f.outW, f.outH, sFormat)) {
            g.failed = true;
            Log::get().note("frame generation (synth): the eye's textures could not be created (%ux%u render, %ux%u output, format %u); it stands down.",
                            static_cast<unsigned>(f.renderW), static_cast<unsigned>(f.renderH), static_cast<unsigned>(f.outW), static_cast<unsigned>(f.outH),
                            static_cast<unsigned>(sFormat));
            return;
        }
        if (!g_told.exchange(true)) {
            const std::string line = formatFrameGenSynthArmed(f.renderW, f.renderH, f.outW, f.outH, static_cast<uint32_t>(sFormat));
            Log::get().note("%s", line.c_str());
        }
    }
    pollReadbacks(ctx, r, r.writes);
    pollTimers(ctx);

    const bool half = f.wantShown || ((r.calls++ & 1u) == 0u);   // the even frames (and the dump's) build the f = 0.5 S
    const bool score = r.havePending && r.bFrame + 1u == frame && !f.cameraJump;
    const bool keep = !half && !f.cameraJump;

    FrameGenSynthCb cb;
    frameGenSynthFillCb(cb, f.renderW, f.renderH, f.outW, f.outH, f.tanNow, f.depthA, f.depthB, f.splitMetres, half ? 0.5f : 1.0f, f.deltas);
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(ctx->Map(g.cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        ++g.win.skipped;
        r.havePending = false;
        return;
    }
    std::memcpy(m.pData, &cb, sizeof(cb));
    ctx->Unmap(g.cb, 0);

    // A staging slot still unread when it comes round again is overwritten (its frame is lost to the window, not waited for).
    const int slot = static_cast<int>(r.writes % kStagingDepth);
    r.pending[slot] = false;

    TimerSlot* timer = beginTimer(dev, ctx, half ? 0 : 1);
    CsStageSave save;
    save.save(ctx);
    ctx->CSSetConstantBuffers(0, 1, &g.cb);
    const uint32_t rgx = (r.rw + 7u) / 8u, rgy = (r.rh + 7u) / 8u;
    const uint32_t ogx = (r.ow + 7u) / 8u, ogy = (r.oh + 7u) / 8u;
    const uint32_t qgx = (r.qw + 7u) / 8u, qgy = (r.qh + 7u) / 8u;
    ID3D11ShaderResourceView* const noSrv[kSrvSlots] = {};
    ID3D11UnorderedAccessView* const noUav[kUavSlots] = {};
    ID3D11ShaderResourceView* const out = f.outSrv;
    ID3D11ShaderResourceView* const depth = f.depthSrv;
    ID3D11ShaderResourceView* const mv = f.mvSrv;
    {   // clear: the key and source grids and the stats words
        ID3D11UnorderedAccessView* uav[kUavSlots] = {nullptr, r.key.uav, r.src.uav, nullptr, nullptr, r.statsUav};
        bindAndDispatch(ctx, kClear, noSrv, uav, rgx, rgy);
    }
    if (score) {   // the f = 1 S kept from the frame before against this frame, which is C: its luma, S's and B's, by B's class
        ID3D11ShaderResourceView* srv[kSrvSlots] = {out, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, r.sLuma.srv, r.bInfo.srv};
        ID3D11UnorderedAccessView* uav[kUavSlots] = {nullptr, nullptr, nullptr, nullptr, nullptr, r.statsUav};
        bindAndDispatch(ctx, kScore, srv, uav, qgx, qgy);
    }
    {   // dilate B's depth
        ID3D11ShaderResourceView* srv[kSrvSlots] = {nullptr, depth};
        ID3D11UnorderedAccessView* uav[kUavSlots] = {r.depthD.uav};
        bindAndDispatch(ctx, kDilate, srv, uav, rgx, rgy);
    }
    {   // splat: B's pixels carried to S, the nearer surface's key kept
        ID3D11ShaderResourceView* srv[kSrvSlots] = {nullptr, depth, mv, r.depthD.srv};
        ID3D11UnorderedAccessView* uav[kUavSlots] = {nullptr, r.key.uav};
        bindAndDispatch(ctx, kSplat, srv, uav, rgx, rgy);
    }
    {   // source: each footprint pixel whose final key is the pixel's own takes the pixel's index
        ID3D11ShaderResourceView* srv[kSrvSlots] = {nullptr, depth, mv, r.depthD.srv, r.key.srv};
        ID3D11UnorderedAccessView* uav[kUavSlots] = {nullptr, nullptr, r.src.uav};
        bindAndDispatch(ctx, kSrc, srv, uav, rgx, rgy);
    }
    {   // resolve: the position every S pixel reads, the guard and the background pull
        ID3D11ShaderResourceView* srv[kSrvSlots] = {nullptr, depth, mv, r.depthD.srv, r.key.srv, r.src.srv};
        ID3D11UnorderedAccessView* uav[kUavSlots] = {nullptr, nullptr, nullptr, r.read.uav, nullptr, r.statsUav};
        bindAndDispatch(ctx, kResolve, srv, uav, rgx, rgy);
    }
    {   // resample B's output into S at the output size
        ID3D11ShaderResourceView* srv[kSrvSlots] = {out, nullptr, nullptr, nullptr, nullptr, nullptr, r.read.srv};
        ID3D11UnorderedAccessView* uav[kUavSlots] = {nullptr, nullptr, nullptr, nullptr, r.s.uav};
        bindAndDispatch(ctx, kResample, srv, uav, ogx, ogy);
    }
    if (keep) {   // the f = 1 S's luma, B's luma and B's class, for the next real frame to score
        ID3D11ShaderResourceView* srv[kSrvSlots] = {out, depth, nullptr, nullptr, nullptr, nullptr, nullptr, r.s.srv};
        ID3D11UnorderedAccessView* uav[kUavSlots] = {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, r.sLuma.uav, r.bInfo.uav};
        bindAndDispatch(ctx, kLuma, srv, uav, qgx, qgy);
    }
    ctx->CSSetShaderResources(0, kSrvSlots, noSrv);
    ctx->CSSetUnorderedAccessViews(0, kUavSlots, noUav, nullptr);
    ctx->CopyResource(r.staging[slot], r.stats);
    r.pending[slot] = true;
    r.slotKind[slot] = half ? 0 : 1;
    r.slotScored[slot] = score;
    ++r.writes;
    if (timer) timer->timer.end(ctx);
    save.restore(ctx);

    r.havePending = keep;
    r.bFrame = frame;
    r.shownValid = half;
    r.shownFrame = frame;
}

void frameGenSynthFrameBoundary() {
    static int64_t s_ticks = 0;
    if (!g_on.load()) {
        // The key went off: nothing stays resident, and the next time on starts a fresh window.
        if (anyResources() || g.dev || g.failed) releaseAll();
        if (s_ticks != 0 || g.win.frames[0] || g.win.frames[1] || g.win.skipped) { windowReset(); s_ticks = 0; }
        return;
    }
    const int64_t now = qpcNow();
    const int64_t freq = qpcFrequency();
    if (s_ticks == 0) {
        s_ticks = now;
        return;
    }
    if (freq <= 0 || now - s_ticks < freq) return;
    s_ticks = now;
    if (g.win.frames[0] == 0 && g.win.frames[1] == 0 && g.win.skipped == 0) {   // the module was not called in the window (a menu, a loading screen): nothing to say
        windowReset();
        return;
    }
    for (int k = 0; k < 2; ++k) {
        g.win.gpuSamples[k] = static_cast<uint32_t>(g.gpuMs[k].size());
        g.win.gpuMax[k] = 0.0f;
        for (float v : g.gpuMs[k]) if (v > g.win.gpuMax[k]) g.win.gpuMax[k] = v;
        g.win.gpuMedian[k] = frameGenMedian(g.gpuMs[k].data(), g.gpuMs[k].size());
    }
    const std::string line = formatFrameGenSynthReport(g.win);
    Log::get().note("%s", line.c_str());
    windowReset();
}

}  // namespace edvr
