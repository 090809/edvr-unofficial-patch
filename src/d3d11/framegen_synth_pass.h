#pragma once
// The frame-generation synthesizer's dispatch sequence and binding table (fix.frame_generation_shadow, TEMPORARY; docs/design-frame-generation-2026-10-26.md, Phase 2.1), the one
// copy the module (framegen_synth.cpp), the WARP rig (tools\framegen_synth_test) and the bench (tools\framegen_synth_bench) all run, so a pass or a binding changed in one cannot
// drift from the others. The entries, their slots and what each pass reads and writes are documented in framegen_synth_shader.h; the callers own the resources, the constant
// buffer (b0, set before the call) and the shaders (the rig runs mutated builds through the same table).
#include <d3d11.h>

#include <cstdint>

namespace edvr {

constexpr int kFrameGenSynthEntries = 7;
constexpr int kFrameGenSynthSrvSlots = 10;   // t0..t9
constexpr int kFrameGenSynthUavSlots = 8;    // u0..u7

enum FrameGenSynthEntry : int { kFgSynClear = 0, kFgSynSplat, kFgSynSrc, kFgSynResolve, kFgSynResample, kFgSynLuma, kFgSynScore };

// The HLSL entry points, in entry order (tools\temporal_shader_build compiles them to kFrameGenSynth*Bytecode in the same order).
constexpr const char* const kFrameGenSynthEntryNames[kFrameGenSynthEntries] = {"fgSynClear", "fgSynSplat", "fgSynSrc", "fgSynResolve", "fgSynResample", "fgSynLuma", "fgSynScore"};
constexpr const char* const kFrameGenSynthPassLabels[kFrameGenSynthEntries] = {"clear", "splat", "src", "resolve", "resample", "luma", "score"};

// One eye's views for a call. The inputs: B's output (the upscaler's, before the interface; C's on a score), B's depth copy, the DLSS vectors. The synth's own: the key and source
// grids and the read offsets (render size, scratch), S (output size), the kept S luma and B's luma and class (quarter grid), the stats words.
struct FrameGenSynthPassViews {
    ID3D11ShaderResourceView* out = nullptr;
    ID3D11ShaderResourceView* depth = nullptr;
    ID3D11ShaderResourceView* mv = nullptr;
    ID3D11ShaderResourceView* keySrv = nullptr;
    ID3D11ShaderResourceView* srcSrv = nullptr;
    ID3D11ShaderResourceView* readSrv = nullptr;
    ID3D11ShaderResourceView* sSrv = nullptr;
    ID3D11ShaderResourceView* sLumaSrv = nullptr;
    ID3D11ShaderResourceView* bInfoSrv = nullptr;
    ID3D11UnorderedAccessView* keyUav = nullptr;
    ID3D11UnorderedAccessView* srcUav = nullptr;
    ID3D11UnorderedAccessView* readUav = nullptr;
    ID3D11UnorderedAccessView* sUav = nullptr;
    ID3D11UnorderedAccessView* statsUav = nullptr;
    ID3D11UnorderedAccessView* sLumaUav = nullptr;
    ID3D11UnorderedAccessView* bInfoUav = nullptr;
    uint32_t rw = 0, rh = 0;   // render size
    uint32_t ow = 0, oh = 0;   // output size
    uint32_t qw = 0, qh = 0;   // the quarter grid
};

// The sequence of one call: clear; the score of the kept f = 1 S against this frame when `score`; then, when `build`, splat, source, resolve, resample and, when `keep`, the luma
// pass that keeps this f = 1 S for the next frame. `after(int entry)` runs after each dispatch (the bench's timestamps). Every dispatch starts with nothing bound from the last
// (a resource still bound as a UAV would silently null the SRV set for it), and nothing stays bound at the end.
template <class After>
inline void frameGenSynthRunPasses(ID3D11DeviceContext* ctx, ID3D11ComputeShader* const cs[kFrameGenSynthEntries], const FrameGenSynthPassViews& v, bool score, bool build, bool keep,
                                   After&& after) {
    ID3D11ShaderResourceView* const noSrv[kFrameGenSynthSrvSlots] = {};
    ID3D11UnorderedAccessView* const noUav[kFrameGenSynthUavSlots] = {};
    const auto dispatch = [&](int entry, ID3D11ShaderResourceView* const srv[kFrameGenSynthSrvSlots], ID3D11UnorderedAccessView* const uav[kFrameGenSynthUavSlots], uint32_t gx,
                              uint32_t gy) {
        ctx->CSSetShader(cs[entry], nullptr, 0);
        ctx->CSSetUnorderedAccessViews(0, kFrameGenSynthUavSlots, noUav, nullptr);
        ctx->CSSetShaderResources(0, kFrameGenSynthSrvSlots, srv);
        ctx->CSSetUnorderedAccessViews(0, kFrameGenSynthUavSlots, uav, nullptr);
        ctx->Dispatch(gx, gy, 1);
        after(entry);
    };
    const uint32_t rgx = (v.rw + 7u) / 8u, rgy = (v.rh + 7u) / 8u;
    const uint32_t ogx = (v.ow + 7u) / 8u, ogy = (v.oh + 7u) / 8u;
    const uint32_t qgx = (v.qw + 7u) / 8u, qgy = (v.qh + 7u) / 8u;
    {   // clear: the key and source grids and the stats words
        ID3D11UnorderedAccessView* uav[kFrameGenSynthUavSlots] = {nullptr, v.keyUav, v.srcUav, nullptr, nullptr, v.statsUav};
        dispatch(kFgSynClear, noSrv, uav, rgx, rgy);
    }
    if (score) {   // the f = 1 S kept from the frame before against this frame, which is C: its luma, S's and B's, by B's class
        ID3D11ShaderResourceView* srv[kFrameGenSynthSrvSlots] = {v.out, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, v.sLumaSrv, v.bInfoSrv};
        ID3D11UnorderedAccessView* uav[kFrameGenSynthUavSlots] = {nullptr, nullptr, nullptr, nullptr, nullptr, v.statsUav};
        dispatch(kFgSynScore, srv, uav, qgx, qgy);
    }
    if (build) {
        {   // splat: B's pixels carried to S, the nearer surface's key kept
            ID3D11ShaderResourceView* srv[kFrameGenSynthSrvSlots] = {nullptr, v.depth, v.mv};
            ID3D11UnorderedAccessView* uav[kFrameGenSynthUavSlots] = {nullptr, v.keyUav};
            dispatch(kFgSynSplat, srv, uav, rgx, rgy);
        }
        {   // source: each footprint pixel whose final key is the pixel's own takes the pixel's index
            ID3D11ShaderResourceView* srv[kFrameGenSynthSrvSlots] = {nullptr, v.depth, v.mv, nullptr, v.keySrv};
            ID3D11UnorderedAccessView* uav[kFrameGenSynthUavSlots] = {nullptr, nullptr, v.srcUav};
            dispatch(kFgSynSrc, srv, uav, rgx, rgy);
        }
        {   // resolve: the offset every S pixel reads at, the guard and the background pull
            ID3D11ShaderResourceView* srv[kFrameGenSynthSrvSlots] = {nullptr, v.depth, v.mv, nullptr, v.keySrv, v.srcSrv};
            ID3D11UnorderedAccessView* uav[kFrameGenSynthUavSlots] = {nullptr, nullptr, nullptr, v.readUav, nullptr, v.statsUav};
            dispatch(kFgSynResolve, srv, uav, rgx, rgy);
        }
        {   // resample B's output into S at the output size
            ID3D11ShaderResourceView* srv[kFrameGenSynthSrvSlots] = {v.out, nullptr, nullptr, nullptr, nullptr, nullptr, v.readSrv};
            ID3D11UnorderedAccessView* uav[kFrameGenSynthUavSlots] = {nullptr, nullptr, nullptr, nullptr, v.sUav};
            dispatch(kFgSynResample, srv, uav, ogx, ogy);
        }
        if (keep) {   // the f = 1 S's luma, B's luma and B's class, for the next real frame to score
            ID3D11ShaderResourceView* srv[kFrameGenSynthSrvSlots] = {v.out, v.depth, nullptr, nullptr, nullptr, nullptr, nullptr, v.sSrv};
            ID3D11UnorderedAccessView* uav[kFrameGenSynthUavSlots] = {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, v.sLumaUav, v.bInfoUav};
            dispatch(kFgSynLuma, srv, uav, qgx, qgy);
        }
    }
    ctx->CSSetShaderResources(0, kFrameGenSynthSrvSlots, noSrv);
    ctx->CSSetUnorderedAccessViews(0, kFrameGenSynthUavSlots, noUav, nullptr);
}

inline void frameGenSynthRunPasses(ID3D11DeviceContext* ctx, ID3D11ComputeShader* const cs[kFrameGenSynthEntries], const FrameGenSynthPassViews& v, bool score, bool build, bool keep) {
    frameGenSynthRunPasses(ctx, cs, v, score, build, keep, [](int) {});
}

}  // namespace edvr
