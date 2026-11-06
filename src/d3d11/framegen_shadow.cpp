// The frame-generation shadow scorer: see framegen_shadow.h.
#include "temporal_shader_bytecode.h"
#include "framegen_shadow.h"
#include "cs_stage_save.h"
#include "gpu_frame_timing.h"
#include "gpu_timing.h"
#include "shader_swap.h"
#include "../common/framegen_shadow_report.h"
#include "../common/log.h"

#include <windows.h>
#include <d3d11.h>

#include <atomic>
#include <cstring>
#include <vector>

namespace edvr {
namespace {

constexpr int kStagingDepth = 4;
constexpr int kTimerRing = 8;
constexpr uint32_t kMinCells = kFrameGenShadowStatsUints;   // the clear entry zeroes the stats words from the first cells of the grid

std::atomic<bool> g_on{false};
std::atomic<bool> g_told{false};   // the armed line has been said since the key last went on

struct Tex {
    ID3D11Texture2D* tex = nullptr;
    ID3D11ShaderResourceView* srv = nullptr;
    ID3D11UnorderedAccessView* uav = nullptr;
};

// One eye's resources and its last frame's capture and deltas.
struct EyeRes {
    bool created = false;
    uint32_t rw = 0, rh = 0, ow = 0, oh = 0, qw = 0, qh = 0;
    Tex luma[2], depth[2], key;
    ID3D11Buffer* stats = nullptr;
    ID3D11UnorderedAccessView* statsUav = nullptr;
    ID3D11Buffer* staging[kStagingDepth] = {};
    bool pending[kStagingDepth] = {};
    uint32_t writes = 0;
    int cur = 0;                  // the slot of the latest capture
    bool haveCapture = false;
    uint32_t captureFrame = 0;
    FrameGenDeltas prev;          // the latest frame's deltas
    bool haveDeltas = false, prevWorldValid = false, prevJump = false;
};

struct TimerSlot {
    GpuTimer timer;
    bool inUse = false;
};

struct State {
    ID3D11Device* dev = nullptr;   // identity only
    bool failed = false;           // creation failed on this device: stand down until it changes or the key is toggled
    ID3D11ComputeShader* cs[4] = {};   // capture, clear, splat, score
    ID3D11Buffer* cb = nullptr;
    DXGI_FORMAT lumaFormat = DXGI_FORMAT_R16_FLOAT;
    EyeRes eye[2];
    TimerSlot timers[kTimerRing];
    // The one-second window.
    FrameGenShadowWindow win;
    std::vector<float> gpuMs, rigDeg, rigMm;
    uint32_t eyeFrames = 0;
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
    for (Tex& t : r.luma) releaseTex(t);
    for (Tex& t : r.depth) releaseTex(t);
    releaseTex(r.key);
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
    g.win = FrameGenShadowWindow{};
    g.gpuMs.clear();
    g.rigDeg.clear();
    g.rigMm.clear();
    g.eyeFrames = 0;
}

// The shaders, the constant buffer and the formats: made once per device. The timers are left as they are (a timer with a query in flight is released only by
// its owner's context, gpu_timing.h); the ring simply keeps polling them.
void releaseAll() {
    for (EyeRes& r : g.eye) releaseEye(r);
    for (auto& c : g.cs) releaseCom(c);
    releaseCom(g.cb);
    g.dev = nullptr;
    g.failed = false;
}

bool createCommon(ID3D11Device* dev, ID3D11DeviceContext* ctx) {
    struct Entry { const unsigned char* code; size_t size; const char* name; };
    const Entry entries[4] = {
        {kFrameGenShadowCaptureBytecode, sizeof(kFrameGenShadowCaptureBytecode), "framegen_shadow_capture"},
        {kFrameGenShadowClearBytecode, sizeof(kFrameGenShadowClearBytecode), "framegen_shadow_clear"},
        {kFrameGenShadowSplatBytecode, sizeof(kFrameGenShadowSplatBytecode), "framegen_shadow_splat"},
        {kFrameGenShadowScoreBytecode, sizeof(kFrameGenShadowScoreBytecode), "framegen_shadow_score"},
    };
    for (int i = 0; i < 4; ++i) {
        g.cs[i] = shaderSwapCreateCs(ctx, entries[i].code, entries[i].size, entries[i].name, "frame generation (shadow)");
        if (!g.cs[i]) return false;
    }
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = sizeof(FrameGenShadowCb);
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(dev->CreateBuffer(&bd, nullptr, &g.cb))) return false;
    // R16_FLOAT where the adapter does typed UAV stores on it (every D3D11 part that matters does), else R32_FLOAT.
    UINT support = 0;
    const UINT want = D3D11_FORMAT_SUPPORT_TEXTURE2D | D3D11_FORMAT_SUPPORT_SHADER_LOAD | D3D11_FORMAT_SUPPORT_TYPED_UNORDERED_ACCESS_VIEW;
    g.lumaFormat = (SUCCEEDED(dev->CheckFormatSupport(DXGI_FORMAT_R16_FLOAT, &support)) && (support & want) == want) ? DXGI_FORMAT_R16_FLOAT : DXGI_FORMAT_R32_FLOAT;
    return true;
}

bool makeTex(ID3D11Device* dev, uint32_t w, uint32_t h, DXGI_FORMAT fmt, bool srv, Tex& out) {
    D3D11_TEXTURE2D_DESC td{};
    td.Width = w;
    td.Height = h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = fmt;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_UNORDERED_ACCESS | (srv ? D3D11_BIND_SHADER_RESOURCE : 0u);
    if (FAILED(dev->CreateTexture2D(&td, nullptr, &out.tex))) return false;
    if (srv && FAILED(dev->CreateShaderResourceView(out.tex, nullptr, &out.srv))) return false;
    return SUCCEEDED(dev->CreateUnorderedAccessView(out.tex, nullptr, &out.uav));
}

bool createEye(ID3D11Device* dev, EyeRes& r, uint32_t rw, uint32_t rh, uint32_t ow, uint32_t oh) {
    r = EyeRes{};
    r.rw = rw; r.rh = rh; r.ow = ow; r.oh = oh;
    r.qw = (rw + 3u) / 4u;
    r.qh = (rh + 3u) / 4u;
    if (r.qw * r.qh < kMinCells) return false;
    bool ok = true;
    for (int i = 0; i < 2 && ok; ++i) {
        ok = makeTex(dev, r.qw, r.qh, g.lumaFormat, true, r.luma[i]) && makeTex(dev, r.qw, r.qh, DXGI_FORMAT_R32_FLOAT, true, r.depth[i]);
    }
    ok = ok && makeTex(dev, r.qw, r.qh, DXGI_FORMAT_R32_UINT, false, r.key);
    if (ok) {
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = kFrameGenShadowStatsUints * 4u;
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        bd.StructureByteStride = 4;
        ok = SUCCEEDED(dev->CreateBuffer(&bd, nullptr, &r.stats));
        if (ok) {
            D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
            ud.Format = DXGI_FORMAT_UNKNOWN;
            ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
            ud.Buffer.NumElements = kFrameGenShadowStatsUints;
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
        FrameGenShadowEyeResult res;
        frameGenDecodeStats(static_cast<const uint32_t*>(m.pData), res);
        ctx->Unmap(r.staging[slot], 0);
        frameGenShadowMerge(g.win, res);
        ++g.win.framesScored;
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
        if (p == GpuTimerPoll::Ready) g.gpuMs.push_back(static_cast<float>(ms));
    }
}

// A timer pair is begun only inside a frequency scope that is already open (the census's, or the application-frame clock's): without one, GpuTimer::begin would
// open a scope of its own and hold a record of the clock's eight for several frames (gpu_census.cpp, frameWorkFirstCommand).
TimerSlot* beginTimer(ID3D11Device* dev, ID3D11DeviceContext* ctx) {
    if (!(gpuTimingCensusScopeOpen() || gpuFrameClockLive())) return nullptr;
    if (!gpuTimingAccepts(ctx) && !gpuTimingBind(dev, ctx)) return nullptr;
    for (TimerSlot& t : g.timers) {
        if (t.inUse) continue;
        if (t.timer.begin(dev, ctx)) { t.inUse = true; return &t; }
        return nullptr;
    }
    return nullptr;
}

void bindAndDispatch(ID3D11DeviceContext* ctx, int entry, ID3D11ShaderResourceView* const srv[5], ID3D11UnorderedAccessView* const uav[4], uint32_t gx, uint32_t gy) {
    ctx->CSSetShader(g.cs[entry], nullptr, 0);
    ctx->CSSetShaderResources(0, 5, srv);
    ctx->CSSetUnorderedAccessViews(0, 4, uav, nullptr);
    ctx->Dispatch(gx, gy, 1);
}

}  // namespace

void frameGenShadowConfigure(bool on) {
    g_on.store(on);
    if (!on) g_told.store(false);
}

bool frameGenShadowOn() { return g_on.load(); }

bool frameGenShadowResident() { return anyResources(); }

FrameGenShadowWindow frameGenShadowOpenWindow() { return g.win; }

void frameGenShadowEyeFrame(ID3D11Device* dev, ID3D11DeviceContext* ctx, uint32_t eye, uint32_t frame, const FrameGenShadowFrame& f) {
    if (!g_on.load()) return;
    if (!g_told.exchange(true)) {
        const std::string line = formatFrameGenShadowArmed((f.renderW + 3u) / 4u, (f.renderH + 3u) / 4u);
        Log::get().note("%s", line.c_str());
    }
    if (!dev || !ctx || eye > 1 || !f.tanNow) return;
    if (g.dev && g.dev != dev) releaseAll();   // a new device: everything starts over
    if (g.failed) return;
    if (!g.dev) {
        g.dev = dev;
        if (!createCommon(dev, ctx)) {
            g.failed = true;
            Log::get().note("frame generation (shadow): the scorer's shaders or constant buffer could not be created; it stands down.");
            return;
        }
    }
    EyeRes& r = g.eye[eye];
    if (!r.created || r.rw != f.renderW || r.rh != f.renderH || r.ow != f.outW || r.oh != f.outH) {
        releaseEye(r);
        if (!createEye(dev, r, f.renderW, f.renderH, f.outW, f.outH)) {
            g.failed = true;
            Log::get().note("frame generation (shadow): the eye's quarter-size resources could not be created (%ux%u render); it stands down.",
                            static_cast<unsigned>(f.renderW), static_cast<unsigned>(f.renderH));
            return;
        }
    }
    ++g.eyeFrames;
    pollReadbacks(ctx, r, r.writes);
    pollTimers(ctx);

    // Why this frame is not scored, in the plan's order; -1 scores it.
    int skip = -1;
    if (!r.haveCapture || r.captureFrame + 1u != frame || !f.prevDepthValid || !f.depthWritten) skip = kSkipNoPrevDepth;
    else if (!r.haveDeltas || !f.tanPrev) skip = kSkipNoPrevDelta;
    else if (!f.worldValid || !r.prevWorldValid) skip = kSkipRowsInvalid;
    else if (f.cameraJump || r.prevJump) skip = kSkipCameraJump;
    const bool canCapture = f.depthWritten && f.outSrv && f.depthSrv;
    if (skip < 0 && !canCapture) skip = kSkipNoPrevDepth;
    const int next = r.haveCapture ? 1 - r.cur : 0;
    bool scored = skip < 0 && canCapture;

    // A staging slot still unread when it comes round again is overwritten: counted, not waited for.
    const int slot = static_cast<int>(r.writes % kStagingDepth);
    if (scored && r.pending[slot]) {
        ++g.win.skipped[kSkipReadbackLate];
        r.pending[slot] = false;
    }

    if (canCapture) {
        FrameGenShadowCb cb;
        float Wp[9], tp[3];
        FrameGenAffine bwd[kFrameGenShadowClasses];
        if (scored) {
            frameGenPredictWorld(r.prev, f.deltas, Wp, tp);
            frameGenBackward(f.deltas, Wp, tp, bwd);
            float deg = 0.0f, mm = 0.0f;
            frameGenRigMiss(Wp, tp, f.deltas, &deg, &mm);
            g.rigDeg.push_back(deg);
            g.rigMm.push_back(mm);
        }
        frameGenFillCb(cb, f.renderW, f.renderH, f.outW, f.outH, scored ? f.tanPrev : f.tanNow, f.tanNow, f.depthA, f.depthB, f.splitMetres, bwd);
        D3D11_MAPPED_SUBRESOURCE m{};
        if (SUCCEEDED(ctx->Map(g.cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
            std::memcpy(m.pData, &cb, sizeof(cb));
            ctx->Unmap(g.cb, 0);
        } else {
            scored = false;
            skip = skip < 0 ? kSkipNoPrevDelta : skip;
        }
        TimerSlot* timer = beginTimer(dev, ctx);
        CsStageSave save;
        save.save(ctx);
        ctx->CSSetConstantBuffers(0, 1, &g.cb);
        const uint32_t gx = (r.qw + 7u) / 8u, gy = (r.qh + 7u) / 8u;
        ID3D11ShaderResourceView* noSrv[5] = {};
        ID3D11UnorderedAccessView* noUav[4] = {};
        {   // capture this frame into the slot the next frame will read as its previous
            ID3D11ShaderResourceView* srv[5] = {f.outSrv, f.depthSrv, nullptr, nullptr, nullptr};
            ID3D11UnorderedAccessView* uav[4] = {r.luma[next].uav, r.depth[next].uav, nullptr, nullptr};
            bindAndDispatch(ctx, 0, srv, uav, gx, gy);
        }
        if (scored) {
            const int prevSlot = r.cur;
            {   // clear
                ID3D11UnorderedAccessView* uav[4] = {nullptr, nullptr, r.key.uav, r.statsUav};
                bindAndDispatch(ctx, 1, noSrv, uav, gx, gy);
            }
            {   // splat: the previous depth, forward
                ID3D11ShaderResourceView* srv[5] = {nullptr, nullptr, nullptr, r.depth[prevSlot].srv, nullptr};
                ID3D11UnorderedAccessView* uav[4] = {nullptr, nullptr, r.key.uav, nullptr};
                bindAndDispatch(ctx, 2, srv, uav, gx, gy);
            }
            {   // score: the previous and this frame's luma against the keys, backward
                ID3D11ShaderResourceView* srv[5] = {nullptr, nullptr, r.luma[prevSlot].srv, nullptr, r.luma[next].srv};
                ID3D11UnorderedAccessView* uav[4] = {nullptr, nullptr, r.key.uav, r.statsUav};
                bindAndDispatch(ctx, 3, srv, uav, gx, gy);
            }
            ctx->CSSetShaderResources(0, 5, noSrv);
            ctx->CSSetUnorderedAccessViews(0, 4, noUav, nullptr);
            ctx->CopyResource(r.staging[slot], r.stats);
            r.pending[slot] = true;
            ++r.writes;
        } else {
            ctx->CSSetShaderResources(0, 5, noSrv);
            ctx->CSSetUnorderedAccessViews(0, 4, noUav, nullptr);
        }
        if (timer) timer->timer.end(ctx);
        save.restore(ctx);
        r.cur = next;
        r.haveCapture = true;
        r.captureFrame = frame;
    } else {
        r.haveCapture = false;
    }
    if (!scored && skip >= 0) ++g.win.skipped[skip];
    r.prev = f.deltas;
    r.haveDeltas = true;
    r.prevWorldValid = f.worldValid;
    r.prevJump = f.cameraJump;
}

void frameGenShadowFrameBoundary() {
    if (!g_on.load()) {
        // The key went off: nothing stays resident, and the next time on starts a fresh window.
        if (anyResources() || g.dev || g.failed) releaseAll();
        if (g.windowTicks != 0 || g.eyeFrames != 0) { windowReset(); g.windowTicks = 0; }
        return;
    }
    const int64_t now = qpcNow();
    const int64_t freq = qpcFrequency();
    if (g.windowTicks == 0) {
        g.windowTicks = now;
        return;
    }
    if (freq <= 0 || now - g.windowTicks < freq) return;
    g.windowTicks = now;
    if (g.eyeFrames == 0) {   // the pass treated no frame in the window (a menu, a loading screen): nothing to say
        windowReset();
        return;
    }
    g.win.gpuSamples = static_cast<uint32_t>(g.gpuMs.size());
    g.win.gpuMsMax = 0.0f;
    for (float v : g.gpuMs) if (v > g.win.gpuMsMax) g.win.gpuMsMax = v;
    g.win.gpuMsMedian = frameGenMedian(g.gpuMs.data(), g.gpuMs.size());
    g.win.rigDegMax = 0.0f;
    for (float v : g.rigDeg) if (v > g.win.rigDegMax) g.win.rigDegMax = v;
    g.win.rigDegMedian = frameGenMedian(g.rigDeg.data(), g.rigDeg.size());
    g.win.rigMmMax = 0.0f;
    for (float v : g.rigMm) if (v > g.win.rigMmMax) g.win.rigMmMax = v;
    g.win.rigMmMedian = frameGenMedian(g.rigMm.data(), g.rigMm.size());
    const std::string line = formatFrameGenShadowReport(g.win);
    Log::get().note("%s", line.c_str());
    windowReset();
}

}  // namespace edvr
