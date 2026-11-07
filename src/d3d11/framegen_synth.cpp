// The frame-generation synthesizer: see framegen_synth.h.
#include "temporal_shader_bytecode.h"
#include "framegen_synth.h"
#include "framegen_synth_pass.h"
#include "cs_stage_save.h"
#include "gpu_frame_timing.h"
#include "gpu_timing.h"
#include "shader_swap.h"
#include "../common/framegen_replay_math.h"
#include "../common/framegen_synth_report.h"
#include "../common/log.h"

#include <windows.h>
#include <d3d11.h>

#include <atomic>
#include <cstdio>
#include <string>
#include <thread>
#include <cmath>
#include <cstring>
#include <vector>

namespace edvr {
namespace {

constexpr int kStagingDepth = 4;
constexpr int kTimerRing = 8;
constexpr int kEntries = kFrameGenSynthEntries;   // splat, source, resolve, holes, resample, luma, score (framegen_synth_pass.h)

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
    float keptRot[9] = {};        // the head rotation the kept f = 1 S was built for (B's eye space to S's)
    float keptTan[4] = {};        // the tangents it was built with
    bool shownValid = false;      // the latest call built the f = 0.5 S into `s`
    uint32_t shownFrame = 0;
};

// The render-size scratch of one call (the key grid, the read offsets and the hole list): rebuilt from nothing by every call, so both eyes share one set (they run one after the
// other on the same context). 8.5 bytes per render pixel in all; recreated when a call's render size differs from the last's.
struct Scratch {
    uint32_t rw = 0, rh = 0;
    Tex key, read;                              // R32_UINT both: the keys; the read offsets as two packed half floats
    ID3D11Buffer* holes = nullptr;              // frameGenSynthHoleCapacity uints
    ID3D11UnorderedAccessView* holesUav = nullptr;
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
    Scratch scratch;
    TimerSlot timers[kTimerRing];
    FrameGenSynthWindow win;
    std::vector<float> gpuMs[2];
    std::vector<float> rotRes;     // the residual rotation's angle of each scored frame in the window, degrees
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
    releaseTex(r.s);
    releaseTex(r.sLuma);
    releaseTex(r.bInfo);
    releaseCom(r.statsUav);
    releaseCom(r.stats);
    for (auto& s : r.staging) releaseCom(s);
    r = EyeRes{};
}

void releaseScratch() {
    releaseTex(g.scratch.key);
    releaseTex(g.scratch.read);
    releaseCom(g.scratch.holesUav);
    releaseCom(g.scratch.holes);
    g.scratch = Scratch{};
}

bool anyResources() {
    if (g.cb || g.scratch.key.tex) return true;
    for (const EyeRes& r : g.eye) if (r.created) return true;
    for (ID3D11ComputeShader* c : g.cs) if (c) return true;
    return false;
}

void windowReset() {
    g.win = FrameGenSynthWindow{};
    g.gpuMs[0].clear();
    g.gpuMs[1].clear();
    g.rotRes.clear();
}

// The residual rotation's median and maximum over the scored frames of the window.
void fillRotation(FrameGenSynthWindow& w) {
    w.rotSamples = static_cast<uint32_t>(g.rotRes.size());
    w.rotMax = 0.0f;
    for (float v : g.rotRes) if (v > w.rotMax) w.rotMax = v;
    w.rotMedian = frameGenMedian(g.rotRes.data(), g.rotRes.size());
}

// ---- Replay capture (.agents/framegen-replay-format.md) ----
// The eye dump's left-eye run asks for the synthesizer's inputs of its first six frames. Each is copied (CopyResource, GPU side, no wait) into three staging textures; nothing is
// mapped on the frame it was copied. When the dump writes its files frameGenReplayFinish hands the set to the pump, which maps one copy per turn with DO_NOT_WAIT (a copy still in
// flight is tried again next frame) and lets a worker thread hash and write it straight from the mapping; the render thread only maps, unmaps and joins.
#ifndef EDVR_VERSION_STRING
#define EDVR_VERSION_STRING "unversioned test build"
#endif

constexpr uint32_t kReplayFrames = kFrameGenReplayFrames;

struct ReplaySet {
    ID3D11Texture2D* tex[kReplayFrames][3] = {};    // out, depth, mv
    uint32_t texFormat[kReplayFrames][3] = {};
    FrameGenReplayFrame info[kReplayFrames];
    uint32_t count = 0;
    bool stopped = false;
    std::string stop;                               // why the run of consecutive frames ended early
    bool open = false;                              // frame 0 was seen and the set is being filled
    bool writing = false;                           // frameGenReplayFinish handed it over
    std::wstring dir, stamp;
    std::vector<FrameGenReplayFile> files;
    uint32_t job = 0;                               // next copy to write: frame job / 3, texture job % 3
    bool mapped = false;
    D3D11_MAPPED_SUBRESOURCE map{};
    ID3D11DeviceContext* mapCtx = nullptr;          // holds a reference while mapped
    std::thread worker;
    std::atomic<bool> workerDone{false};
    bool workerRunning = false;
    FrameGenReplayFile result;
    bool resultOk = false;
};
// Never destroyed, like the state: a joinable thread member must not be destroyed at process teardown.
ReplaySet* const g_replayPtr = new ReplaySet();
ReplaySet& rp = *g_replayPtr;

const char* const kReplayPart[3] = {"out", "depth", "mv"};

void replayRelease() {
    if (rp.workerRunning) { rp.worker.join(); rp.workerRunning = false; }
    if (rp.mapped && rp.mapCtx && rp.job / 3 < kReplayFrames) {
        ID3D11Texture2D* t = rp.tex[rp.job / 3][rp.job % 3];
        if (t) rp.mapCtx->Unmap(t, 0);
    }
    rp.mapped = false;
    releaseCom(rp.mapCtx);
    for (auto& row : rp.tex) for (auto& t : row) releaseCom(t);
    rp.count = 0; rp.stopped = false; rp.stop.clear(); rp.open = false; rp.writing = false;
    rp.files.clear(); rp.job = 0; rp.dir.clear(); rp.stamp.clear();
}

void replayStop(const std::string& why) {
    if (rp.writing || !rp.open || rp.stopped) return;
    rp.stopped = true;
    rp.stop = why;
}

std::string narrow(const std::wstring& w) {
    std::string s;
    for (wchar_t c : w) s += c < 0x80 ? static_cast<char>(c) : '?';
    return s;
}

bool replayStageCopy(ID3D11Device* dev, ID3D11DeviceContext* ctx, ID3D11ShaderResourceView* srv, ID3D11Texture2D*& out, uint32_t& format) {
    out = nullptr;
    if (!srv) return false;
    ID3D11Resource* res = nullptr;
    srv->GetResource(&res);
    if (!res) return false;
    ID3D11Texture2D* src = nullptr;
    const bool isTex = SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&src))) && src;
    res->Release();
    if (!isTex) return false;
    D3D11_TEXTURE2D_DESC d{};
    src->GetDesc(&d);
    bool ok = false;
    if (d.SampleDesc.Count == 1 && frameGenReplayFormatBytes(d.Format) != 0) {
        D3D11_TEXTURE2D_DESC sd = d;
        sd.Usage = D3D11_USAGE_STAGING; sd.BindFlags = 0; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ; sd.MiscFlags = 0; sd.MipLevels = 1; sd.ArraySize = 1;
        if (SUCCEEDED(dev->CreateTexture2D(&sd, nullptr, &out)) && out) {
            ctx->CopyResource(out, src);
            format = static_cast<uint32_t>(d.Format);
            ok = true;
        }
    }
    src->Release();
    return ok;
}

// Called from frameGenSynthEyeFrame on a frame that built its S. cb is the constant block built for it.
void replayCapture(ID3D11Device* dev, ID3D11DeviceContext* ctx, const FrameGenSynthFrame& f, uint32_t frame, const FrameGenSynthCb& cb) {
    if (!rp.open || rp.stopped || rp.writing || rp.count >= kReplayFrames) return;
    if (f.replayIndex != rp.count) { replayStop("frame " + std::to_string(rp.count) + " of the run was not captured (the synthesizer skipped it or the run broke)"); return; }
    ID3D11ShaderResourceView* const srv[3] = {f.outSrv, f.depthSrv, f.mvSrv};
    const uint32_t k = rp.count;
    for (int i = 0; i < 3; ++i) {
        if (!replayStageCopy(dev, ctx, srv[i], rp.tex[k][i], rp.texFormat[k][i])) {
            for (int j = 0; j < 3; ++j) releaseCom(rp.tex[k][j]);
            replayStop(std::string("frame ") + std::to_string(k) + ": the " + kReplayPart[i] + " texture could not be staged (multisampled, or a format the capture does not know)");
            return;
        }
    }
    FrameGenReplayFrame& r = rp.info[k];
    r = FrameGenReplayFrame{};
    r.index = k;
    r.renderW = f.renderW; r.renderH = f.renderH; r.outW = f.outW; r.outH = f.outH;
    r.outFormat = rp.texFormat[k][0]; r.depthFormat = rp.texFormat[k][1]; r.mvFormat = rp.texFormat[k][2];
    for (int i = 0; i < 4; ++i) r.tanNow[i] = f.tanNow[i];
    if (f.tanPrev) { r.haveTanPrev = true; for (int i = 0; i < 4; ++i) r.tanPrev[i] = f.tanPrev[i]; }
    r.depthA = f.depthA; r.depthB = f.depthB; r.splitMetres = f.splitMetres; r.f = cb.dk[3];
    std::memcpy(r.Hd, f.deltas.Hd, sizeof(r.Hd)); std::memcpy(r.th, f.deltas.th, sizeof(r.th));
    std::memcpy(r.W, f.deltas.W, sizeof(r.W)); std::memcpy(r.tvCam, f.deltas.tv, sizeof(r.tvCam));
    r.worldValid = f.worldValid; r.cameraJump = f.cameraJump;
    r.passFrame = frame;
    r.qpc = qpcNow(); r.qpcFrequency = qpcFrequency();
    ++rp.count;
}

bool replayWriteText(const std::wstring& path, const std::string& text, FrameGenReplayFile& out) {
    FILE* fp = nullptr;
    if (_wfopen_s(&fp, path.c_str(), L"wb") || !fp) return false;
    Sha256 h;
    h.update(text.data(), text.size());
    bool ok = std::fwrite(text.data(), 1, text.size(), fp) == text.size();
    if (std::fclose(fp) != 0) ok = false;
    out.bytes = text.size();
    out.sha256 = h.hex();
    return ok;
}

void replayWorker(const uint8_t* data, uint32_t rowPitch, uint32_t rows, uint32_t rowBytes, std::wstring path) {
    rp.resultOk = false;
    FILE* fp = nullptr;
    if (!_wfopen_s(&fp, path.c_str(), L"wb") && fp) {
        Sha256 h;
        bool ok = true;
        for (uint32_t y = 0; y < rows && ok; ++y) {
            const uint8_t* row = data + static_cast<size_t>(y) * rowPitch;
            h.update(row, rowBytes);
            ok = std::fwrite(row, 1, rowBytes, fp) == rowBytes;
        }
        if (std::fclose(fp) != 0) ok = false;
        rp.result.bytes = static_cast<uint64_t>(rowBytes) * rows;
        rp.result.sha256 = h.hex();
        rp.resultOk = ok;
    }
    rp.workerDone.store(true, std::memory_order_release);
}

// The per-frame json and the manifest: small, written on the render thread once every bin is out.
void replayFinishSet() {
    for (uint32_t k = 0; k < rp.count; ++k) {
        FrameGenReplayFile fl;
        char name[32];
        std::snprintf(name, sizeof(name), "f%u.json", k);
        fl.name = name;
        if (replayWriteText(rp.dir + L"\\" + std::wstring(name, name + std::strlen(name)), frameGenReplayFrameJson(rp.info[k]), fl)) rp.files.push_back(fl);
        else if (rp.stop.empty()) rp.stop = std::string("could not write ") + name;
    }
    std::string why = rp.stop;
    if (why.empty() && rp.count < kReplayFrames) why = "the run ended after " + std::to_string(rp.count) + " frames";
    FrameGenReplayFile mf;
    if (replayWriteText(rp.dir + L"\\manifest.json", frameGenReplayManifestJson(narrow(rp.stamp), EDVR_VERSION_STRING, rp.count, why, rp.files), mf))
        Log::get().note("frame generation (synth): replay capture %ls: %u frames, %zu files written%s%s.", rp.stamp.c_str(), rp.count, rp.files.size() + 1, why.empty() ? "" : "; stopped: ", why.c_str());
    else
        Log::get().note("frame generation (synth): replay capture %ls: could not write manifest.json.", rp.stamp.c_str());
    replayRelease();
}

// The shaders, the constant buffer and the formats: made once per device. The timers are left as they are (a timer with a query in flight is released only by its owner's
// context, gpu_timing.h); the ring simply keeps polling them.
void releaseAll() {
    replayRelease();
    for (EyeRes& r : g.eye) releaseEye(r);
    releaseScratch();
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
    struct Item { int entry; const unsigned char* code; size_t size; const char* name; };
    const Item entries[kEntries] = {   // in framegen_synth_pass.h's entry order
        {kFgSynSplat, kFrameGenSynthSplatBytecode, sizeof(kFrameGenSynthSplatBytecode), "framegen_synth_splat"},
        {kFgSynSrc, kFrameGenSynthSrcBytecode, sizeof(kFrameGenSynthSrcBytecode), "framegen_synth_src"},
        {kFgSynResolve, kFrameGenSynthResolveBytecode, sizeof(kFrameGenSynthResolveBytecode), "framegen_synth_resolve"},
        {kFgSynHoles, kFrameGenSynthHolesBytecode, sizeof(kFrameGenSynthHolesBytecode), "framegen_synth_holes"},
        {kFgSynResample, kFrameGenSynthResampleBytecode, sizeof(kFrameGenSynthResampleBytecode), "framegen_synth_resample"},
        {kFgSynLuma, kFrameGenSynthLumaBytecode, sizeof(kFrameGenSynthLumaBytecode), "framegen_synth_luma"},
        {kFgSynScore, kFrameGenSynthScoreBytecode, sizeof(kFrameGenSynthScoreBytecode), "framegen_synth_score"},
    };
    for (const Item& e : entries) {
        g.cs[e.entry] = shaderSwapCreateCs(ctx, e.code, e.size, e.name, "frame generation (synth)");
        if (!g.cs[e.entry]) return false;
    }
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = sizeof(FrameGenSynthCb);
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(dev->CreateBuffer(&bd, nullptr, &g.cb))) return false;
    // The formats the textures are written in: R32_FLOAT and R32_UINT (the keys, the packed read offsets) are in every D3D11 part's typed set, stores and loads; the two-float
    // class map needs the adapter to store it (every D3D11 part that matters does); the luma is R16_FLOAT where the adapter stores it, else R32_FLOAT, as the scorer's.
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
    bool ok = makeTex(dev, ow, oh, sFormat, r.s) && makeTex(dev, r.qw, r.qh, g.lumaFormat, r.sLuma) && makeTex(dev, r.qw, r.qh, DXGI_FORMAT_R32G32_FLOAT, r.bInfo);
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

bool ensureScratch(ID3D11Device* dev, uint32_t rw, uint32_t rh) {
    Scratch& sc = g.scratch;
    if (sc.key.tex && sc.rw == rw && sc.rh == rh) return true;
    releaseScratch();
    if (makeTex(dev, rw, rh, DXGI_FORMAT_R32_UINT, g.scratch.key) && makeTex(dev, rw, rh, DXGI_FORMAT_R32_UINT, g.scratch.read)) {
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = frameGenSynthHoleCapacity(rw, rh) * 4u;
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        bd.StructureByteStride = 4;
        D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
        ud.Format = DXGI_FORMAT_UNKNOWN;
        ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        ud.Buffer.NumElements = frameGenSynthHoleCapacity(rw, rh);
        if (bd.ByteWidth && SUCCEEDED(dev->CreateBuffer(&bd, nullptr, &g.scratch.holes)) && SUCCEEDED(dev->CreateUnorderedAccessView(g.scratch.holes, &ud, &g.scratch.holesUav))) {
            g.scratch.rw = rw;
            g.scratch.rh = rh;
            return true;
        }
    }
    releaseScratch();
    return false;
}

// The bytes of what was made, from the descriptors (a texture: width x height x bytes per texel; a buffer: its byte width), for the VRAM line.
uint64_t texBytes(const Tex& t) {
    if (!t.tex) return 0;
    D3D11_TEXTURE2D_DESC d{};
    t.tex->GetDesc(&d);
    return static_cast<uint64_t>(d.Width) * d.Height * frameGenReplayFormatBytes(static_cast<uint32_t>(d.Format));
}

uint64_t bufBytes(ID3D11Buffer* b) {
    if (!b) return 0;
    D3D11_BUFFER_DESC d{};
    b->GetDesc(&d);
    return d.ByteWidth;
}

FrameGenSynthVram vramOf(const EyeRes& r, const Scratch& sc) {
    FrameGenSynthVram v;
    v.s = texBytes(r.s);
    v.luma = texBytes(r.sLuma);
    v.cls = texBytes(r.bInfo);
    v.stats = bufBytes(r.stats);
    for (ID3D11Buffer* s : r.staging) v.stats += bufBytes(s);
    v.key = texBytes(sc.key);
    v.read = texBytes(sc.read);
    v.holes = bufBytes(sc.holes);
    return v;
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

}  // namespace

void frameGenSynthConfigure(bool on) {
    g_on.store(on);
    if (!on) g_told.store(false);
}

bool frameGenSynthOn() { return g_on.load(); }

bool frameGenSynthResident() { return anyResources(); }

FrameGenSynthWindow frameGenSynthOpenWindow() {
    FrameGenSynthWindow w = g.win;
    fillRotation(w);
    return w;
}

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
    if (f.replay && f.replayIndex == 0 && !rp.writing) { replayRelease(); rp.open = true; }   // the eye dump's first frame: a new capture (one still being written is not disturbed)
    if (!f.outSrv || !f.depthSrv || !f.mvSrv || !f.depthWritten || f.renderW == 0 || f.renderH == 0 || f.outW == 0 || f.outH == 0) {
        // Nothing to build from: counted, so a window in which the module was called and built nothing says so.
        ++g.win.skipped;
        r.havePending = false;
        if (f.replay) replayStop("frame " + std::to_string(f.replayIndex) + " of the run: the synthesizer had no inputs for it");
        return;
    }
    const DXGI_FORMAT sFormat = viewFormat(f.outSrv);
    bool made = false;   // the eye's textures were made on this call: the VRAM line follows once the scratch exists too
    if (!r.created || r.rw != f.renderW || r.rh != f.renderH || r.ow != f.outW || r.oh != f.outH || r.sFormat != sFormat) {
        made = true;
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
    if (!ensureScratch(dev, f.renderW, f.renderH)) {
        g.failed = true;
        Log::get().note("frame generation (synth): the scratch textures could not be created (%ux%u render); it stands down.", static_cast<unsigned>(f.renderW), static_cast<unsigned>(f.renderH));
        return;
    }
    if (made) {
        const std::string vram = formatFrameGenSynthVram(eye, vramOf(r, g.scratch));
        Log::get().note("%s", vram.c_str());
    }
    pollReadbacks(ctx, r, r.writes);
    pollTimers(ctx);

    const bool half = f.wantShown || ((r.calls++ & 1u) == 0u);   // the even frames (and the dump's) build the f = 0.5 S
    const bool score = r.havePending && r.bFrame + 1u == frame && !f.cameraJump;
    const bool keep = !half && !f.cameraJump;

    // On a score, the residual head rotation: the rotation the kept f = 1 S was built for against C's true one (this frame's backward head delta), and the tangents S was built with.
    float residual[9];
    if (score) {
        frameGenSynthResidual(r.keptRot, f.deltas.Hd, residual);
        g.rotRes.push_back(temporalRotationAngleDeg(residual));
    }
    FrameGenSynthCb cb;
    frameGenSynthFillCb(cb, f.renderW, f.renderH, f.outW, f.outH, f.tanNow, f.depthA, f.depthB, f.splitMetres, half ? 0.5f : 1.0f, f.deltas, score ? residual : nullptr, score ? r.keptTan : nullptr);
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(ctx->Map(g.cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        ++g.win.skipped;
        r.havePending = false;
        return;
    }
    std::memcpy(m.pData, &cb, sizeof(cb));
    ctx->Unmap(g.cb, 0);
    Scratch& sc = g.scratch;

    // A staging slot still unread when it comes round again is overwritten (its frame is lost to the window, not waited for).
    const int slot = static_cast<int>(r.writes % kStagingDepth);
    r.pending[slot] = false;

    TimerSlot* timer = beginTimer(dev, ctx, half ? 0 : 1);
    CsStageSave save;
    save.save(ctx);
    ctx->CSSetConstantBuffers(0, 1, &g.cb);
    FrameGenSynthPassViews v;
    v.out = f.outSrv;
    v.depth = f.depthSrv;
    v.mv = f.mvSrv;
    v.keySrv = sc.key.srv; v.keyUav = sc.key.uav;
    v.readSrv = sc.read.srv; v.readUav = sc.read.uav;
    v.holesUav = sc.holesUav;
    v.sSrv = r.s.srv; v.sUav = r.s.uav;
    v.sLumaSrv = r.sLuma.srv; v.sLumaUav = r.sLuma.uav;
    v.bInfoSrv = r.bInfo.srv; v.bInfoUav = r.bInfo.uav;
    v.statsUav = r.statsUav;
    v.rw = r.rw; v.rh = r.rh; v.ow = r.ow; v.oh = r.oh; v.qw = r.qw; v.qh = r.qh;
    frameGenSynthRunPasses(ctx, g.cs, v, score, true, keep);
    ctx->CopyResource(r.staging[slot], r.stats);
    r.pending[slot] = true;
    r.slotKind[slot] = half ? 0 : 1;
    r.slotScored[slot] = score;
    ++r.writes;
    if (timer) timer->timer.end(ctx);
    save.restore(ctx);

    r.havePending = keep;
    if (keep) {
        for (int i = 0; i < 3; ++i)
            for (int k = 0; k < 3; ++k) r.keptRot[i * 3 + k] = cb.rs[i][k];
        for (int i = 0; i < 4; ++i) r.keptTan[i] = f.tanNow[i];
    }
    r.bFrame = frame;
    r.shownValid = half;
    r.shownFrame = frame;
    if (f.replay) replayCapture(dev, ctx, f, frame, cb);
}

bool frameGenReplayFinish(const wchar_t* eyesDir, const wchar_t* stamp) {
    if (!rp.open || rp.writing || !eyesDir || !stamp) return false;
    if (rp.count == 0) {
        Log::get().note("frame generation (synth): replay capture %ls: nothing captured (%s).", stamp, rp.stop.empty() ? "the synthesizer did not run on the run's first frame" : rp.stop.c_str());
        replayRelease();
        return false;
    }
    const std::wstring dir = std::wstring(eyesDir) + L"\\fgreplay_" + stamp;
    CreateDirectoryW(eyesDir, nullptr);
    if (!CreateDirectoryW(dir.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
        Log::get().note("frame generation (synth): replay capture %ls: could not make %ls.", stamp, dir.c_str());
        replayRelease();
        return false;
    }
    rp.dir = dir;
    rp.stamp = stamp;
    rp.files.clear();
    rp.job = 0;
    rp.writing = true;
    return true;
}

void frameGenReplayPump() {
    if (!rp.writing) return;
    if (rp.workerRunning) {
        if (!rp.workerDone.load(std::memory_order_acquire)) return;
        rp.worker.join();
        rp.workerRunning = false;
        ID3D11Texture2D*& t = rp.tex[rp.job / 3][rp.job % 3];
        if (rp.mapped && rp.mapCtx && t) rp.mapCtx->Unmap(t, 0);
        rp.mapped = false;
        releaseCom(rp.mapCtx);
        releaseCom(t);
        if (rp.resultOk) rp.files.push_back(rp.result);
        else if (rp.stop.empty()) rp.stop = "could not write " + rp.result.name;
        ++rp.job;
    }
    if (rp.job >= rp.count * 3) { replayFinishSet(); return; }
    const uint32_t k = rp.job / 3, part = rp.job % 3;
    ID3D11Texture2D* t = rp.tex[k][part];
    ID3D11Device* dev = nullptr;
    t->GetDevice(&dev);
    ID3D11DeviceContext* ctx = nullptr;
    if (dev) { dev->GetImmediateContext(&ctx); dev->Release(); }
    if (!ctx) { releaseCom(t); ++rp.job; if (rp.stop.empty()) rp.stop = "no device context to read the capture back"; return; }
    D3D11_MAPPED_SUBRESOURCE m{};
    const HRESULT hr = ctx->Map(t, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m);   // never waits: the copy is tried again next frame
    if (hr == DXGI_ERROR_WAS_STILL_DRAWING) { ctx->Release(); return; }
    char name[32];
    std::snprintf(name, sizeof(name), "f%u_%s.bin", k, kReplayPart[part]);
    if (FAILED(hr)) {
        ctx->Release();
        releaseCom(t);
        ++rp.job;
        if (rp.stop.empty()) rp.stop = std::string("could not read back ") + name;
        return;
    }
    D3D11_TEXTURE2D_DESC d{};
    t->GetDesc(&d);
    rp.mapped = true;
    rp.mapCtx = ctx;   // keeps the reference until the unmap
    rp.map = m;
    rp.result = FrameGenReplayFile{};
    rp.result.name = name;
    rp.workerDone.store(false, std::memory_order_relaxed);
    rp.workerRunning = true;
    rp.worker = std::thread(replayWorker, static_cast<const uint8_t*>(m.pData), static_cast<uint32_t>(m.RowPitch), d.Height, d.Width * frameGenReplayFormatBytes(d.Format), rp.dir + L"\\" + std::wstring(name, name + std::strlen(name)));
}

bool frameGenReplayIdle() { return !rp.open && !rp.writing && !rp.workerRunning && rp.count == 0; }
uint32_t frameGenReplayCaptured() { return rp.count; }

void frameGenSynthFrameBoundary() {
    static int64_t s_ticks = 0;
    if (g_on.load()) frameGenReplayPump();
    if (!g_on.load()) {
        if (rp.open || rp.writing || rp.count) replayRelease();   // the key went off: nothing captured is kept
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
    fillRotation(g.win);
    const std::string line = formatFrameGenSynthReport(g.win);
    Log::get().note("%s", line.c_str());
    windowReset();
}

}  // namespace edvr
