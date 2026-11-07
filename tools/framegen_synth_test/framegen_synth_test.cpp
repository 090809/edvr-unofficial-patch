// framegen_synth_test: the frame-generation synthesizer (fix.frame_generation_shadow, TEMPORARY; docs/design-frame-generation-2026-10-26.md, Phase 2.1) on WARP.
//
//   --dry-run    the same run (the rig never writes a file), for the gate's --dry-run convention
//   --self-test  every check, then the mutations (each must make its case fail)
//   --verbose    the numbers of each case
//
// The production bytecode (kFrameGenSynth*Bytecode, from tools\temporal_shader_build) is run on a WARP device over a scene whose upscaled output is a gradient (red the
// render x, green the render y of the texel, in render pixels, so a pixel of S tells exactly where in B it was read) and the S texture it writes is held to the CPU
// reference in src\common\framegen_synth_math.h: for each output pixel c that read B at r, the reference's landing of r must be c, to under 0.1 px.
//
// Cases:
//   1  a cockpit plane at 1 m under a head translation and turn (f = 0.5), at 1:1 and at 2:1 output
//   2  a world plane at 50 m with the ship's screen motion constant and the head turning (the ex2dh rule), and with the head still the plain p - f m
//   3  the far plane under the same
//   4  the on-foot 1 mm split: a plane at 0.5 mm is the cockpit and moves with the head, one at 2 mm is the world and, its vectors zero and the head still, does not
//   5  the guard and the fill: a near object's edge over the sky as the object moves away. No sky pixel takes the near colour (the guard), the strip the object left is
//      filled from the sky (the background pull, never from the object), and the counters say so
//   6  the module itself (framegen_synth.cpp) on the same device: the key off creates nothing; on, it builds the two variants in turn, scores the f = 1 one against the
//      next frame (a static scene scores in bin 0, a scene scrolling at the vectors' rate scores well against a no-warp comparison that does not), counts the frames it
//      could not build, leaves a gap unscored, hands out the f = 0.5 S, and releases everything when the key goes off
// Mutations (each replaces one of the shader's four #define switches, or the host's f, and the case it guards must fail): f ignored, the head turn not taken out of the
// vector, the guard off, the fill from the nearer side.
#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include <d3dcompiler.h>
#include "framegen_synth_shader.h"

#include "../../src/common/framegen_synth_math.h"
#include "framegen_synth.h"
#include "temporal_shader_bytecode.h"

using Microsoft::WRL::ComPtr;
using namespace edvr;

// shader_swap.cpp reports its compile times to the overlay's counters; the rig has no overlay.
namespace edvr { void perfMonitorNoteEvent(uint32_t, double) {} }

namespace {

bool g_verbose = false;
unsigned g_checks = 0, g_failures = 0;

// A scope collects a case's failures: a mutation runs the case quietly and expects it to fail.
struct Scope {
    bool quiet = false;
    unsigned fails = 0;
    void ok(bool cond, const char* label) {
        if (quiet) { if (!cond) ++fails; return; }
        ++g_checks;
        if (!cond) {
            ++g_failures;
            ++fails;
            std::printf("FAIL: %s\n", label);
            std::fflush(stdout);
        }
    }
};

constexpr int RW = 128, RH = 96;
constexpr int QW = RW / 4, QH = RH / 4;
constexpr float kPi = 3.14159265358979f;
const float kTan[4] = {-1.0f, 1.0f, -0.75f, 0.75f};
const float kDepthB = 0.025f;   // Elite's infinite-far reversed-Z: written = B / metres, A = 0

uint16_t floatToHalf(float f) {
    uint32_t u;
    std::memcpy(&u, &f, 4);
    const uint32_t sign = (u >> 16) & 0x8000u;
    int e = static_cast<int>((u >> 23) & 0xFF) - 127 + 15;
    uint32_t m = u & 0x7FFFFFu;
    if (e <= 0) return static_cast<uint16_t>(sign);
    if (e >= 31) return static_cast<uint16_t>(sign | 0x7BFFu);
    uint32_t h = (static_cast<uint32_t>(e) << 10) | (m >> 13);
    if (m & 0x1000u) ++h;
    return static_cast<uint16_t>(sign | h);
}

void rotY(float deg, float m[9]) {
    const float r = deg * kPi / 180.0f, c = std::cos(r), s = std::sin(r);
    const float v[9] = {c, 0, s, 0, 1, 0, -s, 0, c};
    std::memcpy(m, v, sizeof(v));
}

// ---- the GPU harness -------------------------------------------------------------------------------------------------------------------------------------

struct Tex {
    ComPtr<ID3D11Texture2D> t;
    ComPtr<ID3D11ShaderResourceView> srv;
    ComPtr<ID3D11UnorderedAccessView> uav;
};

constexpr int kEntries = 8, kSrvSlots = 10, kUavSlots = 8;
const char* const kEntryNames[kEntries] = {"fgSynClear", "fgSynDilate", "fgSynSplat", "fgSynSrc", "fgSynResolve", "fgSynResample", "fgSynLuma", "fgSynScore"};

struct Gpu {
    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    ComPtr<ID3D11ComputeShader> cs[kEntries];
    ComPtr<ID3D11Buffer> cb;

    bool init() {
        D3D_FEATURE_LEVEL level{};
        if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &dev, &level, &ctx))) return false;
        const struct { const unsigned char* code; size_t size; } codes[kEntries] = {
            {kFrameGenSynthClearBytecode, sizeof(kFrameGenSynthClearBytecode)}, {kFrameGenSynthDilateBytecode, sizeof(kFrameGenSynthDilateBytecode)},
            {kFrameGenSynthSplatBytecode, sizeof(kFrameGenSynthSplatBytecode)}, {kFrameGenSynthSrcBytecode, sizeof(kFrameGenSynthSrcBytecode)},
            {kFrameGenSynthResolveBytecode, sizeof(kFrameGenSynthResolveBytecode)}, {kFrameGenSynthResampleBytecode, sizeof(kFrameGenSynthResampleBytecode)},
            {kFrameGenSynthLumaBytecode, sizeof(kFrameGenSynthLumaBytecode)}, {kFrameGenSynthScoreBytecode, sizeof(kFrameGenSynthScoreBytecode)}};
        for (int i = 0; i < kEntries; ++i)
            if (FAILED(dev->CreateComputeShader(codes[i].code, codes[i].size, nullptr, &cs[i]))) return false;
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = sizeof(FrameGenSynthCb);
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        return SUCCEEDED(dev->CreateBuffer(&bd, nullptr, &cb));
    }

    bool makeTex(int w, int h, DXGI_FORMAT fmt, const void* data, UINT pitch, Tex& out, bool wantUav) {
        D3D11_TEXTURE2D_DESC td{};
        td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1; td.Format = fmt; td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE | (wantUav ? D3D11_BIND_UNORDERED_ACCESS : 0u);
        D3D11_SUBRESOURCE_DATA sd{data, pitch, 0};
        if (FAILED(dev->CreateTexture2D(&td, data ? &sd : nullptr, &out.t))) return false;
        if (FAILED(dev->CreateShaderResourceView(out.t.Get(), nullptr, &out.srv))) return false;
        if (wantUav && FAILED(dev->CreateUnorderedAccessView(out.t.Get(), nullptr, &out.uav))) return false;
        return true;
    }

    template <class T>
    std::vector<T> readTex(ID3D11Texture2D* src, int w, int h) {
        D3D11_TEXTURE2D_DESC td{};
        src->GetDesc(&td);
        td.Usage = D3D11_USAGE_STAGING; td.BindFlags = 0; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> st;
        std::vector<T> v(static_cast<size_t>(w) * h);
        if (FAILED(dev->CreateTexture2D(&td, nullptr, &st))) return v;
        ctx->CopyResource(st.Get(), src);
        D3D11_MAPPED_SUBRESOURCE m{};
        if (FAILED(ctx->Map(st.Get(), 0, D3D11_MAP_READ, 0, &m))) return v;
        for (int y = 0; y < h; ++y) std::memcpy(&v[static_cast<size_t>(y) * w], static_cast<const char*>(m.pData) + static_cast<size_t>(y) * m.RowPitch, sizeof(T) * w);
        ctx->Unmap(st.Get(), 0);
        return v;
    }

    void setCb(const FrameGenSynthCb& c) {
        D3D11_MAPPED_SUBRESOURCE m{};
        if (SUCCEEDED(ctx->Map(cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
            std::memcpy(m.pData, &c, sizeof(c));
            ctx->Unmap(cb.Get(), 0);
        }
        ID3D11Buffer* b = cb.Get();
        ctx->CSSetConstantBuffers(0, 1, &b);
    }

    void run(int entry, ID3D11ShaderResourceView* const srv[kSrvSlots], ID3D11UnorderedAccessView* const uav[kUavSlots], int gx, int gy) {
        ctx->CSSetShader(cs[entry].Get(), nullptr, 0);
        // Nothing stays bound from the last dispatch: a resource still bound as a UAV would silently null its SRV here.
        ID3D11ShaderResourceView* const noSrv[kSrvSlots] = {};
        ID3D11UnorderedAccessView* const noUav[kUavSlots] = {};
        ctx->CSSetShaderResources(0, kSrvSlots, noSrv);
        ctx->CSSetUnorderedAccessViews(0, kUavSlots, noUav, nullptr);
        ctx->CSSetShaderResources(0, kSrvSlots, srv);
        ctx->CSSetUnorderedAccessViews(0, kUavSlots, uav, nullptr);
        ctx->Dispatch(static_cast<UINT>(gx), static_cast<UINT>(gy), 1);
    }
};

// The production text with one of the four switches turned off, compiled for every entry; `defineName` is "FG_GUARD" and so on.
bool compileMutated(Gpu& g, const char* defineName, ComPtr<ID3D11ComputeShader> out[kEntries]) {
    HMODULE lib = LoadLibraryA("d3dcompiler_47.dll");
    if (!lib) { std::printf("  d3dcompiler_47.dll did not load\n"); return false; }
    using CompileFn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);
    const CompileFn compile = reinterpret_cast<CompileFn>(GetProcAddress(lib, "D3DCompile"));
    if (!compile) return false;
    std::string text = frameGenSynthShaderSource();
    const std::string from = std::string("#define ") + defineName + " 1";
    const size_t at = text.find(from);
    if (at == std::string::npos) return false;
    text.replace(at, from.size(), std::string("#define ") + defineName + " 0");
    for (int i = 0; i < kEntries; ++i) {
        ComPtr<ID3DBlob> code, err;
        if (FAILED(compile(text.data(), text.size(), "framegen_synth_mutated", nullptr, nullptr, kEntryNames[i], "cs_5_0", 0, 0, &code, &err))) {
            if (err) std::printf("  %s: %s\n", kEntryNames[i], static_cast<const char*>(err->GetBufferPointer()));
            return false;
        }
        const HRESULT created = g.dev->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &out[i]);
        if (FAILED(created)) {
            std::printf("  %s: CreateComputeShader failed (0x%08X; the device's removed reason is 0x%08X)\n", kEntryNames[i], static_cast<unsigned>(created), static_cast<unsigned>(g.dev->GetDeviceRemovedReason()));
            return false;
        }
    }
    return true;
}

// ---- a scene and one run of the pass ---------------------------------------------------------------------------------------------------------------------

struct Scene {
    int w = RW, h = RH, ow = RW, oh = RH;
    std::vector<float> rgba;    // the upscaler's output, output size, RGBA32F
    std::vector<float> depth;   // the written depth, render size
    std::vector<float> mv;      // the DLSS vectors, 2 floats a render pixel, B -> A, pixels
    Scene(int ow_ = RW, int oh_ = RH) : ow(ow_), oh(oh_), rgba(static_cast<size_t>(ow_) * oh_ * 4, 0.0f), depth(static_cast<size_t>(RW) * RH, 0.0f), mv(static_cast<size_t>(RW) * RH * 2, 0.0f) {}
    float scale() const { return static_cast<float>(ow) / static_cast<float>(w); }
    // The gradient: red the render x, green the render y of the output texel's centre.
    void gradient() {
        const float s = scale();
        for (int y = 0; y < oh; ++y)
            for (int x = 0; x < ow; ++x) {
                float* p = &rgba[(static_cast<size_t>(y) * ow + x) * 4];
                p[0] = (static_cast<float>(x) + 0.5f) / s - 0.5f;
                p[1] = (static_cast<float>(y) + 0.5f) / s - 0.5f;
                p[2] = 0.5f;
                p[3] = 1.0f;
            }
    }
    void grey(const std::function<float(int, int)>& f) {
        for (int y = 0; y < oh; ++y)
            for (int x = 0; x < ow; ++x) {
                float* p = &rgba[(static_cast<size_t>(y) * ow + x) * 4];
                p[0] = p[1] = p[2] = f(x, y);
                p[3] = 1.0f;
            }
    }
};

struct Run {
    std::vector<float> s;           // S, output size, RGBA32F
    std::vector<uint32_t> stats;
    bool ok = false;
    float at(int x, int y, int ch, int ow) const { return s[(static_cast<size_t>(y) * ow + x) * 4 + ch]; }
};

bool makeInputs(Gpu& g, const Scene& sc, Tex& out, Tex& depth, Tex& mv) {
    std::vector<uint16_t> half(sc.mv.size());
    for (size_t i = 0; i < sc.mv.size(); ++i) half[i] = floatToHalf(sc.mv[i]);
    return g.makeTex(sc.ow, sc.oh, DXGI_FORMAT_R32G32B32A32_FLOAT, sc.rgba.data(), static_cast<UINT>(sc.ow * 16), out, false) &&
           g.makeTex(sc.w, sc.h, DXGI_FORMAT_R32_FLOAT, sc.depth.data(), static_cast<UINT>(sc.w * 4), depth, false) &&
           g.makeTex(sc.w, sc.h, DXGI_FORMAT_R16G16_FLOAT, half.data(), static_cast<UINT>(sc.w * 4), mv, false);
}

// The module's order of dispatches for one eye-frame, up to and including the resample.
Run runSynth(Gpu& g, const Scene& sc, const FrameGenSynthCb& cb) {
    Run r;
    Tex out, depth, mv, depthD, key, src, read, s;
    if (!makeInputs(g, sc, out, depth, mv)) return r;
    if (!g.makeTex(sc.w, sc.h, DXGI_FORMAT_R32_FLOAT, nullptr, 0, depthD, true) || !g.makeTex(sc.w, sc.h, DXGI_FORMAT_R32_UINT, nullptr, 0, key, true) ||
        !g.makeTex(sc.w, sc.h, DXGI_FORMAT_R32_UINT, nullptr, 0, src, true) || !g.makeTex(sc.w, sc.h, DXGI_FORMAT_R32G32_FLOAT, nullptr, 0, read, true) ||
        !g.makeTex(sc.ow, sc.oh, DXGI_FORMAT_R32G32B32A32_FLOAT, nullptr, 0, s, true))
        return r;
    ComPtr<ID3D11Buffer> stats, staging;
    ComPtr<ID3D11UnorderedAccessView> statsUav;
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = kFrameGenSynthStatsUints * 4; bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED; bd.StructureByteStride = 4;
    if (FAILED(g.dev->CreateBuffer(&bd, nullptr, &stats))) return r;
    D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
    ud.Format = DXGI_FORMAT_UNKNOWN; ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER; ud.Buffer.NumElements = kFrameGenSynthStatsUints;
    if (FAILED(g.dev->CreateUnorderedAccessView(stats.Get(), &ud, &statsUav))) return r;
    bd.Usage = D3D11_USAGE_STAGING; bd.BindFlags = 0; bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(g.dev->CreateBuffer(&bd, nullptr, &staging))) return r;

    g.setCb(cb);
    const int rgx = (sc.w + 7) / 8, rgy = (sc.h + 7) / 8, ogx = (sc.ow + 7) / 8, ogy = (sc.oh + 7) / 8;
    ID3D11ShaderResourceView* const none[kSrvSlots] = {};
    {
        ID3D11UnorderedAccessView* uav[kUavSlots] = {nullptr, key.uav.Get(), src.uav.Get(), nullptr, nullptr, statsUav.Get()};
        g.run(0, none, uav, rgx, rgy);
    }
    {
        ID3D11ShaderResourceView* srv[kSrvSlots] = {nullptr, depth.srv.Get()};
        ID3D11UnorderedAccessView* uav[kUavSlots] = {depthD.uav.Get()};
        g.run(1, srv, uav, rgx, rgy);
    }
    {
        ID3D11ShaderResourceView* srv[kSrvSlots] = {nullptr, depth.srv.Get(), mv.srv.Get(), depthD.srv.Get()};
        ID3D11UnorderedAccessView* uav[kUavSlots] = {nullptr, key.uav.Get()};
        g.run(2, srv, uav, rgx, rgy);
    }
    {
        ID3D11ShaderResourceView* srv[kSrvSlots] = {nullptr, depth.srv.Get(), mv.srv.Get(), depthD.srv.Get(), key.srv.Get()};
        ID3D11UnorderedAccessView* uav[kUavSlots] = {nullptr, nullptr, src.uav.Get()};
        g.run(3, srv, uav, rgx, rgy);
    }
    {
        ID3D11ShaderResourceView* srv[kSrvSlots] = {nullptr, depth.srv.Get(), mv.srv.Get(), depthD.srv.Get(), key.srv.Get(), src.srv.Get()};
        ID3D11UnorderedAccessView* uav[kUavSlots] = {nullptr, nullptr, nullptr, read.uav.Get(), nullptr, statsUav.Get()};
        g.run(4, srv, uav, rgx, rgy);
    }
    {
        ID3D11ShaderResourceView* srv[kSrvSlots] = {out.srv.Get(), nullptr, nullptr, nullptr, nullptr, nullptr, read.srv.Get()};
        ID3D11UnorderedAccessView* uav[kUavSlots] = {nullptr, nullptr, nullptr, nullptr, s.uav.Get()};
        g.run(5, srv, uav, ogx, ogy);
    }
    ID3D11UnorderedAccessView* noUav[kUavSlots] = {};
    g.ctx->CSSetShaderResources(0, kSrvSlots, none);
    g.ctx->CSSetUnorderedAccessViews(0, kUavSlots, noUav, nullptr);
    g.ctx->CopyResource(staging.Get(), stats.Get());
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(g.ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m))) return r;
    r.stats.assign(static_cast<const uint32_t*>(m.pData), static_cast<const uint32_t*>(m.pData) + kFrameGenSynthStatsUints);
    g.ctx->Unmap(staging.Get(), 0);
    r.s = g.readTex<float>(s.t.Get(), sc.ow * 4, sc.oh);
    r.ok = true;
    return r;
}

// ---- the references --------------------------------------------------------------------------------------------------------------------------------------

// Where the head's B-to-A rotation alone puts the pixel (the shader's pHeadPrev).
bool headPrev(const FrameGenSynthCb& cb, float px, float py, float out[2]) {
    float d[3], dh[3], hd[9];
    temporalPixelToDir(px, py, cb.tanB, RW, RH, d);
    for (int r = 0; r < 3; ++r)
        for (int k = 0; k < 3; ++k) hd[r * 3 + k] = cb.hp[r][k];
    temporalApply3(hd, d, dh);
    return temporalDirToPixel(dh, cb.tanB, RW, RH, &out[0], &out[1]);
}

// The vector field of a surface the ship moves at screen velocity m (B to A, pixels) under the head rotation of `cb`: the head's part plus m.
void fillVectors(Scene& sc, const FrameGenSynthCb& cb, float mx, float my, const std::function<bool(int, int)>& inside) {
    for (int y = 0; y < RH; ++y)
        for (int x = 0; x < RW; ++x) {
            float h[2];
            if (!inside(x, y) || !headPrev(cb, static_cast<float>(x), static_cast<float>(y), h)) continue;
            sc.mv[(static_cast<size_t>(y) * RW + x) * 2] = h[0] + mx - static_cast<float>(x);
            sc.mv[(static_cast<size_t>(y) * RW + x) * 2 + 1] = h[1] + my - static_cast<float>(y);
        }
}

struct Worst { float err = 0.0f; int n = 0; };

// For every output pixel whose render position is `margin` or more from the edges: the reference's landing of the position S read B at must be the pixel's own render
// position. `landing` is the reference for a render position (it knows the class, the depth and the vector there).
Worst mappingError(const Run& r, const Scene& sc, int margin, const std::function<bool(float, float, float[2])>& landing) {
    Worst w;
    const float s = sc.scale();
    for (int oy = 0; oy < sc.oh; ++oy)
        for (int ox = 0; ox < sc.ow; ++ox) {
            const float cx = (static_cast<float>(ox) + 0.5f) / s - 0.5f, cy = (static_cast<float>(oy) + 0.5f) / s - 0.5f;
            if (cx < margin || cy < margin || cx > RW - 1 - margin || cy > RH - 1 - margin) continue;
            float pp[2];
            if (!landing(r.at(ox, oy, 0, sc.ow), r.at(ox, oy, 1, sc.ow), pp)) { w.err = 1e9f; continue; }
            w.err = std::max(w.err, std::hypot(pp[0] - cx, pp[1] - cy));
            ++w.n;
        }
    return w;
}

struct Deltas {
    FrameGenDeltas d;
    Deltas(float turnDeg, float tx, float ty, float tz) { rotY(turnDeg, d.Hd); d.th[0] = tx; d.th[1] = ty; d.th[2] = tz; }
};

float depthOf(float metres) { return kDepthB / metres; }

// Case 1: a cockpit plane at 1 m under a head translation and turn.
void case1Cockpit(Gpu& g, Scope& sc, int outScale) {
    char label[200];
    Scene S(RW * outScale, RH * outScale);
    S.gradient();
    std::fill(S.depth.begin(), S.depth.end(), depthOf(1.0f));
    FrameGenSynthCb cb;
    frameGenSynthFillCb(cb, RW, RH, S.ow, S.oh, kTan, 0.0f, kDepthB, 10.0f, 0.5f, Deltas(3.0f, -0.05f, 0.02f, -0.01f).d);
    const Run r = runSynth(g, S, cb);
    sc.ok(r.ok, "case 1: the synthesizer ran");
    if (!r.ok) return;
    const Worst w = mappingError(r, S, 10, [&](float x, float y, float pp[2]) { return frameGenSynthTarget(cb, x, y, 1.0f, false, 0.0f, 0.0f, pp); });
    std::snprintf(label, sizeof(label), "case 1: a cockpit plane at 1 m under a head step lands where the reference says, at %d:1 output (worst %.4f px over %d pixels)", outScale, w.err, w.n);
    sc.ok(w.n > 1000 && w.err < 0.1f, label);
    // The head moved the plane: the shift of the centre is the reference's, and it is not nothing.
    float pp[2];
    frameGenSynthTarget(cb, 64.0f, 48.0f, 1.0f, false, 0.0f, 0.0f, pp);
    std::snprintf(label, sizeof(label), "case 1: the plane really moves (the centre pixel lands %.2f px away)", std::hypot(pp[0] - 64.0f, pp[1] - 48.0f));
    sc.ok(std::hypot(pp[0] - 64.0f, pp[1] - 48.0f) > 1.0f, label);
    if (g_verbose && !sc.quiet) std::printf("  case 1 (%d:1): worst %.4f px over %d pixels, holes left %u, filled %u\n", outScale, w.err, w.n, r.stats[kFrameGenSynthStatsLeft], r.stats[kFrameGenSynthStatsFilled]);
}

// Case 2: a world plane at 50 m; mutation hooks run it with a turning head (m non-zero) and compare to the reference.
void case2World(Gpu& g, Scope& sc) {
    char label[200];
    {   // the head turning, the ship's screen motion constant: the ex2dh rule
        Scene S;
        S.gradient();
        std::fill(S.depth.begin(), S.depth.end(), depthOf(50.0f));
        FrameGenSynthCb cb;
        frameGenSynthFillCb(cb, RW, RH, RW, RH, kTan, 0.0f, kDepthB, 10.0f, 0.5f, Deltas(2.0f, 0.0f, 0.0f, 0.0f).d);
        const float mx = 3.0f, my = -2.0f;
        fillVectors(S, cb, mx, my, [](int, int) { return true; });
        const Run r = runSynth(g, S, cb);
        sc.ok(r.ok, "case 2: the synthesizer ran");
        if (!r.ok) return;
        const Worst w = mappingError(r, S, 12, [&](float x, float y, float pp[2]) {
            float h[2];
            if (!headPrev(cb, x, y, h)) return false;
            return frameGenSynthTarget(cb, x, y, 50.0f, false, h[0] + mx - x, h[1] + my - y, pp);
        });
        std::snprintf(label, sizeof(label), "case 2: a world plane at a constant ship screen velocity under a head turn lands where the ex2dh reference says (worst %.4f px over %d pixels)", w.err, w.n);
        sc.ok(w.n > 1000 && w.err < 0.1f, label);
        // The head's turn is a real part of the vector here (so a shader that left it in would double it).
        float h[2];
        headPrev(cb, 64.0f, 48.0f, h);
        std::snprintf(label, sizeof(label), "case 2: the head's part of the vector is %.2f px at the centre", std::hypot(h[0] - 64.0f, h[1] - 48.0f));
        sc.ok(std::hypot(h[0] - 64.0f, h[1] - 48.0f) > 1.5f, label);
        if (g_verbose && !sc.quiet) std::printf("  case 2: worst %.4f px over %d pixels\n", w.err, w.n);
    }
    {   // the head still: S(c) = B(c + f m), independent of the reference
        Scene S;
        S.gradient();
        std::fill(S.depth.begin(), S.depth.end(), depthOf(50.0f));
        FrameGenSynthCb cb;
        frameGenSynthFillCb(cb, RW, RH, RW, RH, kTan, 0.0f, kDepthB, 10.0f, 0.5f, Deltas(0.0f, 0.0f, 0.0f, 0.0f).d);
        for (size_t i = 0; i < S.mv.size(); i += 2) { S.mv[i] = 6.0f; S.mv[i + 1] = -2.0f; }
        const Run r = runSynth(g, S, cb);
        sc.ok(r.ok, "case 2b: the synthesizer ran");
        if (!r.ok) return;
        float worst = 0.0f;
        int n = 0;
        for (int y = 12; y < RH - 12; ++y)
            for (int x = 12; x < RW - 12; ++x) {
                worst = std::max(worst, std::hypot(r.at(x, y, 0, RW) - (x + 3.0f), r.at(x, y, 1, RW) - (y - 1.0f)));
                ++n;
            }
        std::snprintf(label, sizeof(label), "case 2b: with the head still a world pixel moves by -f of its vector: S(c) reads B at c + (3, -1) for the vector (6, -2) and f 0.5 (worst %.4f px)", worst);
        sc.ok(n > 1000 && worst < 0.05f, label);
    }
}

// Case 3: the far plane (the written depth 0) under the same.
void case3Far(Gpu& g, Scope& sc) {
    char label[200];
    Scene S;
    S.gradient();
    FrameGenSynthCb cb;
    frameGenSynthFillCb(cb, RW, RH, RW, RH, kTan, 0.0f, kDepthB, 10.0f, 0.5f, Deltas(-1.5f, 0.0f, 0.0f, 0.0f).d);
    const float mx = -2.0f, my = 1.5f;
    fillVectors(S, cb, mx, my, [](int, int) { return true; });
    const Run r = runSynth(g, S, cb);
    sc.ok(r.ok, "case 3: the synthesizer ran");
    if (!r.ok) return;
    const Worst w = mappingError(r, S, 12, [&](float x, float y, float pp[2]) {
        float h[2];
        if (!headPrev(cb, x, y, h)) return false;
        return frameGenSynthTarget(cb, x, y, 0.0f, true, h[0] + mx - x, h[1] + my - y, pp);
    });
    std::snprintf(label, sizeof(label), "case 3: the far plane rides the vectors' rotation like the world (worst %.4f px over %d pixels)", w.err, w.n);
    sc.ok(w.n > 1000 && w.err < 0.1f, label);
}

// Case 4: the on-foot 1 mm split.
void case4Split(Gpu& g, Scope& sc) {
    char label[220];
    Scene S;
    S.gradient();
    for (int y = 0; y < RH; ++y)
        for (int x = 0; x < RW; ++x) S.depth[static_cast<size_t>(y) * RW + x] = x < 64 ? depthOf(0.0005f) : depthOf(0.002f);
    FrameGenSynthCb cb;
    frameGenSynthFillCb(cb, RW, RH, RW, RH, kTan, 0.0f, kDepthB, 0.001f, 0.5f, Deltas(0.0f, 4e-5f, 0.0f, 0.0f).d);
    const Run r = runSynth(g, S, cb);
    sc.ok(r.ok, "case 4: the synthesizer ran");
    if (!r.ok) return;
    float worstL = 0.0f, worstR = 0.0f;
    int nl = 0, nr = 0;
    for (int y = 12; y < RH - 12; ++y) {
        for (int x = 12; x < 52; ++x) {   // the cockpit half: the reference's landing of what it read is its own position
            float pp[2];
            if (!frameGenSynthTarget(cb, r.at(x, y, 0, RW), r.at(x, y, 1, RW), 0.0005f, false, 0.0f, 0.0f, pp)) { worstL = 1e9f; continue; }
            worstL = std::max(worstL, std::hypot(pp[0] - x, pp[1] - y));
            ++nl;
        }
        for (int x = 76; x < RW - 12; ++x) {   // the world half: the head still and the vectors zero, S is B unmoved
            worstR = std::max(worstR, std::hypot(r.at(x, y, 0, RW) - x, r.at(x, y, 1, RW) - y));
            ++nr;
        }
    }
    float pp[2];
    frameGenSynthTarget(cb, 30.0f, 48.0f, 0.0005f, false, 0.0f, 0.0f, pp);
    std::snprintf(label, sizeof(label), "case 4: 0.5 mm is the cockpit and moves with the head (the reference moves it %.2f px; worst %.4f px), 2 mm is the world and, its vectors zero, does not (worst %.4f px)",
                  std::fabs(pp[0] - 30.0f), worstL, worstR);
    sc.ok(nl > 500 && nr > 500 && worstL < 0.1f && worstR < 0.05f && std::fabs(pp[0] - 30.0f) > 1.0f, label);
}

// Case 5: the guard and the fill. The sky (left, written depth 0, grey 0.2) and an object at 50 m (right of x = 64, grey 0.9) whose vectors say it moves away from the
// sky edge: at f = 0.5 it lands 3.5 px to the right.
struct Edge { bool ran = false; float skyWorst = 0.0f, objWorst = 0.0f; unsigned filled = 0, left = 0, guard = 0; };

Edge case5Edge(Gpu& g, Scope& sc, bool report) {
    Edge e;
    char label[260];
    Scene S;
    S.grey([](int x, int) { return x < 64 ? 0.2f : 0.9f; });
    for (int y = 0; y < RH; ++y)
        for (int x = 0; x < RW; ++x) {
            S.depth[static_cast<size_t>(y) * RW + x] = x < 64 ? 0.0f : depthOf(50.0f);
            if (x >= 64) S.mv[(static_cast<size_t>(y) * RW + x) * 2] = -7.0f;
        }
    FrameGenSynthCb cb;
    frameGenSynthFillCb(cb, RW, RH, RW, RH, kTan, 0.0f, kDepthB, 10.0f, 0.5f, Deltas(0.0f, 0.0f, 0.0f, 0.0f).d);
    const Run r = runSynth(g, S, cb);
    sc.ok(r.ok, "case 5: the synthesizer ran");
    if (!r.ok) return e;
    e.ran = true;
    for (int y = 4; y < RH - 4; ++y) {
        for (int x = 8; x <= 66; ++x) e.skyWorst = std::max(e.skyWorst, std::fabs(r.at(x, y, 0, RW) - 0.2f));      // up to 1.5 px before the object's new edge at 67.5
        for (int x = 69; x < RW - 8; ++x) e.objWorst = std::max(e.objWorst, std::fabs(r.at(x, y, 0, RW) - 0.9f));  // and from 1.5 px after it
    }
    e.filled = r.stats[kFrameGenSynthStatsFilled];
    e.left = r.stats[kFrameGenSynthStatsLeft];
    e.guard = r.stats[kFrameGenSynthStatsGuard];
    std::snprintf(label, sizeof(label), "case 5: no sky pixel takes the object's colour (worst deviation from the sky %.4f, guard hits %u) and the strip the object left is sky (filled %u, left %u)", e.skyWorst, e.guard,
                  e.filled, e.left);
    sc.ok(e.skyWorst < 0.01f, label);
    std::snprintf(label, sizeof(label), "case 5: the object keeps its colour past its edge (worst deviation %.4f)", e.objWorst);
    sc.ok(e.objWorst < 0.01f, label);
    std::snprintf(label, sizeof(label), "case 5: the counters: the strip of 2 px by %d rows was filled from the background (%u), nothing was left (%u), the guard moved one read a row (%u)", RH, e.filled, e.left, e.guard);
    sc.ok(e.filled >= 2u * (RH - 2) && e.filled <= 2u * RH + 4 && e.left == 0 && e.guard >= static_cast<unsigned>(RH - 2) && e.guard <= static_cast<unsigned>(RH + 4), label);
    if (g_verbose && report) std::printf("  case 5: sky worst %.4f, object worst %.4f, filled %u, left %u, guard %u\n", e.skyWorst, e.objWorst, e.filled, e.left, e.guard);
    return e;
}

// ---- case 6: the module ----------------------------------------------------------------------------------------------------------------------------------

struct ModuleFrame {
    Tex out, depth, mv;
};

void case6Module(Gpu& g, Scope& sc) {
    char label[260];
    Scene base;
    base.depth.assign(base.depth.size(), 0.0f);
    for (int y = 0; y < RH; ++y)
        for (int x = 0; x < RW; ++x) base.depth[static_cast<size_t>(y) * RW + x] = x < 40 ? 0.0f : (x < 84 ? depthOf(0.8f) : depthOf(30.0f));
    // A scrolling pattern: frame k shows the pattern shifted right by 4 k px; the vectors (B to A) say so.
    const auto frameScene = [&](int k, float shift, bool vectors) {
        Scene s = base;
        s.grey([&](int x, int y) { return 0.5f + 0.3f * std::sin(0.21f * (static_cast<float>(x) - shift * k)) * std::cos(0.17f * y); });
        if (vectors)
            for (size_t i = 0; i < s.mv.size(); i += 2) s.mv[i] = -shift;
        return s;
    };
    FrameGenSynthFrame f;
    f.renderW = RW; f.renderH = RH; f.outW = RW; f.outH = RH;
    f.tanNow = kTan;
    f.depthA = 0.0f; f.depthB = kDepthB; f.splitMetres = 10.0f;
    f.depthWritten = true;

    // The key off: nothing is created, whatever the pass calls.
    frameGenSynthConfigure(false);
    {
        ModuleFrame m;
        const Scene s = frameScene(0, 0.0f, false);
        sc.ok(makeInputs(g, s, m.out, m.depth, m.mv), "case 6: the module's inputs were made");
        f.outSrv = m.out.srv.Get(); f.depthSrv = m.depth.srv.Get(); f.mvSrv = m.mv.srv.Get();
        for (uint32_t n = 1; n <= 6; ++n) { frameGenSynthEyeFrame(g.dev.Get(), g.ctx.Get(), n & 1u, n, f); frameGenSynthFrameBoundary(); }
        sc.ok(!frameGenSynthResident(), "case 6: with the key off nothing is created (no shader, buffer or texture is resident)");
        const FrameGenSynthWindow w = frameGenSynthOpenWindow();
        sc.ok(w.frames[0] == 0 && w.frames[1] == 0 && w.skipped == 0 && frameGenSynthShown(0, 6) == nullptr, "case 6: with the key off nothing is built, counted or handed out");
    }

    // On, a static scene on both eyes: the variants alternate, the f = 1 ones are scored against the next frame, and a static scene scores in bin 0 with no hole.
    frameGenSynthConfigure(true);
    uint32_t frame = 100;
    {
        ModuleFrame m;
        const Scene s = frameScene(0, 0.0f, false);
        sc.ok(makeInputs(g, s, m.out, m.depth, m.mv), "case 6: the static inputs were made");
        f.outSrv = m.out.srv.Get(); f.depthSrv = m.depth.srv.Get(); f.mvSrv = m.mv.srv.Get();
        f.deltas = FrameGenDeltas{};
        int shown = 0, shownOnOdd = 0;
        for (int n = 0; n < 12; ++n, ++frame) {
            for (uint32_t eye = 0; eye < 2; ++eye) {
                frameGenSynthEyeFrame(g.dev.Get(), g.ctx.Get(), eye, frame, f);
                if (ID3D11Texture2D* st = frameGenSynthShown(eye, frame)) {
                    ++shown;
                    if (n & 1) ++shownOnOdd;
                    if (g_verbose && n == 0 && eye == 0) {
                        const std::vector<float> px = g.readTex<float>(st, RW * 4, RH);
                        const std::vector<float> in = s.rgba;
                        std::printf("  case 6: first S centre %.4f %.4f %.4f (B %.4f %.4f %.4f), corner %.4f (B %.4f)\n", px[(48 * RW + 64) * 4], px[(48 * RW + 64) * 4 + 1], px[(48 * RW + 64) * 4 + 2],
                                    in[(48 * RW + 64) * 4], in[(48 * RW + 64) * 4 + 1], in[(48 * RW + 64) * 4 + 2], px[0], in[0]);
                    }
                }
            }
            g.ctx->Flush();
            Sleep(15);
        }
        sc.ok(frameGenSynthResident(), "case 6: with the key on the synthesizer's resources are resident");
        sc.ok(shown == 12 && shownOnOdd == 0, "case 6: the f = 0.5 S is handed out on the even frames of each eye only (12 of 24 calls, none of the odd)");
        sc.ok(frameGenSynthShown(0, 9999) == nullptr, "case 6: a frame the module did not build has no S to hand out");
        // The dump asks for the f = 0.5 S on a frame that would build the f = 1 one: the next call of the eye (frame n, an odd turn) with wantShown.
        FrameGenSynthFrame ask = f;
        ask.wantShown = true;
        frameGenSynthEyeFrame(g.dev.Get(), g.ctx.Get(), 0, frame++, f);
        frameGenSynthEyeFrame(g.dev.Get(), g.ctx.Get(), 0, frame, ask);
        sc.ok(frameGenSynthShown(0, frame) != nullptr, "case 6: wantShown builds the f = 0.5 S on a turn that would have built the f = 1 one");
        ++frame;
        for (int k = 0; k < 4; ++k) { g.ctx->Flush(); Sleep(15); frame += 10; frameGenSynthEyeFrame(g.dev.Get(), g.ctx.Get(), 0, frame, f); }   // gaps: read the rest back
        const FrameGenSynthWindow w = frameGenSynthOpenWindow();
        uint64_t px = 0, bin0 = 0;
        for (int c = 0; c < kFrameGenShadowClasses; ++c) { px += w.cls[c].pixels; bin0 += w.cls[c].hist[0]; }
        std::snprintf(label, sizeof(label), "case 6: the two variants alternate (%u built at f 0.5, %u at f 1) and the f = 1 ones are scored against the next frame (%u scored)", w.frames[0], w.frames[1], w.scored);
        sc.ok(w.frames[0] >= 13 && w.frames[1] >= 11 && w.scored >= 10 && w.scored <= w.frames[1], label);
        // The cells on the far/cockpit seam (a far pixel beside the cockpit takes the cockpit's class and motion, and the guard may read it a pixel off) differ by a few thousandths
        // of luma at most: almost every cell is in bin 0 and none is over 0.01.
        bool noneOver = true;
        for (int c = 0; c < kFrameGenShadowClasses; ++c) noneOver = noneOver && frameGenShadowShareOver(w.cls[c].hist, 0.01f) == 0.0f;
        std::snprintf(label, sizeof(label), "case 6: a static scene scores in bin 0 in every class (%llu of the %llu cells of %u eye-frames; none over 0.01)", static_cast<unsigned long long>(bin0),
                      static_cast<unsigned long long>(px), w.scored);
        sc.ok(px == static_cast<uint64_t>(w.scored) * QW * QH && bin0 * 100 >= px * 99 && noneOver && w.cells == px && w.cls[0].pixels > 0 && w.cls[1].pixels > 0 && w.cls[2].pixels > 0, label);
        if (g_verbose)
            for (int c = 0; c < kFrameGenShadowClasses; ++c) {
                std::printf("  case 6: class %d: %u cells, not in bin 0:", c, w.cls[c].pixels);
                for (int b = 1; b < kFrameGenShadowBins; ++b) if (w.cls[c].hist[b]) std::printf(" bin%d x%u", b, w.cls[c].hist[b]);
                std::printf("\n");
            }
        const uint64_t pixels = w.pixels;
        std::snprintf(label, sizeof(label), "case 6: a static scene fills no hole from the background (%llu) and leaves none (%llu); the guard moved %llu reads in %llu render pixels",
                      static_cast<unsigned long long>(w.filled), static_cast<unsigned long long>(w.left), static_cast<unsigned long long>(w.guard), static_cast<unsigned long long>(pixels));
        sc.ok(w.left == 0 && w.filled == 0 && pixels == static_cast<uint64_t>(w.frames[0] + w.frames[1]) * RW * RH, label);
        // A gap is not scored: after an f = 1 frame, the next call 5 frames later scores nothing.
        frameGenSynthEyeFrame(g.dev.Get(), g.ctx.Get(), 1, 4000, f);   // reads back what eye 1 still had in flight
        g.ctx->Flush();
        Sleep(15);
        frameGenSynthEyeFrame(g.dev.Get(), g.ctx.Get(), 1, 4100, f);
        const unsigned scoredBefore = frameGenSynthOpenWindow().scored;
        for (int k = 0; k < 6; ++k) frameGenSynthEyeFrame(g.dev.Get(), g.ctx.Get(), 1, 5000 + 7u * k, f);   // every call a gap from the one before it
        for (int k = 0; k < 4; ++k) { g.ctx->Flush(); Sleep(15); frameGenSynthEyeFrame(g.dev.Get(), g.ctx.Get(), 1, 6000 + 7u * k, f); }
        sc.ok(frameGenSynthOpenWindow().scored == scoredBefore, "case 6: an f = 1 frame followed by a gap in the frame numbers is not scored");
        // A frame the module cannot build from is counted, not built.
        const FrameGenSynthWindow w0 = frameGenSynthOpenWindow();
        FrameGenSynthFrame bad = f;
        bad.depthWritten = false;
        frameGenSynthEyeFrame(g.dev.Get(), g.ctx.Get(), 0, frame + 100, bad);
        bad = f;
        bad.mvSrv = nullptr;
        frameGenSynthEyeFrame(g.dev.Get(), g.ctx.Get(), 0, frame + 101, bad);
        const FrameGenSynthWindow w1 = frameGenSynthOpenWindow();
        sc.ok(w1.skipped == w0.skipped + 2 && w1.frames[0] == w0.frames[0] && w1.frames[1] == w0.frames[1], "case 6: a frame with no depth, or no vectors, is counted as skipped and builds nothing");
    }

    // A scene scrolling 4 px a frame at the vectors' rate (a world surface at 30 m): the f = 1 S lands on the next frame, the no-warp comparison does not.
    if (g_verbose) std::printf("  case 6: device after the static block: 0x%08X\n", static_cast<unsigned>(g.dev->GetDeviceRemovedReason()));
    frameGenSynthConfigure(false);
    frameGenSynthFrameBoundary();
    if (g_verbose) std::printf("  case 6: device after the release: 0x%08X\n", static_cast<unsigned>(g.dev->GetDeviceRemovedReason()));
    sc.ok(!frameGenSynthResident(), "case 6: turning the key off releases every resource at the next frame boundary");
    frameGenSynthConfigure(true);
    {
        for (size_t i = 0; i < base.depth.size(); ++i) base.depth[i] = depthOf(30.0f);
        ModuleFrame m;   // outside the loop: the gap calls below still hold the last frame's views
        for (int k = 0; k < 14; ++k) {
            const Scene s = frameScene(k, 4.0f, true);
            if (!makeInputs(g, s, m.out, m.depth, m.mv)) { sc.ok(false, "case 6: the scrolling inputs were made"); return; }
            f.outSrv = m.out.srv.Get(); f.depthSrv = m.depth.srv.Get(); f.mvSrv = m.mv.srv.Get();
            frameGenSynthEyeFrame(g.dev.Get(), g.ctx.Get(), 0, 300 + static_cast<uint32_t>(k), f);
            g.ctx->Flush();
            Sleep(15);
        }
        for (int k = 0; k < 4; ++k) { g.ctx->Flush(); Sleep(15); frameGenSynthEyeFrame(g.dev.Get(), g.ctx.Get(), 0, 400 + 10u * k, f); }
        const FrameGenSynthWindow w = frameGenSynthOpenWindow();
        const FrameGenShadowClassStats& wc = w.cls[2];
        const double px = wc.pixels ? static_cast<double>(wc.pixels) : 1.0;
        const double mean = static_cast<double>(wc.sumErr1024) / 1024.0 / px, meanId = static_cast<double>(wc.sumErrId1024) / 1024.0 / px;
        std::snprintf(label, sizeof(label), "case 6: a world surface scrolling at the vectors' rate is predicted by the f = 1 S (mean error %.4f over %llu cells, %u scored) and not by the frame before it (no warp %.4f)", mean,
                      static_cast<unsigned long long>(wc.pixels), w.scored, meanId);
        sc.ok(w.scored >= 5 && wc.pixels > 0 && mean < 0.02 && meanId > 0.05 && meanId > 4.0 * mean, label);
        if (g_verbose && !sc.quiet) std::printf("  case 6: scrolling mean error %.4f vs no-warp %.4f, %u scored\n", mean, meanId, w.scored);
    }
    if (g_verbose) std::printf("  case 6: device after the scrolling block: 0x%08X\n", static_cast<unsigned>(g.dev->GetDeviceRemovedReason()));
    frameGenSynthConfigure(false);
    frameGenSynthFrameBoundary();
    sc.ok(!frameGenSynthResident(), "case 6: the key off again releases the resources");
}

// ---- the run ---------------------------------------------------------------------------------------------------------------------------------------------

void swapIn(Gpu& g, ComPtr<ID3D11ComputeShader> saved[kEntries], const ComPtr<ID3D11ComputeShader> mutated[kEntries]) {
    for (int i = 0; i < kEntries; ++i) { saved[i] = g.cs[i]; g.cs[i] = mutated[i]; }
}

void swapOut(Gpu& g, const ComPtr<ID3D11ComputeShader> saved[kEntries]) {
    for (int i = 0; i < kEntries; ++i) g.cs[i] = saved[i];
}

int run() {
    Gpu g;
    if (!g.init()) {
        std::printf("FAIL: WARP could not start or the production shaders did not create\n");
        return 1;
    }
    Scope sc;
    const auto alive = [&](const char* after) {
        const HRESULT hr = g.dev->GetDeviceRemovedReason();
        if (hr != S_OK) { ++g_failures; std::printf("FAIL: the device was removed (0x%08X) by %s\n", static_cast<unsigned>(hr), after); }
        return hr == S_OK;
    };
    case1Cockpit(g, sc, 1);
    case1Cockpit(g, sc, 2);
    case2World(g, sc);
    case3Far(g, sc);
    case4Split(g, sc);
    const Edge edge = case5Edge(g, sc, true);
    if (!alive("cases 1 to 5")) return 1;
    case6Module(g, sc);
    if (!alive("case 6")) return 1;
    (void)edge;

    // The mutations: each must break its case.
    struct Mutation { const char* define; const char* what; std::function<void(Scope&)> run; unsigned fails = 0; };
    Mutation muts[4] = {
        {"FG_USE_F", "f ignored (the landing uses f = 1 whatever the block says)", [&](Scope& s) { case2World(g, s); }},
        {"FG_REMOVE_HEAD", "the head turn not taken out of the vector", [&](Scope& s) { case2World(g, s); }},
        {"FG_GUARD", "the guard off", [&](Scope& s) { case5Edge(g, s, false); }},
        {"FG_FILL_FAR", "the fill from the nearer side", [&](Scope& s) { case5Edge(g, s, false); }},
    };
    for (Mutation& m : muts) {
        ComPtr<ID3D11ComputeShader> mutated[kEntries], saved[kEntries];
        ++g_checks;
        if (!compileMutated(g, m.define, mutated)) {
            ++g_failures;
            std::printf("FAIL: mutation: the shader with %s could not be built (is `#define %s 1` still in the text?)\n", m.what, m.define);
            continue;
        }
        Scope ms;
        ms.quiet = true;
        swapIn(g, saved, mutated);
        m.run(ms);
        swapOut(g, saved);
        m.fails = ms.fails;
        if (ms.fails == 0) { ++g_failures; std::printf("FAIL: mutation: %s still passes\n", m.what); }
    }
    // Case 3 reads the head's part the same way as case 2: the head-turn mutation must break it too (a second look, not a second count).
    std::printf("%s: %u checks, %u failures (mutations: f ignored broke %u checks, head turn not removed broke %u, guard off broke %u, fill from the nearer side broke %u)\n", g_failures ? "FAIL" : "PASS",
                g_checks, g_failures, muts[0].fails, muts[1].fails, muts[2].fails, muts[3].fails);
    return g_failures ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) {
    bool selfTest = false, dry = false;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--self-test")) selfTest = true;
        else if (!std::strcmp(argv[i], "--dry-run")) dry = true;
        else if (!std::strcmp(argv[i], "--verbose")) g_verbose = true;
        else { std::printf("usage: framegen_synth_test [--dry-run | --self-test] [--verbose]\n"); return 2; }
    }
    if (!selfTest && !dry) { std::printf("usage: framegen_synth_test [--dry-run | --self-test] [--verbose]\n"); return 2; }
    return run();
}
