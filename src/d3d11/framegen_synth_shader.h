#pragma once
// The frame-generation synthesizer's compute shaders (fix.frame_generation_shadow, TEMPORARY; docs/design-frame-generation-2026-10-26.md, Phase 2.1, "The synthesis pass").
// One source, seven entry points (and Phase 2.3b's eighth, fgSynFinish, outside the pass sequence), one constant block (src\common\framegen_synth_math.h FrameGenSynthCb). Nothing they write is shown in the headset. Before them the key grid and the
// stats words are cleared to 0 by the API (ClearUnorderedAccessViewUint, framegen_synth_pass.h); the read offsets are not cleared: a pixel whose key is 0 has kNoRead written by
// fgSynResolve, and a pixel whose key is not 0 has its offset written by fgSynSrc (the pixel that set the key finds it there again and writes).
//   fgSynSplat     each B pixel carried to S by its class's motion (fgSynTarget; its class and depth are the nearest of the 3x3 around it, synDepth9, taken where they are read) and
//                  scattered as a key into the four pixels of its bilinear footprint (InterlockedMax: the nearer surface wins; a pixel that is its own nearest beats one that borrowed
//                  the dilated depth);
//   fgSynSrc       the second pass over the same keys: each pixel writes, to every footprint pixel whose final key is its own, that S pixel's READ OFFSET: the B position to read,
//                  as an offset from the S pixel's own centre, two half floats packed in a uint (kMaxOffset, kNoRead; synPackOffset rounds to nearest even as a typed half store
//                  does). The read is the pixel's own position moved by the landing offset (the S pixel minus where the pixel landed), or the pixel's own position itself when the
//                  guard says so (a nearer 3x3 neighbour on the side the offset points to would be read across the edge), then an integer pair stored exactly with the guard's
//                  mark (synPackGuarded); a read past kMaxOffset is clamped and counted;
//   fgSynResolve   every S pixel no key reached is a hole: kNoRead written, counted as left, its index listed in the hole list (u0, an eighth of the render pixels; the count in the
//                  stats words); the S pixels whose stored offset carries the guard's mark are counted as guarded, one each;
//   fgSynHoles     one thread per listed hole, in full 64-thread groups: the farthest key found within 8 px of it read 2 px further into its own surface (the background pull) becomes
//                  its read offset, counted as filled and no longer left. The neighbour's read is taken in full precision, as fgSynSrc computed it before the store rounded it
//                  (synWinnerRead finds the neighbour's source pixel again from the stored offset and recomputes its read), so the fill is the same as when every read was computed
//                  in one pass; the counters (filled, left, guard, clamped) of the three passes add up in the stats words;
//   fgSynResample  output size: each output pixel to its S pixel, its read offset, B's output there, bilinear, into the S texture (dlOut's format);
//   fgSynLuma      quarter grid: S's luma the way the scorer's capture takes it, B's luma and B's class (the f = 1 S is kept for the next real frame);
//   fgSynScore     quarter grid, on the next real frame C: C's luma against the kept S luma and against B's (the no-warp comparison), binned per class of B as the scorer bins; and again
//                  against the kept S luma TURNED by the residual head rotation (the compositor's step: C's cell direction rotated into the eye S was built for, projected with the
//                  tangents S was built with, a bilinear read of the quarter luma; outside S: not scored, counted).
// The pixel class and the carry are the temporal pass's own text: the source the build compiles is the text between the two EDVR_TEMPORAL_CARRY markers of kTemporalCsHlsl, then
// kFrameGenSynthCs (frameGenSynthShaderSource); the markers are read out of kTemporalCsHlsl by frameGenCarryText (framegen_shadow_shader.h).
//
// Bindings: t0 B's output (the upscaler's, before the interface; on C's frame the score reads C's there), t1 B's depth copy, t2 the DLSS vectors (R16G16_FLOAT, render size, pixels,
// this frame to the previous, jitter-free), t3 and t5 fgSynFinish's frame and UI layer (t10 its multiply, u2 its output: Phase 2.3b, at the end of the text), t4 the key grid, t6 the read offsets, t7 the S texture, t8 the kept S luma, t9 the kept B luma (x) and class (y);
// u0 the hole list (a structured buffer of uints), u1 the key grid (R32_UINT), u2 fgSynFinish's output, u3 the read offsets (R32_UINT: two packed half floats, or a guarded read's integer
// pair with the mark; the resolve and the hole pass read other pixels' through this view, a typed uint load every D3D11 part does), u4 the S texture, u5 the stats words, u6 the
// S luma, u7 B's luma and class; b0 P.
//
// The key: 0 a hole, 4 the far plane, else (max(asuint(1 / metres), 8) & ~7) | 4 when the pixel is its own nearest | class (1 cockpit, 2 world). A larger key is a nearer surface.
//
// The eight #defines at the top of kFrameGenSynthCs are the rig's mutation points (tools\framegen_synth_test replaces one with 0 and the case it guards must fail); production
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
#define FG_DILATE_MV 1   // a pixel moves by the vector of the 3x3 pixel whose depth it took (0: its own)
#define FG_SENTINEL 1   // a rejected-history vector (the size * 2 sentinel) borrows its background's (0: taken as motion)
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
Texture2D<uint>   ReadT   : register(t6);   // the read offsets, packed
Texture2D<float4> SIn     : register(t7);
Texture2D<float>  SLumaIn : register(t8);
Texture2D<float2> BInfoIn : register(t9);
RWStructuredBuffer<uint> Holes : register(u0);   // the render pixel index of every hole the resolve found, for fgSynHoles; the count is Stats[kHoleCount]
RWTexture2D<uint>   Key       : register(u1);
RWTexture2D<uint>   ReadOut   : register(u3);   // R32_UINT: two half floats (synPackOffset)
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
static const uint  kHoleCount = 602;      // the holes listed in Holes (kFrameGenSynthStatsHoles)
static const uint  kStatsUints = 603;     // src\common\framegen_synth_math.h kFrameGenSynthStatsUints
static const float kGuardMargin = 0.9;    // a 3x3 neighbour nearer by more than a tenth of the pixel's own distance
static const int   kFillRadius = 8;
static const float kFillShift = 2.0;
static const float kNoRead = -4096.0;       // framegen_synth_math.h kFrameGenSynthNoRead
static const float kMaxOffset = 2000.0;    // kFrameGenSynthMaxOffset
static const uint  kGuardMark = 0x7C00u;      // a packed offset whose x half has this exponent field (a NaN's, never a clamped offset's) is a guarded read's: an integer pair, stored as such

float synLuma(float3 c) { return clamp(dot(c, float3(0.2126, 0.7152, 0.0722)), 0.0, 1.0); }

// A float's half-float bits, rounded toward zero: what the typed R16G16_FLOAT store the offsets went through before they shared a uint with the source grid's slot did on the
// pilot's NVIDIA card (D3D leaves a store's float16 rounding to the implementation; the replays of 2026-11-07 match that store byte for byte with truncation and not with
// round-to-nearest-even), so the offsets packed here are the ones that store kept. Finite inputs (the offsets are clamped). synUnpackOffset is exact (f16tof32).
uint synHalfBits(float v) {
    uint b = asuint(v);
    uint sign = (b >> 16) & 0x8000u;
    uint a = b & 0x7FFFFFFFu;
    if (a >= 0x47800000u) return sign | 0x7C00u;                             // 65536 and up: infinity
    if (a < 0x38800000u) return sign | uint(asfloat(a) * 16777216.0);       // under 2^-14: a half denormal or zero, in units of 2^-24, truncated
    return sign | (((a >> 23) - 112u) << 10) | ((a >> 13) & 0x3FFu);
}
uint synPackOffset(float2 o) { return synHalfBits(o.x) | (synHalfBits(o.y) << 16); }

// A guarded read's offset (the source pixel minus the S pixel: integers, within the clamp) with the mark, in the bits the mark leaves: x + 2048 in bits 0-9 and 15-16, y + 2048
// in bits 17-28. The mark is how fgSynResolve counts the guarded pixels, once each, from the one store fgSynSrc makes.
uint synPackGuarded(float2 o) {
    uint x = uint(int(o.x) + 2048), y = uint(int(o.y) + 2048);
    return kGuardMark | (x & 0x3FFu) | (((x >> 10) & 3u) << 15) | (y << 17);
}
bool synIsGuarded(uint p) { return (p & kGuardMark) == kGuardMark; }
float2 synUnpackOffset(uint p) {
    if (synIsGuarded(p)) return float2(int((p & 0x3FFu) | (((p >> 15) & 3u) << 10)) - 2048, int((p >> 17) & 0xFFFu) - 2048);
    return float2(f16tof32(p & 0xFFFFu), f16tof32(p >> 16));
}

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

// The 3x3 depths around a pixel (z9, row by row, the pixel's own at 4) and their nearest (the larger reversed-Z value): the class and depth an edge pixel takes are the near
// object's, and the guard compares the neighbours with the centre.
float synDepth9(int2 q, out float z9[9]) {
    float z = 0.0;
    [unroll] for (int i = 0; i < 9; ++i) {
        int2 n = clamp(q + int2(i % 3 - 1, i / 3 - 1), int2(0, 0), int2(sz.x, sz.y) - 1);
        z9[i] = Depth.Load(int3(n.x, n.y, 0));
        z = max(z, z9[i]);
    }
    return z;
}

// The pixel whose depth synDepth9 took (the pixel's own when it ties): its DLSS vector is the one the pixel moves by, so an edge pixel the near object's depth reached
// moves with the near object and does not carry its colour (the outline the game draws just outside a hull) away at the background's motion (2026-11-07).
int2 synNearest9(int2 q, float z9[9], float zr) {
#if FG_DILATE_MV
    if (z9[4] == zr) return q;
    int k = 4;
    [unroll] for (int i = 8; i >= 0; --i) if (z9[i] == zr) k = i;
    return clamp(q + int2(k % 3 - 1, k / 3 - 1), int2(0, 0), int2(sz.x, sz.y) - 1);
#else
    return q;
#endif
}

// A written depth's metres, 1e9 for the far plane (the guard's comparisons).
float synMetresOrFar(float zr) {
    bool far;
    float z = temporalDepthMetres(zr, dk.x, dk.y, far);
    return far ? 1.0e9 : z;
}

// B's DLSS vector at q, for the ex2dh rule. Where the temporal pass rejected the history (a background just uncovered beside a nearer edge, a masked engine record, a fresh
// screen edit) it writes the size * 2 sentinel instead of the motion (temporal_shader_source.h, `written`): not a vector, and taken as one it throws the pixel off the image
// and leaves a hole band beside every moving edge, filled as a lattice (2026-11-07, the eye dump 222433569001). Such a pixel borrows the vector of the farthest pixel
// without the sentinel on a sparse ring (2 to 32 px, 8 directions): the uncovered background moves as the background beside it. None found: the head's motion alone.
bool synRejected(float2 mv) { return any(abs(mv) >= float2(sz.x, sz.y)); }
float2 synVector(int2 q, float2 headPrevMinusB) {
    float2 mv = Mv.Load(int3(q.x, q.y, 0));
#if FG_SENTINEL
    if (!synRejected(mv)) return mv;
    float bestZ = 2.0;
    float2 best = headPrevMinusB;
    [loop] for (int r = 2; r <= 32; r *= 2) {
        [unroll] for (int j = 0; j < 8; ++j) {
            int2 d = int2(j < 3 ? -1 : (j > 4 ? 1 : 0), (j == 0 || j == 3 || j == 5) ? -1 : ((j == 2 || j == 4 || j == 7) ? 1 : 0));
            int2 n = clamp(q + d * r, int2(0, 0), int2(sz.x, sz.y) - 1);
            float2 v = Mv.Load(int3(n.x, n.y, 0));
            float zn = Depth.Load(int3(n.x, n.y, 0));
            if (!synRejected(v) && zn < bestZ) { bestZ = zn; best = v; }
        }
    }
    return best;
#else
    return mv;
#endif
}

// Where B's pixel id lands in S, and its key, from its dilated depth zr (synDepth9's nearest) and `own` (4 when the pixel's own depth is zr, else 0). The class and depth are
// the dilated depth's. false: behind the eye. The motion is src\common\framegen_synth_math.h frameGenSynthTarget's: cockpit the head's rigid step with the depth; far and world
// the ex2dh rule on B's vector, rotation only.
bool fgSynTargetZ(uint2 id, float zr, uint own, int2 vq, out float2 pp, out uint key) {
    pp = float2(0.0, 0.0);
    key = 0u;
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
#if FG_REMOVE_HEAD
        float3 dH = float3(dot(hp[0].xyz, dB), dot(hp[1].xyz, dB), dot(hp[2].xyz, dB));
        float2 pHeadPrev;
        if (!temporalProjectPixel(dH, tanB, int2(sz.x, sz.y), pHeadPrev)) return false;
#else
        float2 pHeadPrev = pB;
#endif
        float2 mv = synVector(vq, pHeadPrev - pB);
        float2 m = (pB + mv) - pHeadPrev;
        float3 dS = temporalPixelDir(pB - f * m, tanB, int2(sz.x, sz.y));
        float3 dR = float3(dot(rs[0].xyz, dS), dot(rs[1].xyz, dS), dot(rs[2].xyz, dS));
        if (!temporalProjectPixel(dR, tanB, int2(sz.x, sz.y), pp)) return false;
    }
    key = 4u;
    if (cls != 0u) key = (max(asuint(1.0 / max(zKey, 1e-6)), 8u) & ~7u) | own | cls;
    return true;
}

// fgSynTargetZ with the pixel's 3x3 read here (the splat).
bool fgSynTarget(uint2 id, out float2 pp, out uint key) {
    float z9[9];
    float zr = synDepth9(int2(id), z9);
    return fgSynTargetZ(id, zr, (z9[4] == zr) ? 4u : 0u, synNearest9(int2(id), z9, zr), pp, key);
}

// The 3x3 depths' metres (synMetresOrFar of each), once per source pixel: the guard's comparisons.
void synMetres9(float z9[9], out float m9[9]) {
    [unroll] for (int j = 0; j < 9; ++j) m9[j] = synMetresOrFar(z9[j]);
}

// The guard: a source pixel (its 3x3 metres m9) with a nearer neighbour (by more than kGuardMargin) on the side its landing offset `off` points to would read across the
// edge, so it reads itself.
bool synGuarded(float m9[9], float2 off) {
#if FG_GUARD
    float zw = m9[4] * kGuardMargin;
    bool g = false;
    [unroll] for (int j = 0; j < 9; ++j) {
        if (j != 4) {
            float2 d = float2(j % 3 - 1, j / 3 - 1);
            if (m9[j] < zw && dot(off, d) > 0.0) g = true;
        }
    }
    return g;
#else
    return false;
#endif
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

)HLSL" R"HLSL(
groupshared uint gSrc;   // clamped

// The winner's read offset for every S pixel its key won: its own position moved by the landing offset (c minus where it landed), guarded, as an offset from c. A pixel two
// sources tie for is written by both (the splat's tie: the last write stands). A guarded read is stored with the mark, for the resolve to count once per S pixel; a clamped
// read is counted here, per write.
[numthreads(8, 8, 1)]
void fgSynSrc(uint3 id : SV_DispatchThreadID, uint gi : SV_GroupIndex) {
    if (gi == 0) gSrc = 0u;
    GroupMemoryBarrierWithGroupSync();
    if (id.x < uint(sz.x) && id.y < uint(sz.y)) {
        float z9[9];
        float zr = synDepth9(int2(id.xy), z9);
        float2 pp;
        uint key;
        if (fgSynTargetZ(id.xy, zr, (z9[4] == zr) ? 4u : 0u, synNearest9(int2(id.xy), z9, zr), pp, key)) {
            float m9[9];
            synMetres9(z9, m9);
            int2 base = int2(floor(pp));
            [unroll] for (int dy = 0; dy < 2; ++dy) {
                [unroll] for (int dx = 0; dx < 2; ++dx) {
                    int2 c = base + int2(dx, dy);
                    if (c.x >= 0 && c.y >= 0 && c.x < sz.x && c.y < sz.y && KeyT.Load(int3(c.x, c.y, 0)) == key) {
                        float2 off = float2(c) - pp;
                        float2 rd = float2(id.xy) + off;
                        bool guarded = synGuarded(m9, off);
                        if (guarded) rd = float2(id.xy);
                        float2 o = rd - float2(c);
                        if (any(abs(o) > kMaxOffset)) InterlockedAdd(gSrc, 1u);
                        o = clamp(o, -kMaxOffset, kMaxOffset);
                        ReadOut[uint2(c)] = guarded ? synPackGuarded(o) : synPackOffset(o);
                    }
                }
            }
        }
    }
    GroupMemoryBarrierWithGroupSync();
    if (gi == 0 && gSrc != 0u) InterlockedAdd(Stats[kCountBase + 3], gSrc);
}

// The read of a pixel n that has a winner, in full precision (what fgSynSrc computed before the store rounded it to halves): the winner's source pixel is found again as the B
// pixel whose key is n's and whose footprint holds n, within the window the stored offset leaves (the source is the offset's end moved by the landing, under a pixel, and the
// rounding, under half a pixel at the clamp), and its read is recomputed. Of tied sources (the same key, n in both footprints) the one whose read rounds to the stored offset is
// taken; the stored offset itself stands in when none is found (a clamped read).
bool synWinnerRead(int2 n, out float2 rd) {
    rd = float2(0.0, 0.0);
    uint key = KeyT.Load(int3(n.x, n.y, 0));
    float2 o = synUnpackOffset(ReadOut[uint2(n)]);
    if (key == 0u || o.x < kNoRead * 0.5) return false;
    float2 approx = float2(n) + o;
    rd = approx;
    int2 lo = int2(floor(approx - 1.5));
    float bestD = 1.0e9;
    [loop] for (int j = 0; j < 16; ++j) {
        int2 w = lo + int2(j & 3, j >> 2);
        if (w.x < 0 || w.y < 0 || w.x >= sz.x || w.y >= sz.y) continue;
        float z9[9];
        float zr = synDepth9(w, z9);
        float2 pp;
        uint kw;
        if (!fgSynTargetZ(uint2(w), zr, (z9[4] == zr) ? 4u : 0u, synNearest9(w, z9, zr), pp, kw) || kw != key) continue;
        int2 dd = n - int2(floor(pp));
        if (dd.x < 0 || dd.x > 1 || dd.y < 0 || dd.y > 1) continue;
        float2 off = float2(n) - pp;
        float2 r = float2(w) + off;
        float m9[9];
        synMetres9(z9, m9);
        if (synGuarded(m9, off)) r = float2(w);
        float2 oc = clamp(r - float2(n), -kMaxOffset, kMaxOffset);
        float d = abs(oc.x - o.x) + abs(oc.y - o.y);
        if (d < bestD) { bestD = d; rd = r; }
    }
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
        [unroll] for (int dx = -kFillRadius; dx <= kFillRadius; ++dx) {   // a row's 17 loads in flight together
            if (dx == 0 && dy == 0) continue;
            int2 n = c + int2(dx, dy);
            if (n.x < 0 || n.y < 0 || n.x >= sz.x || n.y >= sz.y) continue;
            uint kn = KeyT.Load(int3(n.x, n.y, 0));   // a key that is not 0 has its read offset written (fgSynSrc, the header)
            if (kn == 0u) continue;
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
    if (!synWinnerRead(bestN, rn)) return false;
    rd = rn + kFillShift * normalize(float2(bestN - c));
    return true;
}

uint fgSynHoleCapacity() { return uint(sz.x) * uint(sz.y) / 8u; }   // src\common\framegen_synth_math.h frameGenSynthHoleCapacity

groupshared uint gCount[3];   // left, guard; the holes of this group
groupshared uint gHoleBase;   // where this group's holes start in Holes

// Every S pixel no key reached is a hole: kNoRead for now, counted as left, and its index listed in Holes for fgSynHoles (the group reserves one run of the list; a hole past
// the list's capacity stays left). A pixel whose stored offset carries the guard's mark is counted as guarded.
[numthreads(8, 8, 1)]
void fgSynResolve(uint3 id : SV_DispatchThreadID, uint gi : SV_GroupIndex) {
    if (gi < 3) gCount[gi] = 0u;
    GroupMemoryBarrierWithGroupSync();
    bool hole = false;
    uint rank = 0u;
    if (id.x < uint(sz.x) && id.y < uint(sz.y)) {
        if (KeyT.Load(int3(id.x, id.y, 0)) == 0u) {
            hole = true;
            ReadOut[id.xy] = synPackOffset(float2(kNoRead, 0.0));
            InterlockedAdd(gCount[0], 1u);
            InterlockedAdd(gCount[2], 1u, rank);
        } else if (synIsGuarded(ReadOut[id.xy])) {
            InterlockedAdd(gCount[1], 1u);
        }
    }
    GroupMemoryBarrierWithGroupSync();
    if (gi == 0 && gCount[2] != 0u) InterlockedAdd(Stats[kHoleCount], gCount[2], gHoleBase);
    if (gi == 1 && gCount[0] != 0u) InterlockedAdd(Stats[kCountBase + 1], gCount[0]);
    if (gi == 2 && gCount[1] != 0u) InterlockedAdd(Stats[kCountBase + 2], gCount[1]);
    GroupMemoryBarrierWithGroupSync();
    if (hole) {
        uint slot = gHoleBase + rank;
        if (slot < fgSynHoleCapacity()) Holes[slot] = id.y * uint(sz.x) + id.x;
    }
}

groupshared uint gFill[2];   // filled, clamped

// The background pull for the listed holes, one thread each in full groups: run by every lane of an 8x8 group that held one hole, fgSynFill's 289-neighbour search cost more than
// the rest of the resolve. A filled hole takes its read offset (clamped and counted as the winners' are), is counted as filled and is no longer left; the groups past the count
// return at once.
[numthreads(64, 1, 1)]
void fgSynHoles(uint3 id : SV_DispatchThreadID, uint gi : SV_GroupIndex) {
    if (gi < 2) gFill[gi] = 0u;
    GroupMemoryBarrierWithGroupSync();
    uint count = min(Stats[kHoleCount], fgSynHoleCapacity());
    if (id.x < count) {
        uint p = Holes[id.x];
        int2 c = int2(int(p % uint(sz.x)), int(p / uint(sz.x)));
        float2 rd;
        if (fgSynFill(c, rd)) {
            float2 off = rd - float2(c);
            if (any(abs(off) > kMaxOffset)) InterlockedAdd(gFill[1], 1u);
            ReadOut[uint2(c)] = synPackOffset(clamp(off, -kMaxOffset, kMaxOffset));
            InterlockedAdd(gFill[0], 1u);
        }
    }
    GroupMemoryBarrierWithGroupSync();
    if (gi == 0 && gFill[0] != 0u) {
        InterlockedAdd(Stats[kCountBase], gFill[0]);
        InterlockedAdd(Stats[kCountBase + 1], 0u - gFill[0]);   // the resolve counted every hole as left; the sum stays a count
    }
    if (gi == 1 && gFill[1] != 0u) InterlockedAdd(Stats[kCountBase + 3], gFill[1]);
}

[numthreads(8, 8, 1)]
void fgSynResample(uint3 id : SV_DispatchThreadID) {
    if (id.x >= uint(sz.z) || id.y >= uint(sz.w)) return;
    float2 scale = float2(sz.z, sz.w) / float2(sz.x, sz.y);
    float2 ci = (float2(id.xy) + 0.5) / scale - 0.5;       // this output pixel's centre in render pixels
    int2 c = clamp(int2(floor(ci + 0.5)), int2(0, 0), int2(sz.x, sz.y) - 1);
    float2 off = synUnpackOffset(ReadT.Load(int3(c.x, c.y, 0)));
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
)HLSL" R"HLSL(
// Phase 2.3b, S finished like B (fgSynFinish): the UI layer B's door composited over B, composited over S the same way (ui_layer's kUiLayerCompositeHlsl, line for line: the
// layer and the multiply averaged over the output pixel's exact footprint, at most 4 x 4 taps, out = L.rgb + F.rgb * L.a * M.rgb, the frame's alpha kept), except that each S
// pixel reads the layer where B's eye saw the same cockpit-fixed point: S's pixel centre carried to B's output pixels by the head's step alone (a homography, the layer taken as a
// plane at a fixed panel distance; exact for the rotation). Its own block in P (framegen_synth_math.h frameGenSynthFinishFillCb): sz.xy the output (the region's) size, sz.zw the
// region's origin in FinFrame; qz.x 1 when a multiply drew into M this frame; dk.xy the layer's size in texels; tanK the layer's rectangle (u0, v0, u1, v1) for the region; rs[0..2]
// the homography's rows, S output pixel (centre at integers) to B's. A homography that is the identity reads exactly the texels B's composite read.
Texture2D<float4>   FinFrame : register(t3);
Texture2D<float4>   FinLayer : register(t5);
Texture2D<float4>   FinMult  : register(t10);
RWTexture2D<float4> FinOut   : register(u2);
[numthreads(8, 8, 1)]
void fgSynFinish(uint3 id : SV_DispatchThreadID) {
    if (id.x >= uint(sz.x) || id.y >= uint(sz.y)) return;
    float4 f = FinFrame.Load(int3(sz.zw + int2(id.xy), 0));
    float3 p = float3(float2(id.xy), 1.0);
    float3 hq = float3(dot(rs[0].xyz, p), dot(rs[1].xyz, p), dot(rs[2].xyz, p));
    float4 l = float4(0, 0, 0, 1);
    float3 m = float3(1, 1, 1);
    if (hq.z > 1e-6) {
        float2 q = hq.xy / hq.z;
        float2 layerSize = dk.xy;
        float2 span = (tanK.zw - tanK.xy) * layerSize / float2(sz.xy);
        float2 x0 = tanK.xy * layerSize + q * span;
        float2 x1 = x0 + span;
        int2 k0 = max(int2(floor(x0)), int2(0, 0));
        int2 k1 = min(int2(ceil(x1)) - 1, int2(layerSize) - 1);
        float4 acc = float4(0, 0, 0, 0);
        float3 accM = float3(0, 0, 0);
        float wsum = 0;
        [loop] for (int j = k0.y; j <= min(k1.y, k0.y + 3); ++j) {
            float wy = min(x1.y, (float)j + 1.0) - max(x0.y, (float)j);
            if (wy <= 0) continue;
            [loop] for (int i = k0.x; i <= min(k1.x, k0.x + 3); ++i) {
                float wx = min(x1.x, (float)i + 1.0) - max(x0.x, (float)i);
                if (wx <= 0) continue;
                acc += FinLayer.Load(int3(i, j, 0)) * (wx * wy);
                if (qz.x != 0) accM += FinMult.Load(int3(i, j, 0)).rgb * (wx * wy);
                wsum += wx * wy;
            }
        }
        l = wsum > 0 ? acc / wsum : float4(0, 0, 0, 1);
        m = (qz.x != 0 && wsum > 0) ? accM / wsum : float3(1, 1, 1);
    }
    float3 c = l.rgb + f.rgb * l.a * m;
    FinOut[id.xy] = float4(saturate(c), f.a);
}
)HLSL";

inline const std::string& frameGenSynthShaderSource() {
    static const std::string s = frameGenCarryText() + kFrameGenSynthCs;
    return s;
}

}  // namespace edvr
