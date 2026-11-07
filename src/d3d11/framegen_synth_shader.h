#pragma once
// The frame-generation synthesizer's compute shaders (fix.frame_generation_shadow, TEMPORARY; docs/design-frame-generation-2026-10-26.md, Phase 2.1, "The synthesis pass").
// One source, six entry points, one constant block (src\common\framegen_synth_math.h FrameGenSynthCb). Nothing they write is shown in the headset. Before them the key grid and the
// stats words are cleared to 0 by the API (ClearUnorderedAccessViewUint, framegen_synth_pass.h); the source grid is not cleared: a pixel whose key is 0 never has its source
// read, and a pixel whose key is not 0 has it written by fgSynSrc (the pixel that set the key finds it there again and writes).
//   fgSynSplat     each B pixel carried to S by its class's motion (fgSynTarget; its class and depth are the nearest of the 3x3 around it, synDilated, taken where they are read) and
//                  scattered as a key into the four pixels of its bilinear footprint (InterlockedMax: the nearer surface wins; a pixel that is its own nearest beats one that borrowed
//                  the dilated depth);
//   fgSynSrc       the second pass over the same keys: each pixel writes its index (plus one) to every footprint pixel whose final key is its own;
//   fgSynResolve   for every S pixel the B position to read, stored as an OFFSET from the S pixel's own centre in two half floats (kMaxOffset, kNoRead): the winner's own position moved
//                  by the landing offset (the guard reads the winner's pixel itself when it has a nearer 3x3 neighbour on the side the offset points to), or, for a pixel with no
//                  winner, the farthest key found within 8 px read 2 px further into its own surface (the background pull); the counters (filled, left, guard, clamped) are added to the
//                  stats words;
//   fgSynResample  output size: each output pixel to its S pixel, its read offset, B's output there, bilinear, into the S texture (dlOut's format);
//   fgSynLuma      quarter grid: S's luma the way the scorer's capture takes it, B's luma and B's class (the f = 1 S is kept for the next real frame);
//   fgSynScore     quarter grid, on the next real frame C: C's luma against the kept S luma and against B's (the no-warp comparison), binned per class of B as the scorer bins; and again
//                  against the kept S luma TURNED by the residual head rotation (the compositor's step: C's cell direction rotated into the eye S was built for, projected with the
//                  tangents S was built with, a bilinear read of the quarter luma; outside S: not scored, counted).
// The pixel class and the carry are the temporal pass's own text: the source the build compiles is the text between the two EDVR_TEMPORAL_CARRY markers of kTemporalCsHlsl, then
// kFrameGenSynthCs (frameGenSynthShaderSource); the markers are read out of kTemporalCsHlsl by frameGenCarryText (framegen_shadow_shader.h).
//
// Bindings: t0 B's output (the upscaler's, before the interface; on C's frame the score reads C's there), t1 B's depth copy, t2 the DLSS vectors (R16G16_FLOAT, render size, pixels,
// this frame to the previous, jitter-free), t3 unused, t4 the key grid, t5 the source grid, t6 the read offsets, t7 the S texture, t8 the kept S luma, t9 the kept B luma (x) and class
// (y); u1 the key grid (R32_UINT), u2 the source grid (R32_UINT), u3 the read offsets (R16G16_FLOAT), u4 the S texture, u5 the stats words, u6 the S luma, u7 B's luma and class; b0 P.
//
// The key: 0 a hole, 4 the far plane, else (max(asuint(1 / metres), 8) & ~7) | 4 when the pixel is its own nearest | class (1 cockpit, 2 world). A larger key is a nearer surface.
//
// The six #defines at the top of kFrameGenSynthCs are the rig's mutation points (tools\framegen_synth_test replaces one with 0 and the case it guards must fail); production
// compiles them all 1.
#include <cstring>
#include <string>

#include "framegen_shadow_shader.h"

namespace edvr {

inline constexpr char kFrameGenSynthCs[] = R"HLSL(
#define FG_USE_F 1
#define FG_REMOVE_HEAD 1
#define FG_GUARD 1
#define FG_FILL_FAR 1
#define FG_TURN 1        // the score turns S by the residual head rotation (0: it scores it unturned)
#define FG_TURN_SIGN 1   // the residual is applied the right way round (0: its transpose)
cbuffer P : register(b0) {
    int4   sz;       // render width, height, output width, height
    int4   qz;       // quarter grid width, height
    float4 tanB;     // this frame's tangents
    float4 dk;       // depth A, depth B, split metres, f
    float4 rs[3];    // the head's step B to S: rotation rows, w the translation component
    float4 hp[3];    // the head's rotation B to A (Hd_B) rows
    float4 rr[3];    // the score's residual rotation: C's eye space to the eye space the kept S was built for
    float4 tanK;     // the tangents the kept S was built with
};
Texture2D<float4> Out     : register(t0);
Texture2D<float>  Depth   : register(t1);
Texture2D<float2> Mv      : register(t2);
Texture2D<uint>   KeyT    : register(t4);
Texture2D<uint>   SrcT    : register(t5);
Texture2D<float2> ReadT   : register(t6);   // the read offsets
Texture2D<float4> SIn     : register(t7);
Texture2D<float>  SLumaIn : register(t8);
Texture2D<float2> BInfoIn : register(t9);
RWTexture2D<uint>   Key       : register(u1);
RWTexture2D<uint>   Src       : register(u2);
RWTexture2D<float2> ReadOut   : register(u3);   // R16G16_FLOAT
RWTexture2D<float4> SOut      : register(u4);
RWStructuredBuffer<uint> Stats : register(u5);
RWTexture2D<float>  SLumaOut  : register(u6);
RWTexture2D<float2> BInfoOut  : register(u7);

static const uint  kBins = 48;
static const uint  kPerClass = 99;        // 2 * kBins + 3
static const uint  kHoles = 297;          // 3 * kPerClass
static const uint  kBlockUints = 299;     // the score block: 3 classes, holes, cells
static const uint  kCountBase = 299;      // filled, left, guard, clamped
static const uint  kTurnBase = 303;       // the turned score block
static const uint  kStatsUints = 602;     // src\common\framegen_synth_math.h kFrameGenSynthStatsUints
static const float kGuardMargin = 0.9;    // a 3x3 neighbour nearer by more than a tenth of the pixel's own distance
static const int   kFillRadius = 8;
static const float kFillShift = 2.0;
static const float kNoRead = -4096.0;       // framegen_synth_math.h kFrameGenSynthNoRead
static const float kMaxOffset = 2000.0;    // kFrameGenSynthMaxOffset

float synLuma(float3 c) { return clamp(dot(c, float3(0.2126, 0.7152, 0.0722)), 0.0, 1.0); }

// A bilinear read of a render-or-output size float4 texture at a continuous position (texel centres at +0.5), edges clamped.
float4 synBilinear(Texture2D<float4> t, float2 u, int2 size) {
    float2 f = u - 0.5;
    int2 i0 = int2(floor(f));
    float2 w = f - float2(i0);
    int2 hi = size - 1;
    int2 a = clamp(i0, int2(0, 0), hi);
    int2 b = clamp(i0 + 1, int2(0, 0), hi);
    float4 c00 = t.Load(int3(a.x, a.y, 0));
    float4 c10 = t.Load(int3(b.x, a.y, 0));
    float4 c01 = t.Load(int3(a.x, b.y, 0));
    float4 c11 = t.Load(int3(b.x, b.y, 0));
    return lerp(lerp(c00, c10, w.x), lerp(c01, c11, w.x), w.y);
}

uint synBin(float e) {
    if (!(e >= 1.0 / 1024.0)) return 0u;
    float l = 4.0 * log2(e * 1024.0);
    if (!(l < 46.0)) return kBins - 1;
    return uint(clamp(1 + int(floor(l)), 1, int(kBins) - 1));
}

// The class of a depth: 0 far, 1 cockpit (the head's), 2 world; metres out (0 for far).
uint synClass(float zr, out float z) {
    bool far;
    z = temporalDepthMetres(zr, dk.x, dk.y, far);
    return far ? 0u : (temporalWorldPath(true, far, z, dk.z, false) ? 2u : 1u);
}

// The nearest of the 3x3 around a pixel (the larger reversed-Z value): the class and depth an edge pixel takes are the near object's.
float synDilated(int2 q) {
    float z = 0.0;
    [unroll] for (int dy = -1; dy <= 1; ++dy) {
        [unroll] for (int dx = -1; dx <= 1; ++dx) {
            int2 n = clamp(q + int2(dx, dy), int2(0, 0), int2(sz.x, sz.y) - 1);
            z = max(z, Depth.Load(int3(n.x, n.y, 0)));
        }
    }
    return z;
}

float synRawMetres(int2 p) {
    p = clamp(p, int2(0, 0), int2(sz.x, sz.y) - 1);
    bool far;
    float z = temporalDepthMetres(Depth.Load(int3(p.x, p.y, 0)), dk.x, dk.y, far);
    return far ? 1.0e9 : z;
}

// Where B's pixel id lands in S, and its key. The class and depth are the dilated depth's. false: behind the eye. The motion is src\common\framegen_synth_math.h
// frameGenSynthTarget's: cockpit the head's rigid step with the depth; far and world the ex2dh rule on B's vector, rotation only.
bool fgSynTarget(uint2 id, out float2 pp, out uint key) {
    pp = float2(0.0, 0.0);
    key = 0u;
    float zr = synDilated(int2(id));
    float z;
    uint cls = synClass(zr, z);
    float2 pB = float2(id);
    float f = FG_USE_F ? dk.w : 1.0;
    float3 dB = temporalPixelDir(pB, tanB, int2(sz.x, sz.y));
    float zKey = z;
    if (cls == 1u) {
        float3 t = float3(rs[0].w, rs[1].w, rs[2].w);
        float3 dp = temporalCarry(dB, z, false, rs[0].xyz, rs[1].xyz, rs[2].xyz, t);
        if (!temporalProjectPixel(dp, tanB, int2(sz.x, sz.y), pp)) return false;
        zKey = -dp.z;
    } else {
        float2 mv = Mv.Load(int3(id.x, id.y, 0));
#if FG_REMOVE_HEAD
        float3 dH = float3(dot(hp[0].xyz, dB), dot(hp[1].xyz, dB), dot(hp[2].xyz, dB));
        float2 pHeadPrev;
        if (!temporalProjectPixel(dH, tanB, int2(sz.x, sz.y), pHeadPrev)) return false;
#else
        float2 pHeadPrev = pB;
#endif
        float2 m = (pB + mv) - pHeadPrev;
        float3 dS = temporalPixelDir(pB - f * m, tanB, int2(sz.x, sz.y));
        float3 dR = float3(dot(rs[0].xyz, dS), dot(rs[1].xyz, dS), dot(rs[2].xyz, dS));
        if (!temporalProjectPixel(dR, tanB, int2(sz.x, sz.y), pp)) return false;
    }
    key = 4u;
    if (cls != 0u) {
        uint own = (Depth.Load(int3(id.x, id.y, 0)) == zr) ? 4u : 0u;
        key = (max(asuint(1.0 / max(zKey, 1e-6)), 8u) & ~7u) | own | cls;
    }
    return true;
}

[numthreads(8, 8, 1)]
void fgSynSplat(uint3 id : SV_DispatchThreadID) {
    if (id.x >= uint(sz.x) || id.y >= uint(sz.y)) return;
    float2 pp;
    uint key;
    if (!fgSynTarget(id.xy, pp, key)) return;
    int2 base = int2(floor(pp));
    [unroll] for (int dy = 0; dy < 2; ++dy) {
        [unroll] for (int dx = 0; dx < 2; ++dx) {
            int2 c = base + int2(dx, dy);
            if (c.x >= 0 && c.y >= 0 && c.x < sz.x && c.y < sz.y) InterlockedMax(Key[uint2(c)], key);
        }
    }
}

[numthreads(8, 8, 1)]
void fgSynSrc(uint3 id : SV_DispatchThreadID) {
    if (id.x >= uint(sz.x) || id.y >= uint(sz.y)) return;
    float2 pp;
    uint key;
    if (!fgSynTarget(id.xy, pp, key)) return;
    int2 base = int2(floor(pp));
    [unroll] for (int dy = 0; dy < 2; ++dy) {
        [unroll] for (int dx = 0; dx < 2; ++dx) {
            int2 c = base + int2(dx, dy);
            if (c.x >= 0 && c.y >= 0 && c.x < sz.x && c.y < sz.y && KeyT.Load(int3(c.x, c.y, 0)) == key) Src[uint2(c)] = id.y * uint(sz.x) + id.x + 1u;
        }
    }
}

)HLSL" R"HLSL(
// The winner's read for S pixel c: its source pixel moved by the landing offset (c minus where the source landed). false: no winner, a hole. `guarded` says the guard
// moved the read: a source pixel with a nearer 3x3 neighbour (by more than kGuardMargin) on the side the offset points to would read across the edge, so it reads itself.
bool fgSynWinner(int2 c, out float2 rd, out bool guarded) {
    rd = float2(0.0, 0.0);
    guarded = false;
    uint key = KeyT.Load(int3(c.x, c.y, 0));
    uint s = SrcT.Load(int3(c.x, c.y, 0));
    if (key == 0u || s == 0u) return false;
    uint si = s - 1u;
    int2 w = int2(int(si % uint(sz.x)), int(si / uint(sz.x)));
    float2 pp;
    uint k2;
    if (!fgSynTarget(uint2(w), pp, k2)) return false;
    float2 off = float2(c) - pp;
    rd = float2(w) + off;
#if FG_GUARD
    float zw = synRawMetres(w);
    [unroll] for (int dy = -1; dy <= 1; ++dy) {
        [unroll] for (int dx = -1; dx <= 1; ++dx) {
            if (dx != 0 || dy != 0) {
                if (synRawMetres(w + int2(dx, dy)) < zw * kGuardMargin && dot(off, float2(dx, dy)) > 0.0) guarded = true;
            }
        }
    }
    if (guarded) rd = float2(w);
#endif
    return true;
}

// The background pull for a hole at c: among the neighbours within kFillRadius that have a winner, the one with the farthest key (the smallest; the nearest of equals), read
// kFillShift further into its own surface, away from the hole. false: nothing to pull from.
bool fgSynFill(int2 c, out float2 rd) {
    rd = float2(0.0, 0.0);
    uint bestKey = FG_FILL_FAR ? 0xFFFFFFFFu : 0u;
    float bestD = 1.0e9;
    int2 bestN = int2(0, 0);
    bool found = false;
    [loop] for (int dy = -kFillRadius; dy <= kFillRadius; ++dy) {
        [loop] for (int dx = -kFillRadius; dx <= kFillRadius; ++dx) {
            if (dx == 0 && dy == 0) continue;
            int2 n = c + int2(dx, dy);
            if (n.x < 0 || n.y < 0 || n.x >= sz.x || n.y >= sz.y) continue;
            uint kn = KeyT.Load(int3(n.x, n.y, 0));
            if (kn == 0u || SrcT.Load(int3(n.x, n.y, 0)) == 0u) continue;
            float d = float(dx * dx + dy * dy);
            bool better = FG_FILL_FAR ? (kn < bestKey) : (kn > bestKey);
            if (!found || better || (kn == bestKey && d < bestD)) {
                found = true;
                bestKey = kn;
                bestD = d;
                bestN = n;
            }
        }
    }
    if (!found) return false;
    float2 rn;
    bool gn;
    if (!fgSynWinner(bestN, rn, gn)) return false;
    rd = rn + kFillShift * normalize(float2(bestN - c));
    return true;
}

groupshared uint gCount[4];

[numthreads(8, 8, 1)]
void fgSynResolve(uint3 id : SV_DispatchThreadID, uint gi : SV_GroupIndex) {
    if (gi < 4) gCount[gi] = 0u;
    GroupMemoryBarrierWithGroupSync();
    if (id.x < uint(sz.x) && id.y < uint(sz.y)) {
        int2 c = int2(id.xy);
        float2 rd;
        bool guarded;
        bool none = false;
        if (fgSynWinner(c, rd, guarded)) {
            if (guarded) InterlockedAdd(gCount[2], 1u);
        } else if (fgSynFill(c, rd)) {
            InterlockedAdd(gCount[0], 1u);
        } else {
            rd = float2(0.0, 0.0);
            none = true;
            InterlockedAdd(gCount[1], 1u);
        }
        float2 off = rd - float2(c);
        if (!none) {
            if (any(abs(off) > kMaxOffset)) InterlockedAdd(gCount[3], 1u);
            off = clamp(off, -kMaxOffset, kMaxOffset);
        } else {
            off = float2(kNoRead, 0.0);
        }
        ReadOut[id.xy] = off;
    }
    GroupMemoryBarrierWithGroupSync();
    if (gi < 4 && gCount[gi] != 0u) InterlockedAdd(Stats[kCountBase + gi], gCount[gi]);
}

[numthreads(8, 8, 1)]
void fgSynResample(uint3 id : SV_DispatchThreadID) {
    if (id.x >= uint(sz.z) || id.y >= uint(sz.w)) return;
    float2 scale = float2(sz.z, sz.w) / float2(sz.x, sz.y);
    float2 ci = (float2(id.xy) + 0.5) / scale - 0.5;       // this output pixel's centre in render pixels
    int2 c = clamp(int2(floor(ci + 0.5)), int2(0, 0), int2(sz.x, sz.y) - 1);
    float2 off = ReadT.Load(int3(c.x, c.y, 0));
    float2 r = (off.x < kNoRead * 0.5) ? ci : ci + off;       // the read position is the pixel's own plus its offset
    SOut[id.xy] = synBilinear(Out, (r + 0.5) * scale, int2(sz.z, sz.w));
}

)HLSL" R"HLSL(
// The scorer's capture, on the S texture or the output: four taps a cell at (1, 3) of its 4 x 4 render pixels, converted to output pixels.
float synCellLuma(Texture2D<float4> t, uint2 cell) {
    float2 scale = float2(sz.z, sz.w) / float2(sz.x, sz.y);
    float2 r0 = float2(cell) * 4.0;
    float lum = 0.0;
    [unroll] for (int ty = 0; ty < 2; ++ty) {
        [unroll] for (int tx = 0; tx < 2; ++tx) {
            float2 r = r0 + float2(1.0 + 2.0 * tx, 1.0 + 2.0 * ty);
            lum += synLuma(synBilinear(t, r * scale, int2(sz.z, sz.w)).rgb);
        }
    }
    return lum * 0.25;
}

[numthreads(8, 8, 1)]
void fgSynLuma(uint3 id : SV_DispatchThreadID) {
    if (id.x >= uint(qz.x) || id.y >= uint(qz.y)) return;
    float z = 0.0;
    [unroll] for (int y = 0; y < 4; ++y) {
        [unroll] for (int x = 0; x < 4; ++x) {
            int2 q = clamp(int2(id.xy) * 4 + int2(x, y), int2(0, 0), int2(sz.x, sz.y) - 1);
            z = max(z, Depth.Load(int3(q.x, q.y, 0)));
        }
    }
    float metres;
    uint cls = synClass(z, metres);
    SLumaOut[id.xy] = synCellLuma(SIn, id.xy);
    BInfoOut[id.xy] = float2(synCellLuma(Out, id.xy), float(cls));
}

// The f = 1 S as the compositor would show it: C's cell direction (C's tangents) turned by the residual head rotation into the eye S was built for, projected with the tangents S
// was built with, and the kept quarter luma read there bilinearly. false: the direction falls outside S (or behind the eye).
bool synTurnedLuma(uint2 cell, out float lum) {
    lum = 0.0;
    float3 d = temporalPixelDir(float2(cell) * 4.0 + 1.5, tanB, int2(sz.x, sz.y));
#if FG_TURN
#if FG_TURN_SIGN
    float3 dS = float3(dot(rr[0].xyz, d), dot(rr[1].xyz, d), dot(rr[2].xyz, d));
#else
    float3 dS = rr[0].xyz * d.x + rr[1].xyz * d.y + rr[2].xyz * d.z;
#endif
#else
    float3 dS = d;
#endif
    float2 pS;
    if (!temporalProjectPixel(dS, tanK, int2(sz.x, sz.y), pS)) return false;
    if (pS.x < -0.5 || pS.y < -0.5 || pS.x > float(sz.x) - 0.5 || pS.y > float(sz.y) - 0.5) return false;
    float2 f = (pS - 1.5) * 0.25;                       // in cells, centres at integers
    int2 i0 = int2(floor(f));
    float2 w = f - float2(i0);
    int2 hi = int2(qz.x, qz.y) - 1;
    int2 a = clamp(i0, int2(0, 0), hi);
    int2 b = clamp(i0 + 1, int2(0, 0), hi);
    float l00 = SLumaIn.Load(int3(a.x, a.y, 0));
    float l10 = SLumaIn.Load(int3(b.x, a.y, 0));
    float l01 = SLumaIn.Load(int3(a.x, b.y, 0));
    float l11 = SLumaIn.Load(int3(b.x, b.y, 0));
    lum = lerp(lerp(l00, l10, w.x), lerp(l01, l11, w.x), w.y);
    return true;
}

groupshared uint gScore[299];   // kBlockUints
groupshared uint gTurn[299];    // the same, S turned

[numthreads(8, 8, 1)]
void fgSynScore(uint3 id : SV_DispatchThreadID, uint gi : SV_GroupIndex) {
    for (uint i = gi; i < kBlockUints; i += 64) { gScore[i] = 0u; gTurn[i] = 0u; }
    GroupMemoryBarrierWithGroupSync();
    bool active = id.x < uint(qz.x) && id.y < uint(qz.y);
    uint cls = 0u, bin = 0u, binId = 0u, sumE = 0u, sumId = 0u;
    bool turnedIn = false;
    uint binT = 0u, sumT = 0u;
    if (active) {
        float lumC = synCellLuma(Out, id.xy);
        float lumS = SLumaIn.Load(int3(id.x, id.y, 0));
        float2 bi = BInfoIn.Load(int3(id.x, id.y, 0));
        cls = min(uint(bi.y + 0.5), 2u);
        float e = abs(lumC - lumS);
        float eId = abs(lumC - bi.x);
        bin = synBin(e);
        binId = synBin(eId);
        sumE = uint(e * 1024.0 + 0.5);
        sumId = uint(eId * 1024.0 + 0.5);
        float lumT;
        turnedIn = synTurnedLuma(id.xy, lumT);
        if (turnedIn) {
            float eT = abs(lumC - lumT);
            binT = synBin(eT);
            sumT = uint(eT * 1024.0 + 0.5);
        }
    }
    // The shared-memory adds sit after every read and before the barrier, with no early exit in the group.
    if (active) {
        uint b = cls * kPerClass;
        InterlockedAdd(gScore[kBlockUints - 1], 1u);
        InterlockedAdd(gScore[b + bin], 1u);
        InterlockedAdd(gScore[b + kBins + binId], 1u);
        InterlockedAdd(gScore[b + 2 * kBins], 1u);
        InterlockedAdd(gScore[b + 2 * kBins + 1], sumE);
        InterlockedAdd(gScore[b + 2 * kBins + 2], sumId);
        if (turnedIn) {
            InterlockedAdd(gTurn[kBlockUints - 1], 1u);
            InterlockedAdd(gTurn[b + binT], 1u);
            InterlockedAdd(gTurn[b + kBins + binId], 1u);
            InterlockedAdd(gTurn[b + 2 * kBins], 1u);
            InterlockedAdd(gTurn[b + 2 * kBins + 1], sumT);
            InterlockedAdd(gTurn[b + 2 * kBins + 2], sumId);
        } else {
            InterlockedAdd(gTurn[kHoles], 1u);
        }
    }
    GroupMemoryBarrierWithGroupSync();
    for (uint j = gi; j < kBlockUints; j += 64) {
        uint v = gScore[j];
        if (v != 0u) InterlockedAdd(Stats[j], v);
        uint vt = gTurn[j];
        if (vt != 0u) InterlockedAdd(Stats[kTurnBase + j], vt);
    }
}
)HLSL";

inline const std::string& frameGenSynthShaderSource() {
    static const std::string s = frameGenCarryText() + kFrameGenSynthCs;
    return s;
}

}  // namespace edvr
