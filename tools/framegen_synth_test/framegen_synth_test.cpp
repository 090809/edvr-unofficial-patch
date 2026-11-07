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
//   7  the f = 1 S scored the way it would be seen (the compositor's turn): a far scene seen from a head turning at a changing rate, the S built for the extrapolated turn,
//      scored against the next frame raw and turned by the residual rotation (the true head against the predicted): the turned score is near zero where the raw one is
//      clearly not, and the residual's angle is the sequence's own
//   8  the read offsets' half-float storage: exact to 1/8 px up to 512 px, and the clamp's bounds
// Mutations (each replaces one of the shader's six #define switches, or the host's f, and the case it guards must fail): f ignored, the head turn not taken out of the
// vector, the guard off, the fill from the nearer side, the score's turn not applied, the score's turn applied the wrong way round.
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
#include "framegen_synth_pass.h"

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

constexpr int kEntries = kFrameGenSynthEntries;   // the entries, their names and the dispatch sequence: framegen_synth_pass.h
const char* const* const kEntryNames = kFrameGenSynthEntryNames;

struct Gpu {
    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    ComPtr<ID3D11ComputeShader> cs[kEntries];
    ComPtr<ID3D11Buffer> cb;

    bool init() {
        D3D_FEATURE_LEVEL level{};
        if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &dev, &level, &ctx))) return false;
        const struct { const unsigned char* code; size_t size; } codes[kEntries] = {
            {kFrameGenSynthSplatBytecode, sizeof(kFrameGenSynthSplatBytecode)}, {kFrameGenSynthSrcBytecode, sizeof(kFrameGenSynthSrcBytecode)},
            {kFrameGenSynthResolveBytecode, sizeof(kFrameGenSynthResolveBytecode)}, {kFrameGenSynthHolesBytecode, sizeof(kFrameGenSynthHolesBytecode)},
            {kFrameGenSynthResampleBytecode, sizeof(kFrameGenSynthResampleBytecode)},
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

    // The module's dispatch sequence (framegen_synth_pass.h) on this device's shaders (the production bytecode, or a mutated build swapped into cs).
    void run(const FrameGenSynthPassViews& v, bool score, bool build, bool keep) {
        ID3D11ComputeShader* raw[kEntries];
        for (int i = 0; i < kEntries; ++i) raw[i] = cs[i].Get();
        frameGenSynthRunPasses(ctx.Get(), raw, v, score, build, keep);
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

// What a built f = 1 S leaves for the next frame's score: the quarter-grid luma of S, and B's luma and class.
struct Keep {
    Tex sLuma, bInfo;
};

// The module's order of dispatches for one eye-frame, up to and including the resample (and, with `keep`, the luma that is kept for the score). The read offsets are the
// module's: R16G16_FLOAT.
Run runSynth(Gpu& g, const Scene& sc, const FrameGenSynthCb& cb, Keep* keep = nullptr) {
    Run r;
    Tex out, depth, mv, key, src, read, s;
    if (!makeInputs(g, sc, out, depth, mv)) return r;
    if (!g.makeTex(sc.w, sc.h, DXGI_FORMAT_R32_UINT, nullptr, 0, key, true) || !g.makeTex(sc.w, sc.h, DXGI_FORMAT_R32_UINT, nullptr, 0, src, true) ||
        !g.makeTex(sc.w, sc.h, DXGI_FORMAT_R16G16_FLOAT, nullptr, 0, read, true) || !g.makeTex(sc.ow, sc.oh, DXGI_FORMAT_R32G32B32A32_FLOAT, nullptr, 0, s, true))
        return r;
    if (keep && (!g.makeTex((sc.w + 3) / 4, (sc.h + 3) / 4, DXGI_FORMAT_R32_FLOAT, nullptr, 0, keep->sLuma, true) ||
                 !g.makeTex((sc.w + 3) / 4, (sc.h + 3) / 4, DXGI_FORMAT_R32G32_FLOAT, nullptr, 0, keep->bInfo, true)))
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
    ComPtr<ID3D11Buffer> holes;   // the hole list (the module's ensureScratch)
    ComPtr<ID3D11UnorderedAccessView> holesUav;
    bd = D3D11_BUFFER_DESC{};
    bd.ByteWidth = frameGenSynthHoleCapacity(static_cast<uint32_t>(sc.w), static_cast<uint32_t>(sc.h)) * 4; bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED; bd.StructureByteStride = 4;
    ud.Buffer.NumElements = frameGenSynthHoleCapacity(static_cast<uint32_t>(sc.w), static_cast<uint32_t>(sc.h));
    if (FAILED(g.dev->CreateBuffer(&bd, nullptr, &holes)) || FAILED(g.dev->CreateUnorderedAccessView(holes.Get(), &ud, &holesUav))) return r;

    g.setCb(cb);
    FrameGenSynthPassViews v;
    v.out = out.srv.Get(); v.depth = depth.srv.Get(); v.mv = mv.srv.Get();
    v.keySrv = key.srv.Get(); v.keyUav = key.uav.Get();
    v.srcSrv = src.srv.Get(); v.srcUav = src.uav.Get();
    v.readSrv = read.srv.Get(); v.readUav = read.uav.Get();
    v.holesUav = holesUav.Get();
    v.sSrv = s.srv.Get(); v.sUav = s.uav.Get();
    if (keep) { v.sLumaUav = keep->sLuma.uav.Get(); v.bInfoUav = keep->bInfo.uav.Get(); }
    v.statsUav = statsUav.Get();
    v.rw = static_cast<uint32_t>(sc.w); v.rh = static_cast<uint32_t>(sc.h); v.ow = static_cast<uint32_t>(sc.ow); v.oh = static_cast<uint32_t>(sc.oh);
    v.qw = static_cast<uint32_t>((sc.w + 3) / 4); v.qh = static_cast<uint32_t>((sc.h + 3) / 4);
    g.run(v, false, true, keep != nullptr);
    g.ctx->CopyResource(staging.Get(), stats.Get());
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(g.ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m))) return r;
    r.stats.assign(static_cast<const uint32_t*>(m.pData), static_cast<const uint32_t*>(m.pData) + kFrameGenSynthStatsUints);
    g.ctx->Unmap(staging.Get(), 0);
    r.s = g.readTex<float>(s.t.Get(), sc.ow * 4, sc.oh);
    r.ok = true;
    return r;
}

// The score of frame C against the S kept from B (the module's order: clear the stats, then the score entry on C's output), decoded.
bool runScore(Gpu& g, const Scene& scC, const FrameGenSynthCb& cbC, const Keep& keep, FrameGenSynthEyeResult& res) {
    Tex out, depth, mv, key, src;
    if (!makeInputs(g, scC, out, depth, mv)) return false;
    if (!g.makeTex(scC.w, scC.h, DXGI_FORMAT_R32_UINT, nullptr, 0, key, true) || !g.makeTex(scC.w, scC.h, DXGI_FORMAT_R32_UINT, nullptr, 0, src, true)) return false;
    ComPtr<ID3D11Buffer> stats, staging;
    ComPtr<ID3D11UnorderedAccessView> statsUav;
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = kFrameGenSynthStatsUints * 4; bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED; bd.StructureByteStride = 4;
    if (FAILED(g.dev->CreateBuffer(&bd, nullptr, &stats))) return false;
    D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
    ud.Format = DXGI_FORMAT_UNKNOWN; ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER; ud.Buffer.NumElements = kFrameGenSynthStatsUints;
    if (FAILED(g.dev->CreateUnorderedAccessView(stats.Get(), &ud, &statsUav))) return false;
    bd.Usage = D3D11_USAGE_STAGING; bd.BindFlags = 0; bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(g.dev->CreateBuffer(&bd, nullptr, &staging))) return false;
    g.setCb(cbC);
    FrameGenSynthPassViews v;
    v.out = out.srv.Get(); v.depth = depth.srv.Get(); v.mv = mv.srv.Get();
    v.keyUav = key.uav.Get(); v.srcUav = src.uav.Get();
    v.sLumaSrv = keep.sLuma.srv.Get(); v.bInfoSrv = keep.bInfo.srv.Get();
    v.statsUav = statsUav.Get();
    v.rw = static_cast<uint32_t>(scC.w); v.rh = static_cast<uint32_t>(scC.h);
    v.qw = static_cast<uint32_t>((scC.w + 3) / 4); v.qh = static_cast<uint32_t>((scC.h + 3) / 4);
    g.run(v, true, false, false);
    g.ctx->CopyResource(staging.Get(), stats.Get());
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(g.ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m))) return false;
    frameGenSynthDecodeStats(static_cast<const uint32_t*>(m.pData), res);
    g.ctx->Unmap(staging.Get(), 0);
    return true;
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
        // The head did not move, so S was built for the head C has: the residual is nothing, the turned score is the raw one and every cell is inside S.
        std::snprintf(label, sizeof(label), "case 6: a still head has no residual rotation (%u samples, median %.4f, max %.4f deg) and the turned score is the raw one over the same cells (%llu of %llu, %llu outside)",
                      w.rotSamples, static_cast<double>(w.rotMedian), static_cast<double>(w.rotMax), static_cast<unsigned long long>(w.tcells), static_cast<unsigned long long>(w.cells),
                      static_cast<unsigned long long>(w.tout));
        sc.ok(w.rotSamples >= w.scored && w.rotMax < 0.01f && w.tcells == w.cells && w.tout == 0 && w.clamped == 0, label);
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

// ---- case 7: the f = 1 S turned the way the compositor would turn it -------------------------------------------------------------------------------------------

void mul3(const float a[9], const float b[9], float out[9]) {
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) out[r * 3 + c] = a[r * 3] * b[c] + a[r * 3 + 1] * b[3 + c] + a[r * 3 + 2] * b[6 + c];
}

struct TurnedScore { bool ran = false; double rawMean = 0, turnedMean = 0; uint64_t cells = 0, tcells = 0, tout = 0; float resMed = 0, resMax = 0; float wantMed = 0, wantMax = 0; };

// A far scene seen from a head that yaws by a_k degrees in frame k (the eye space of frame k is Q_k = rotY(a_k) of a fixed world): the picture is a function of the world
// direction, the vectors are the head's own (B to A, the ship still). The step a_k - a_(k-1) changes from frame to frame, so the head's extrapolation misses; the f = 1 S built
// at B for the head's constant-velocity step, scored at C raw and turned by the residual rotation.
TurnedScore case7Turned(Gpu& g) {
    TurnedScore t;
    const float angles[12] = {0.0f, 2.0f, 5.0f, 6.0f, 10.0f, 12.0f, 13.0f, 18.0f, 20.0f, 22.0f, 27.0f, 28.0f};   // steps 2 3 1 4 2 1 5 2 2 5 1
    float Q[12][9];
    for (int k = 0; k < 12; ++k) rotY(angles[k], Q[k]);
    const auto frameAt = [&](int k) {
        Scene sc;
        sc.grey([&](int x, int y) {
            float d[3], w[3];
            temporalPixelToDir(static_cast<float>(x), static_cast<float>(y), kTan, RW, RH, d);
            float qt[9];
            temporalTranspose3(Q[k], qt);
            temporalApply3(qt, d, w);
            return 0.5f + 0.35f * std::sin(7.0f * std::atan2(w[0], -w[2]));
        });
        return sc;
    };
    const auto hdOf = [&](int k, float hd[9]) {   // x_(k-1) = Hd x_k
        float qt[9];
        temporalTranspose3(Q[k], qt);
        mul3(Q[k - 1], qt, hd);
    };
    std::vector<float> angs;
    for (int k = 2; k < 12; k += 1) {
        // B = frame k - 1, C = frame k. The vectors of B are the head's own: its backward delta is hd(B).
        Scene B = frameAt(k - 1), C = frameAt(k);
        FrameGenDeltas dB, dC;
        hdOf(k - 1, dB.Hd);
        hdOf(k, dC.Hd);
        for (int y = 0; y < RH; ++y)
            for (int x = 0; x < RW; ++x) {
                float d[3], dh[3], pp[2];
                temporalPixelToDir(static_cast<float>(x), static_cast<float>(y), kTan, RW, RH, d);
                temporalApply3(dB.Hd, d, dh);
                if (!temporalDirToPixel(dh, kTan, RW, RH, &pp[0], &pp[1])) continue;
                B.mv[(static_cast<size_t>(y) * RW + x) * 2] = pp[0] - static_cast<float>(x);
                B.mv[(static_cast<size_t>(y) * RW + x) * 2 + 1] = pp[1] - static_cast<float>(y);
            }
        FrameGenSynthCb cbB;
        frameGenSynthFillCb(cbB, RW, RH, RW, RH, kTan, 0.0f, kDepthB, 10.0f, 1.0f, dB);
        Keep keep;
        const Run rb = runSynth(g, B, cbB, &keep);
        if (!rb.ok) return t;
        float kept[9], resid[9];
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c) kept[r * 3 + c] = cbB.rs[r][c];
        frameGenSynthResidual(kept, dC.Hd, resid);
        angs.push_back(temporalRotationAngleDeg(resid));
        FrameGenSynthCb cbC;
        frameGenSynthFillCb(cbC, RW, RH, RW, RH, kTan, 0.0f, kDepthB, 10.0f, 1.0f, dC, resid, kTan);
        FrameGenSynthEyeResult res;
        if (!runScore(g, C, cbC, keep, res)) return t;
        const FrameGenShadowClassStats& cls = res.cls[0];
        const FrameGenShadowClassStats& tc = res.tcls[0];
        t.rawMean += static_cast<double>(cls.sumErr1024) / 1024.0;
        t.turnedMean += static_cast<double>(tc.sumErr1024) / 1024.0;
        t.cells += cls.pixels;
        t.tcells += tc.pixels;
        t.tout += res.tout;
    }
    t.rawMean = t.cells ? t.rawMean / static_cast<double>(t.cells) : 0.0;
    t.turnedMean = t.tcells ? t.turnedMean / static_cast<double>(t.tcells) : 0.0;
    std::sort(angs.begin(), angs.end());
    t.wantMed = angs[angs.size() / 2];
    t.wantMax = angs.back();
    t.resMed = t.wantMed;
    t.resMax = t.wantMax;
    t.ran = true;
    return t;
}

void case7Check(Gpu& g, Scope& sc) {
    char label[300];
    const TurnedScore t = case7Turned(g);
    sc.ok(t.ran, "case 7: the turned score ran");
    if (!t.ran) return;
    std::snprintf(label, sizeof(label), "case 7: the head's extrapolation misses (raw mean error %.4f over %llu cells) and the turn by the residual rotation puts it right (turned mean %.4f over %llu cells, %llu outside S)",
                  t.rawMean, static_cast<unsigned long long>(t.cells), t.turnedMean, static_cast<unsigned long long>(t.tcells), static_cast<unsigned long long>(t.tout));
    sc.ok(t.cells > 5000 && t.rawMean > 0.03 && t.turnedMean < 0.012 && t.turnedMean * 4.0 < t.rawMean, label);
    if (g_verbose && !sc.quiet) std::printf("  case 7: raw %.4f, turned %.4f, %llu cells, %llu turned, %llu outside, the residual median %.3f max %.3f deg\n", t.rawMean, t.turnedMean,
                                            static_cast<unsigned long long>(t.cells), static_cast<unsigned long long>(t.tcells), static_cast<unsigned long long>(t.tout), t.wantMed, t.wantMax);
}

float halfToFloat(uint16_t h) {
    const int e = (h >> 10) & 31;
    const float m = static_cast<float>(h & 1023);
    const float v = e == 0 ? std::ldexp(m, -24) : std::ldexp(1024.0f + m, e - 25);
    return (h & 0x8000u) ? -v : v;
}

// Case 8: the offsets' storage. floatToHalf rounds to nearest, as the GPU's store does.
void case8Offsets(Scope& sc) {
    char label[260];
    float worst512 = 0.0f, worst256 = 0.0f, worst2000 = 0.0f;
    for (int i = -20000; i <= 20000; ++i) {
        const float v = static_cast<float>(i) * 0.1f * (1.0f + 0.000137f * static_cast<float>(i % 7));   // up to +-2000, off the grid of halves
        const float e = std::fabs(halfToFloat(floatToHalf(v)) - v);
        if (std::fabs(v) <= 256.0f) worst256 = std::max(worst256, e);
        if (std::fabs(v) <= 512.0f) worst512 = std::max(worst512, e);
        if (std::fabs(v) <= kFrameGenSynthMaxOffset) worst2000 = std::max(worst2000, e);
    }
    std::snprintf(label, sizeof(label), "case 8: a read offset stored in half floats is exact to %.4f px up to 256 px, %.4f px up to 512 px and %.4f px up to the clamp (%.0f px)", worst256, worst512, worst2000,
                  static_cast<double>(kFrameGenSynthMaxOffset));
    sc.ok(worst256 <= 0.0625f && worst512 <= 0.125f && worst2000 <= 0.5f && std::fabs(halfToFloat(floatToHalf(kFrameGenSynthNoRead)) - kFrameGenSynthNoRead) == 0.0f &&
          kFrameGenSynthNoRead < -kFrameGenSynthMaxOffset * 1.5f, label);
}

// ---- case 9: the replay capture's writer (.agents/framegen-replay-format.md) ----------------------------------------------------------------------------------

std::string readWholeFile(const std::wstring& path) {
    std::string out;
    FILE* fp = nullptr;
    if (_wfopen_s(&fp, path.c_str(), L"rb") || !fp) return out;
    char buf[65536];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), fp)) > 0) out.append(buf, n);
    std::fclose(fp);
    return out;
}

bool pumpUntilIdle() {
    for (int i = 0; i < 4000 && !frameGenReplayIdle(); ++i) { frameGenReplayPump(); Sleep(5); }
    return frameGenReplayIdle();
}

std::wstring widen(const char* s) { return std::wstring(s, s + std::strlen(s)); }

void case9Replay(Gpu& g, Scope& sc) {
    char label[260];
    {   // the hash is a real SHA-256
        Sha256 a; a.update("abc", 3);
        sc.ok(a.hex() == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "case 9: SHA-256 of \"abc\" is the published value");
        Sha256 e;
        sc.ok(e.hex() == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", "case 9: SHA-256 of nothing is the published value");
        Sha256 m; const std::string block(1000, 'a'); for (int i = 0; i < 1000; ++i) m.update(block.data(), block.size());
        sc.ok(m.hex() == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0", "case 9: SHA-256 of a million a's (fed in pieces) is the published value");
    }
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    const std::wstring root = std::wstring(tmp) + L"edvr_replay_test_" + std::to_wstring(GetCurrentProcessId());
    CreateDirectoryW(root.c_str(), nullptr);
    Scene s;
    s.gradient();
    for (size_t i = 0; i < s.depth.size(); ++i) s.depth[i] = 0.001f * static_cast<float>(i % 97);
    for (size_t i = 0; i < s.mv.size(); ++i) s.mv[i] = 0.25f * static_cast<float>(i % 13);
    ModuleFrame m;
    if (!makeInputs(g, s, m.out, m.depth, m.mv)) { sc.ok(false, "case 9: inputs were made"); return; }
    FrameGenSynthFrame f;
    f.renderW = RW; f.renderH = RH; f.outW = RW; f.outH = RH;
    f.tanNow = kTan;
    f.depthA = 0.0f; f.depthB = kDepthB; f.splitMetres = 10.0f;
    f.depthWritten = true;
    f.outSrv = m.out.srv.Get(); f.depthSrv = m.depth.srv.Get(); f.mvSrv = m.mv.srv.Get();
    f.wantShown = true;
    f.worldValid = true;
    f.deltas = Deltas(1.0f, 0.5f, 0.25f, -0.125f).d;
    f.deltas.W[1] = 0.5f; f.deltas.tv[2] = 3.0f;
    const wchar_t* stamp = L"20260101-000000";
    uint32_t frame = 9000;

    // The trigger off (replay never set): nothing is captured, nothing is written, not even the folder.
    frameGenSynthConfigure(true);
    for (uint32_t k = 0; k < 8; ++k) frameGenSynthEyeFrame(g.dev.Get(), g.ctx.Get(), 0, frame++, f);
    sc.ok(!frameGenReplayFinish(root.c_str(), stamp) && frameGenReplayIdle() && GetFileAttributesW((root + L"\\fgreplay_" + stamp).c_str()) == INVALID_FILE_ATTRIBUTES,
          "case 9: without the dump trigger nothing is captured and nothing is written");

    // A full run of six frames (a seventh asked for is ignored).
    f.replay = true;
    for (uint32_t k = 0; k < 7; ++k) { f.replayIndex = k; f.deltas.Hd[0] = 1.0f - 0.001f * k; frameGenSynthEyeFrame(g.dev.Get(), g.ctx.Get(), 0, frame++, f); }
    sc.ok(frameGenReplayCaptured() == 6, "case 9: six consecutive frames were captured (not seven)");
    sc.ok(frameGenReplayFinish(root.c_str(), stamp), "case 9: the capture was handed to the writer");
    const std::wstring dir = root + L"\\fgreplay_" + stamp;
    sc.ok(GetFileAttributesW((dir + L"\\f0_out.bin").c_str()) == INVALID_FILE_ATTRIBUTES, "case 9: nothing is written on the frame it was handed over (the writer is the pump's)");
    sc.ok(pumpUntilIdle(), "case 9: the pump wrote everything and went idle");
    const std::string manifest = readWholeFile(dir + L"\\manifest.json");
    sc.ok(manifest.find("\"frames\": 6") != std::string::npos && manifest.find("\"stopped_reason\": null") != std::string::npos && manifest.find("\"eye\": \"left\"") != std::string::npos &&
              manifest.find("\"stamp\": \"20260101-000000\"") != std::string::npos && manifest.find("\"build\": \"") != std::string::npos,
          "case 9: the manifest names the stamp, the build, the eye, six frames and no early stop");
    int goodFiles = 0, files = 0;
    const size_t want[3] = {static_cast<size_t>(RW) * RH * 16, static_cast<size_t>(RW) * RH * 4, static_cast<size_t>(RW) * RH * 4};
    const char* part[3] = {"out", "depth", "mv"};
    std::vector<uint16_t> mvHalf(s.mv.size());
    for (size_t i = 0; i < s.mv.size(); ++i) mvHalf[i] = floatToHalf(s.mv[i]);
    const void* ref[3] = {s.rgba.data(), s.depth.data(), mvHalf.data()};
    for (uint32_t k = 0; k < 6; ++k)
        for (int i = 0; i < 3; ++i) {
            char name[40];
            std::snprintf(name, sizeof(name), "f%u_%s.bin", k, part[i]);
            ++files;
            const std::string body = readWholeFile(dir + L"\\" + widen(name));
            Sha256 h; h.update(body.data(), body.size());
            char probe[200];
            std::snprintf(probe, sizeof(probe), "{\"name\": \"%s\", \"bytes\": %zu, \"sha256\": \"%s\"}", name, want[i], h.hex().c_str());
            if (body.size() == want[i] && std::memcmp(body.data(), ref[i], want[i]) == 0 && manifest.find(probe) != std::string::npos) ++goodFiles;
        }
    std::snprintf(label, sizeof(label), "case 9: all 18 bins have the right byte size, the exact texels and a manifest entry with their SHA-256 (%d of %d)", goodFiles, files);
    sc.ok(goodFiles == 18, label);
    const std::string j3 = readWholeFile(dir + L"\\f3.json");
    if (g_verbose) std::printf("%s", j3.c_str());
    sc.ok(j3.find("\"render_w\": 128") != std::string::npos && j3.find("\"out_format_name\": \"R32G32B32A32_FLOAT\"") != std::string::npos && j3.find("\"depth_format_name\": \"R32_FLOAT\"") != std::string::npos &&
              j3.find("\"mv_format_name\": \"R16G16_FLOAT\"") != std::string::npos && j3.find("\"variant\": 0") != std::string::npos && j3.find("\"world_valid\": true") != std::string::npos &&
              j3.find("\"tan_prev\": null") != std::string::npos && j3.find("\"hd\": [0.99699") != std::string::npos && j3.find("\"tv_cam\": [0, 0, 3]") != std::string::npos && j3.find("\"frame\": 9011") != std::string::npos &&
              j3.find("\"qpc_freq\"") != std::string::npos && j3.find("\"world_w\": [1, 0.5, 0,") != std::string::npos && j3.find("\"split_metres\": 10") != std::string::npos,
          "case 9: f3.json carries the frame's sizes, formats, variant, head and world deltas, pass frame and clock");

    // A broken run (frame 2 never came): the capture stops at 2 frames and the manifest says why.
    const std::wstring dir2 = root + L"\\fgreplay_20260101-000001";
    for (uint32_t k : {0u, 1u, 3u, 4u}) { f.replayIndex = k; frameGenSynthEyeFrame(g.dev.Get(), g.ctx.Get(), 0, frame++, f); }
    sc.ok(frameGenReplayFinish(root.c_str(), L"20260101-000001") && pumpUntilIdle(), "case 9: a run with a missing frame was written");
    const std::string manifest2 = readWholeFile(dir2 + L"\\manifest.json");
    sc.ok(manifest2.find("\"frames\": 2") != std::string::npos && manifest2.find("was not captured") != std::string::npos && GetFileAttributesW((dir2 + L"\\f2_out.bin").c_str()) == INVALID_FILE_ATTRIBUTES,
          "case 9: the broken run stops at the missing frame and the manifest says why");

    // The key off: a run in progress is dropped at the next boundary and nothing more is captured.
    f.replayIndex = 0; frameGenSynthEyeFrame(g.dev.Get(), g.ctx.Get(), 0, frame++, f);
    frameGenSynthConfigure(false);
    frameGenSynthFrameBoundary();
    f.replayIndex = 1; frameGenSynthEyeFrame(g.dev.Get(), g.ctx.Get(), 0, frame++, f);
    sc.ok(frameGenReplayCaptured() == 0 && !frameGenReplayFinish(root.c_str(), L"20260101-000002") && frameGenReplayIdle(), "case 9: with the key off nothing is captured and a run in progress is dropped");

    for (const std::wstring& d : {dir, dir2}) {   // clean up
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((d + L"\\*").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do { if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) DeleteFileW((d + L"\\" + fd.cFileName).c_str()); } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
        RemoveDirectoryW(d.c_str());
    }
    RemoveDirectoryW(root.c_str());
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
    case7Check(g, sc);
    case8Offsets(sc);
    case9Replay(g, sc);
    if (!alive("cases 7 to 9")) return 1;
    (void)edge;

    // The mutations: each must break its case.
    struct Mutation { const char* define; const char* what; std::function<void(Scope&)> run; unsigned fails = 0; };
    Mutation muts[6] = {
        {"FG_USE_F", "f ignored (the landing uses f = 1 whatever the block says)", [&](Scope& s) { case2World(g, s); }},
        {"FG_REMOVE_HEAD", "the head turn not taken out of the vector", [&](Scope& s) { case2World(g, s); }},
        {"FG_GUARD", "the guard off", [&](Scope& s) { case5Edge(g, s, false); }},
        {"FG_FILL_FAR", "the fill from the nearer side", [&](Scope& s) { case5Edge(g, s, false); }},
        {"FG_TURN", "the score's turn not applied (the residual ignored)", [&](Scope& s) { case7Check(g, s); }},
        {"FG_TURN_SIGN", "the score's turn applied the wrong way round (the residual transposed)", [&](Scope& s) { case7Check(g, s); }},
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
    std::printf("%s: %u checks, %u failures (mutations: f ignored broke %u checks, head turn not removed broke %u, guard off broke %u, fill from the nearer side broke %u, turn not applied broke %u, turn the wrong way round broke %u)\n",
                g_failures ? "FAIL" : "PASS", g_checks, g_failures, muts[0].fails, muts[1].fails, muts[2].fails, muts[3].fails, muts[4].fails, muts[5].fails);
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
