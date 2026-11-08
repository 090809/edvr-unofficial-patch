// diag.hull_layer (TEMPORARY): the own hull as a private, lit layer -- Step 1 v2 in the eye dump, Steps 2-3 (separate) out of
// the main pass and composited back after the upscaler. hull_layer.h says what it does; docs\research-own-hull-layer-2026-11-08.md
// ("Step 1 v2 build", "Step 2 build", "Step 3 build") says what each must show.
#include "hull_layer.h"

#include <windows.h>
#include <d3d11.h>

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../common/config.h"
#include "../common/log.h"
#include "../common/runtime_profile.h"   // runtimeVrProfile: Step 3's discard is the VR eye pass's only
#include "binding_shadow.h"
#include "cs_stage_save.h"    // Step 3's dispatches: the compute stage put back as the game (or the temporal pass) left it
#include "engine_velocity.h"  // engineVelocityLiveNow, engineVelocityOwnDiscardPs and its mask slot (kEngineVelocityOwnMaskPsSlot)
#include "framegen_shadow.h"  // frameGenShadowOn: Step 3 stands aside while any frame generation key is on
#include "native_on_foot.h"   // nativeOnFootActive: no discard on foot
#include "temporal_pass.h"    // temporalPassArmEyeDump: the auto dump arms the eye dump the hotkey arms; temporalPassFrameGenMode
#include "tonemap_admit.h"    // Step 3: the game's tonemap, admitted by structure
#include "vr_world_route.h"   // VrWorldInternalScope: the layer's own clears, copies and passes step past the game-facing hooks
#include "vscreen.h"          // the raw setters: the binding shadow keeps describing the game's bindings

namespace edvr {

namespace detail {
bool g_hullLayerOn = false;
bool g_hullLayerSeparate = false;
}  // namespace detail

namespace {

// The own ship's families (engine_velocity_families.h kFamilies: vs_66DE..., vs_61AE..., vs_AACF..., vs_DE54...).
constexpr uint64_t kHullVs[4] = {0x66DE2CADB1F4AE6Bull, 0x61AE8EB05FDC18DDull, 0xAACFDCF2FB9AD809ull, 0xDE545DC8EE4FBB87ull};
// The hull decals (engine_velocity_families.h: vs_BBE5 with ps_DB3E, the guarded overlay pair in the VR eye passes).
constexpr uint64_t kDecalVs = 0xBBE58E40FE88EC80ull;
constexpr uint64_t kShellVs = 0xBFE51414CC3024B4ull;     // the cockpit shell (engine_velocity_families.h)
constexpr uint64_t kTerrainVs = 0x72BDD292154158ADull;   // the terrain's colour pass (celestial_motion.h kCelestialPatchVs)
constexpr uint32_t kTerrainVsHigh = 0x6C320453u;         // the other terrain family, known by its prefix only (the ledgers)
constexpr uint64_t kSkyVs = 0xF8FA801F2CB1E27Cull;       // the sky, the first forward draw after the lighting (flat_projection_recipes.h)
// The lighting: the full-screen sun and ambient, then the instanced volumes (the doc's "The binding map", the mt= census).
// The last is the game's night vision (2026-11-08 census 3 against 4: one draw an eye into the HDR after the lighting,
// reading the depth copy and G-buffer RT0); re-issued like a light so the own hull gets it in separate mode.
constexpr uint64_t kLightVs[7] = {0x7E38A6AA1269C901ull, 0x8289669D93A18C1Dull, 0x963B52C73B4143ACull,
                                  0x24DE25E496342EB8ull, 0xB121A79E457669E8ull, 0x0357BBB2DEE43C1Full,
                                  0xFCF7BD2896751D96ull};
constexpr UINT kPoolSlot = 33;            // VS t33: the pool a pool draw reads
constexpr UINT kSlotTarget = 6;           // engine motion's MRT6 (dxbc_engine_velocity.h kEngineVelocityTarget): float2(2*slot+1, depth)
constexpr uint32_t kRecordBytes = 336;    // a pool record (tools\eye_run_ledger.py REC)
constexpr uint32_t kMaxSeedInstances = 4096;
constexpr uint32_t kMaxRecords = 1u << 20;
constexpr float kOwnRadius = 120.0f;      // metres about the seed records' centroid (the five ledgers: object A at 120 m)
constexpr UINT kSrvSlots = 16;            // PS resource slots a stage is searched and restored over
constexpr size_t kMaxMirrors = 32;
constexpr size_t kMaxViews = 128;
constexpr uint32_t kEvictFrames = 120;    // a private copy whose game texture was not seen for longer goes
constexpr uint32_t kRing = 4;             // readback staging buffers an eye
constexpr uint32_t kReadbackLag = 2;      // frames before a readback is tried (DO_NOT_WAIT)
constexpr uint32_t kLightSpan = 64;       // draws after the shadow mask that may pass with no sky before the lighting window closes
constexpr uint32_t kMaxUnknownPairs = 16;
// kAutoMax 0: the auto dumps are off (the pilot, 2026-11-08 22:25: each dump's hitch in the headset is too disruptive);
// the dump_eyes key still arms one by hand. Raise it to bring them back for an unattended measurement flight.
constexpr uint32_t kAutoFrames = 600, kAutoMax = 0, kAutoOwnLo = 20, kAutoOwnHi = 200;
constexpr ULONGLONG kAutoGapMs = 60000;
constexpr ULONGLONG kReportMs = 30000;
// Step 3 (separate): the discard's safety. A mask past kOwnMaskMax records holds a station's or another ship's records
// within the radius (docking): the frame falls back. A miss (an eye seeded but not lit, tonemapped and composited) holds
// the discard off for kHoldFrames frames. 1500, not 400: the own ship's undrawn records near it (interior, LODs) put
// 260-700 records in the mask over terrain with no neighbour (ledgers 193029413006, 214124277001; 2026-11-08 21:40).
constexpr uint32_t kOwnMaskMax = 1500;
// 120, not 16 (2026-11-08 22:00): while textures streamed in, misses came every few dozen frames and each 16-frame hold
// ended in another lost frame, so the ship blinked. The layer still runs and is checked on held frames, so a hold only
// ends after 120 frames in a row without a miss. Any other fallback ending starts the same hold (decideNext).
constexpr uint32_t kHoldFrames = 120;
constexpr float kTaaCurrent = 0.1f;       // the hull layer's TAA: this frame's weight against the clamped history
constexpr UINT kTimerRing = 8;            // timestamp sets a timer keeps in flight
constexpr int kToneStage = 10;            // Pending::stage of the tonemap re-issue (not a Stage)

enum Stage : int { kCopy = 0, kMask = 1, kLight = 2, kStages = 3 };
struct StagePair {
    uint64_t vs, ps;
    Stage stage;
    uint8_t bit;   // once per eye per frame
};
// The doc's binding map ("The binding map"); flat_forward_light.h, flat_inert_pairs.h and flat_deferred_camera.h name the same pairs.
constexpr StagePair kStagePairs[] = {
    {0xDEF19B035D5EDEDCull, 0xCB95394B50D737D6ull, kCopy, 1},   // the depth copy into f39: the end of the eye's G-buffer
    {0x53211E8C072CD02Eull, 0x7EAC71963E66C5FEull, kMask, 2},   // the shadow mask, first draw
    {0x53211E8C072CD02Eull, 0xB403F48CB35D9739ull, kMask, 4},   // ...and second
};

// How an eye's private G-buffer window closed.
enum End : int { kEndCopy = 0, kEndShell, kEndLight, kEndFrame, kEnds };

bool isHullVs(uint64_t vs) {
    for (uint64_t h : kHullVs)
        if (h == vs) return true;
    return false;
}
bool isLightVs(uint64_t vs) {
    for (uint64_t h : kLightVs)
        if (h == vs) return true;
    return false;
}
bool isTerrainVs(uint64_t vs) { return vs == kTerrainVs || static_cast<uint32_t>(vs >> 32) == kTerrainVsHigh; }

// ---- the shaders (compiled at the first seed with d3dcompiler_47.dll, the gpu_pass_delay_shader.h way) ----------------

// Compiled at run time; tools\hlsl_embedded_check.py compiles them in the build (build.bat gate), one line each:
// hlsl-check: kSeedHlsl hull_layer_seed_cs cs_5_0
// hlsl-check: kCountHlsl hull_layer_count_cs cs_5_0
// hlsl-check: kFilterVsHlsl hull_layer_filter_vs vs_5_0
// hlsl-check: kFilterPsHlsl hull_layer_filter_ps ps_5_0
//
// The seed: the seed draw's instance window names its records; their mean position is the own ship's centre, and every
// pool record within the radius of it is the own ship's. One group; one uint a record in the mask; stats = (own records,
// filtered pixels, seed records read, seed instances).
constexpr char kSeedHlsl[] =
    "cbuffer Seed : register(b0) { uint seedCount; uint recordCount; float radius2; uint unused; };\n"
    "ByteAddressBuffer inst : register(t0);\n"
    "ByteAddressBuffer pool : register(t1);\n"
    "RWByteAddressBuffer ownMask : register(u0);\n"
    "RWByteAddressBuffer stats : register(u1);\n"
    "groupshared float3 gSum[256];\n"
    "groupshared uint gN[256];\n"
    "groupshared uint gOwn;\n"
    "[numthreads(256, 1, 1)] void hull_layer_seed_cs(uint t : SV_GroupIndex) {\n"
    "    float3 s = float3(0, 0, 0);\n"
    "    uint n = 0;\n"
    "    for (uint i = t; i < seedCount; i += 256) {\n"
    "        uint r = inst.Load(i * 8) & 0x7FFFFF;\n"
    "        if (r < recordCount) {\n"
    "            float3 p = asfloat(pool.Load3(r * 336 + 16));\n"
    "            if (all(isfinite(p))) { s += p; n += 1; }\n"
    "        }\n"
    "    }\n"
    "    gSum[t] = s;\n"
    "    gN[t] = n;\n"
    "    if (t == 0) gOwn = 0;\n"
    "    GroupMemoryBarrierWithGroupSync();\n"
    "    [unroll] for (uint k = 128; k > 0; k >>= 1) {\n"
    "        if (t < k) { gSum[t] += gSum[t + k]; gN[t] += gN[t + k]; }\n"
    "        GroupMemoryBarrierWithGroupSync();\n"
    "    }\n"
    "    uint valid = gN[0];\n"
    "    float3 c = gSum[0] / max((float)valid, 1.0);\n"
    "    uint mine = 0;\n"
    "    for (uint q = t; q < recordCount; q += 256) {\n"
    "        float3 p = asfloat(pool.Load3(q * 336 + 16));\n"
    "        float3 d = p - c;\n"
    // An unused pool record sits at exactly (0,0,0); with the own ship near the origin they flooded the mask (dump
    // 214124277001: 936 of 1056, never drawn), and the 400-record guard then held separate mode off all flight.
    "        uint own = (valid > 0 && all(isfinite(p)) && any(p != 0) && dot(d, d) <= radius2) ? 1u : 0u;\n"
    "        ownMask.Store(q * 4, own);\n"
    "        mine += own;\n"
    "    }\n"
    "    InterlockedAdd(gOwn, mine);\n"
    "    GroupMemoryBarrierWithGroupSync();\n"
    "    if (t == 0) stats.Store4(0, uint4(gOwn, 0, valid, seedCount));\n"
    "}\n";

// A pixel is a neighbour's when its private slot is set (2*slot+1 >= 1; the clear is -1) and names a record outside the
// own mask (or outside the pool).
#define HULL_LAYER_PASS_HLSL                                                                       \
    "cbuffer HullLayerPassCb : register(b0) { uint recordCount; float clearDepth; uint width; uint height; };\n" \
    "Texture2D<float2> slots : register(t0);\n"                                                    \
    "ByteAddressBuffer ownMask : register(t1);\n"                                                  \
    "bool foreignSlot(float x) {\n"                                                                \
    "    if (!(x >= 1.0)) return false;\n"                                                         \
    "    uint slot = (uint(x) - 1) >> 1;\n"                                                        \
    "    return slot >= recordCount || ownMask.Load(slot * 4) == 0;\n"                             \
    "}\n"

// The filter: RT0-3 to zero and the depth to the clear value on a neighbour's pixel; every other pixel is discarded.
constexpr char kFilterVsHlsl[] =
    "float4 hull_layer_filter_vs(uint id : SV_VertexID) : SV_Position {\n"
    "    float2 uv = float2((id << 1) & 2, id & 2);\n"
    "    return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);\n"
    "}\n";
constexpr char kFilterPsHlsl[] = HULL_LAYER_PASS_HLSL
    "struct HullLayerFilterOut { float4 c0 : SV_Target0; float4 c1 : SV_Target1; float4 c2 : SV_Target2; float4 c3 : SV_Target3;"
    " float depth : SV_Depth; };\n"
    "HullLayerFilterOut hull_layer_filter_ps(float4 pos : SV_Position) {\n"
    "    if (!foreignSlot(slots.Load(int3(int2(pos.xy), 0)).x)) discard;\n"
    "    HullLayerFilterOut o;\n"
    "    o.c0 = 0; o.c1 = 0; o.c2 = 0; o.c3 = 0;\n"
    "    o.depth = clearDepth;\n"
    "    return o;\n"
    "}\n";
// The count: the filtered pixels, into stats[1].
constexpr char kCountHlsl[] = HULL_LAYER_PASS_HLSL
    "RWByteAddressBuffer stats : register(u0);\n"
    "groupshared uint gCount;\n"
    "[numthreads(8, 8, 1)] void hull_layer_count_cs(uint3 id : SV_DispatchThreadID, uint gi : SV_GroupIndex) {\n"
    "    if (gi == 0) gCount = 0;\n"
    "    GroupMemoryBarrierWithGroupSync();\n"
    "    if (id.x < width && id.y < height && foreignSlot(slots.Load(int3(id.xy, 0)).x)) InterlockedAdd(gCount, 1);\n"
    "    GroupMemoryBarrierWithGroupSync();\n"
    "    if (gi == 0 && gCount != 0) stats.InterlockedAdd(4, gCount);\n"
    "}\n";
#undef HULL_LAYER_PASS_HLSL

// Step 3 (separate), compiled at the first tonemap re-issue; the build compiles them too:
// hlsl-check: kCoverHlsl hull_layer_cover_cs cs_5_0
// hlsl-check: kTaaHlsl hull_layer_taa_cs cs_5_0
//
// The cover: the re-issued tonemap's pixel where the hull is shown -- the private depth set (nearer than its clear, reversed
// Z) and at least as near as the game's main depth, which holds the cockpit shell and no longer the hull -- with alpha 1,
// else 0 (premultiplied); and the private depth wherever the hull is, for the TAA's reprojection. The tonemap is read through
// a UNORM view: the bytes the game's tonemap would have written, the encoding the upscaler's input and output carry.
constexpr char kCoverHlsl[] =
    "cbuffer HullLayerCoverCb : register(b0) { float clearDepth; uint width; uint height; uint coverPad; };\n"
    "Texture2D<float4> tone : register(t0);\n"
    "Texture2D<float> hullDepth : register(t1);\n"
    "Texture2D<float> mainDepth : register(t2);\n"
    "RWTexture2D<float4> cover : register(u0);\n"
    "RWTexture2D<float> coverDepth : register(u1);\n"
    "[numthreads(8, 8, 1)] void hull_layer_cover_cs(uint3 id : SV_DispatchThreadID) {\n"
    "    if (id.x >= width || id.y >= height) return;\n"
    "    float hz = hullDepth.Load(int3(id.xy, 0));\n"
    "    float mz = mainDepth.Load(int3(id.xy, 0));\n"
    "    bool hull = hz > clearDepth;\n"
    "    bool shown = hull && hz >= mz;\n"
    "    cover[id.xy] = shown ? float4(tone.Load(int3(id.xy, 0)).rgb, 1.0) : float4(0.0, 0.0, 0.0, 0.0);\n"
    "    coverDepth[id.xy] = hull ? hz : 0.0;\n"
    "}\n";

// The hull layer's TAA and composite, one thread an output pixel. This frame: the cover's 3x3 around the pixel's unjittered
// position, each render sample q at q - jitter weighted exp(-2.29 d^2) (the temporal pass's own filter), and its range. The
// history: the pixel carried to last frame's view by the head's delta (and its translation at the cover's nearest depth of
// the 3x3, when the pass carries it) -- temporal_shader_source.h's temporalPixelDir, temporalCarry and temporalProjectPixel,
// transcribed -- then fetched bilinearly and clamped to the range. The result (premultiplied colour, coverage in alpha) is the
// next history and goes over the upscaled eye.
constexpr char kTaaHlsl[] =
    "cbuffer HullLayerTaaCb : register(b0) {\n"
    "    float4 tanNow; float4 tanPrev; float4 dRow0; float4 dRow1; float4 dRow2;\n"
    "    float4 headT; float4 jit; float4 depthAB; int4 sizes; int4 origin;\n"
    "};\n"
    "Texture2D<float4> cover : register(t0);\n"
    "Texture2D<float> coverDepth : register(t1);\n"
    "Texture2D<float4> histIn : register(t2);\n"
    "Texture2D<float4> world : register(t3);\n"
    "SamplerState lin : register(s0);\n"
    "RWTexture2D<float4> histOut : register(u0);\n"
    "RWTexture2D<float4> compOut : register(u1);\n"
    // The history read: Catmull-Rom through nine bilinear taps, not one bilinear tap. A bilinear history fetch blurs a
    // little every frame and, at a current weight of 0.1, the blur accumulated: the pilot found the hull's textures soft
    // (2026-11-08 22:20). The 3x3 range clamp after it removes the kernel's overshoot.
    "float4 histCatmullRom(float2 uv) {\n"
    "    float2 tsz; histIn.GetDimensions(tsz.x, tsz.y);\n"
    "    float2 pos = uv * tsz;\n"
    "    float2 c1 = floor(pos - 0.5) + 0.5;\n"
    "    float2 f = pos - c1;\n"
    "    float2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));\n"
    "    float2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);\n"
    "    float2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));\n"
    "    float2 w3 = f * f * (-0.5 + 0.5 * f);\n"
    "    float2 w12 = w1 + w2;\n"
    "    float2 o12 = w2 / w12;\n"
    "    float2 t0 = (c1 - 1.0) / tsz;\n"
    "    float2 t3 = (c1 + 2.0) / tsz;\n"
    "    float2 t12 = (c1 + o12) / tsz;\n"
    "    float4 r = 0.0;\n"
    "    r += histIn.SampleLevel(lin, float2(t0.x, t0.y), 0) * (w0.x * w0.y);\n"
    "    r += histIn.SampleLevel(lin, float2(t12.x, t0.y), 0) * (w12.x * w0.y);\n"
    "    r += histIn.SampleLevel(lin, float2(t3.x, t0.y), 0) * (w3.x * w0.y);\n"
    "    r += histIn.SampleLevel(lin, float2(t0.x, t12.y), 0) * (w0.x * w12.y);\n"
    "    r += histIn.SampleLevel(lin, float2(t12.x, t12.y), 0) * (w12.x * w12.y);\n"
    "    r += histIn.SampleLevel(lin, float2(t3.x, t12.y), 0) * (w3.x * w12.y);\n"
    "    r += histIn.SampleLevel(lin, float2(t0.x, t3.y), 0) * (w0.x * w3.y);\n"
    "    r += histIn.SampleLevel(lin, float2(t12.x, t3.y), 0) * (w12.x * w3.y);\n"
    "    r += histIn.SampleLevel(lin, float2(t3.x, t3.y), 0) * (w3.x * w3.y);\n"
    "    return r;\n"
    "}\n"
    "[numthreads(8, 8, 1)] void hull_layer_taa_cs(uint3 id : SV_DispatchThreadID) {\n"
    "    int2 rsz = sizes.xy;\n"
    "    int2 osz = sizes.zw;\n"
    "    if (id.x >= (uint)osz.x || id.y >= (uint)osz.y) return;\n"
    "    float2 pr = (float2(id.xy) + 0.5) * float2(rsz) / float2(osz) - 0.5;\n"
    "    int2 c = int2(round(pr + jit.xy));\n"
    "    float4 cur = 0.0;\n"
    "    float wsum = 0.0;\n"
    "    float4 lo = 1e9;\n"
    "    float4 hi = -1e9;\n"
    "    float zr = 0.0;\n"
    "    [unroll] for (int dy = -1; dy <= 1; ++dy) {\n"
    "        [unroll] for (int dx = -1; dx <= 1; ++dx) {\n"
    "            int2 q = clamp(c + int2(dx, dy), int2(0, 0), rsz - 1);\n"
    "            float4 s = cover.Load(int3(origin.xy + q, 0));\n"
    "            float2 dd = float2(q) - jit.xy - pr;\n"
    // exp(-5.0 d^2) in render pixels: sigma 0.32 render, about 0.42 output pixels at 1.33x. The temporal pass's
    // exp(-2.29 d^2) (0.62 output pixels) left the hull visibly softer than DLSS's own hull (the pilot, 22:30). The
    // jitter moves the samples every frame and the history (weight 0.9) averages them, so the narrow kernel's
    // per-frame aliasing averages out while its detail stays.
    "            float wt = exp(-5.0 * dot(dd, dd));\n"
    "            cur += s * wt;\n"
    "            wsum += wt;\n"
    "            lo = min(lo, s);\n"
    "            hi = max(hi, s);\n"
    "            zr = max(zr, coverDepth.Load(int3(origin.xy + q, 0)));\n"
    "        }\n"
    "    }\n"
    "    cur /= max(wsum, 1e-6);\n"
    "    float4 res = cur;\n"
    "    if (jit.z != 0.0) {\n"
    "        float3 d;\n"
    "        d.x = tanNow.x + (pr.x + 0.5) / float(rsz.x) * (tanNow.y - tanNow.x);\n"
    "        d.y = tanNow.w - (pr.y + 0.5) / float(rsz.y) * (tanNow.w - tanNow.z);\n"
    "        d.z = -1.0;\n"
    "        float3 dp = float3(dot(dRow0.xyz, d), dot(dRow1.xyz, d), dot(dRow2.xyz, d));\n"
    "        float den = zr - depthAB.x;\n"
    "        if (headT.w != 0.0 && zr > 0.0 && den > 0.0) dp = dp * (depthAB.y / den) + headT.xyz;\n"
    "        if (dp.z < -1e-6) {\n"
    "            float2 pp;\n"
    "            pp.x = (dp.x / -dp.z - tanPrev.x) / (tanPrev.y - tanPrev.x) * float(rsz.x) - 0.5;\n"
    "            pp.y = (tanPrev.w - dp.y / -dp.z) / (tanPrev.w - tanPrev.z) * float(rsz.y) - 0.5;\n"
    "            float2 uv = (pp + 0.5) / float2(rsz);\n"
    "            if (all(uv >= 0.0) && all(uv <= 1.0)) res = lerp(clamp(histCatmullRom(uv), lo, hi), cur, jit.w);\n"
    "        }\n"
    "    }\n"
    "    histOut[id.xy] = res;\n"
    "    float4 w = world[id.xy];\n"
    "    compOut[id.xy] = float4(w.rgb * (1.0 - res.a) + res.rgb, w.a);\n"
    "}\n";

// The sharpen and composite, after the TAA (2026-11-08 22:35): the TAA output measured 0.7 output px softer than one frame
// (dumps 222624596001, 222629772002: 26-29% of the high-frequency energy, no history shift), and the pilot saw the hull
// softer than DLSS's. A cross unsharp mask on the TAA result, clamped to the cross's range (no halo); only where the whole
// cross is hull (alpha 1), so the silhouette's anti-aliased edge stays as the TAA left it. The history is not sharpened.
// hlsl-check: kSharpHlsl hull_layer_sharp_cs cs_5_0
constexpr float kSharpAmount = 0.8f;   // c + amount * (c - mean of the 4 neighbours)
constexpr char kSharpHlsl[] =
    "Texture2D<float4> hist : register(t0);\n"
    "Texture2D<float4> world : register(t1);\n"
    "RWTexture2D<float4> compOut : register(u0);\n"
    "cbuffer HullLayerSharpCb : register(b0) { float4 amount; };\n"
    "[numthreads(8, 8, 1)] void hull_layer_sharp_cs(uint3 id : SV_DispatchThreadID) {\n"
    "    uint ow, oh; hist.GetDimensions(ow, oh);\n"
    "    if (id.x >= ow || id.y >= oh) return;\n"
    "    int2 p = int2(id.xy);\n"
    "    int2 mx = int2(ow, oh) - 1;\n"
    "    float4 c = hist.Load(int3(p, 0));\n"
    "    float4 l = hist.Load(int3(clamp(p + int2(-1, 0), 0, mx), 0));\n"
    "    float4 r = hist.Load(int3(clamp(p + int2(1, 0), 0, mx), 0));\n"
    "    float4 u = hist.Load(int3(clamp(p + int2(0, -1), 0, mx), 0));\n"
    "    float4 d = hist.Load(int3(clamp(p + int2(0, 1), 0, mx), 0));\n"
    "    float4 res = c;\n"
    "    if (min(min(c.a, l.a), min(min(r.a, u.a), d.a)) > 0.999) {\n"
    "        float3 m = (l.rgb + r.rgb + u.rgb + d.rgb) * 0.25;\n"
    "        float3 lo = min(c.rgb, min(min(l.rgb, r.rgb), min(u.rgb, d.rgb)));\n"
    "        float3 hi = max(c.rgb, max(max(l.rgb, r.rgb), max(u.rgb, d.rgb)));\n"
    "        res.rgb = clamp(c.rgb + amount.x * (c.rgb - m), lo, hi);\n"
    "    }\n"
    "    float4 w = world.Load(int3(p, 0));\n"
    "    compOut[p] = float4(w.rgb * (1.0 - res.a) + res.rgb, w.a);\n"
    "}\n";

// A private copy of one game texture: the same desc, default usage, nothing shared. `game` is an identity only.
struct Mirror {
    const void* game = nullptr;
    D3D11_TEXTURE2D_DESC desc{};
    ID3D11Texture2D* priv = nullptr;
    uint32_t lastSeen = 0;   // the layer's frame the game texture was last bound or cleared
};
enum class ViewKind : uint8_t { Rtv, Dsv, Srv };
union ViewDesc {
    D3D11_RENDER_TARGET_VIEW_DESC rtv;
    D3D11_DEPTH_STENCIL_VIEW_DESC dsv;
    D3D11_SHADER_RESOURCE_VIEW_DESC srv;
};
// A private view over a mirror, made with the game's own view desc (so a read-only depth view, an array slice or a
// typed format comes across as the game bound it).
struct ViewEntry {
    const void* game = nullptr;
    ViewKind kind = ViewKind::Rtv;
    ViewDesc desc;
    ID3D11View* priv = nullptr;
};

struct ClearValue {
    const void* res = nullptr;
    float depth = 0.0f;
    uint8_t stencil = 0;
};

struct Readback {
    ID3D11Buffer* staging = nullptr;
    uint32_t frame = 0;
    bool pending = false;
};

// The eye's private G-buffer window: before the seed, between the seed and the eye's depth copy, filtered, or off for
// this eye-frame (no slot target, no shader, a seed refused).
enum class Gb : uint8_t { Before, In, Done, Off };
struct Eye {
    const void* depth = nullptr;   // the game's eye depth texture (identity): the eye's key
    // This frame.
    bool seen = false;
    bool shell = false;            // the cockpit shell was drawn into this depth this frame
    Gb gb = Gb::Before;
    uint32_t hull = 0, noSlotDraws = 0, lightDraws = 0, lightSpan = 0;
    bool lightOpen = false;        // between the shadow mask and the sky
    uint8_t stagesDone = 0;
    const void* hdrGame = nullptr;   // the G-buffer's RT3: the game's HDR (identity)
    const void* slotGame = nullptr;  // engine motion's slot target, the seed's RT6 (identity)
    float clearDepth = 0.0f;
    uint32_t records = 0, seedInstances = 0;
    // The seed's private views (references held from the seed to the filter).
    ID3D11RenderTargetView* privRtv[4] = {};
    ID3D11RenderTargetView* privSlot = nullptr;
    ID3D11DepthStencilView* privDsv = nullptr;
    // GPU objects kept across frames.
    ID3D11Buffer* mask = nullptr;
    ID3D11ShaderResourceView* maskSrv = nullptr;
    ID3D11UnorderedAccessView* maskUav = nullptr;
    uint32_t maskRecords = 0;
    ID3D11Buffer* stats = nullptr;
    ID3D11UnorderedAccessView* statsUav = nullptr;
    Readback ring[kRing];
    ID3D11ShaderResourceView* slotSrv = nullptr;   // over the private slot target
    const void* slotSrvOf = nullptr;
    // The dump's staging copies (the capture frame only).
    ID3D11Texture2D* stageHull = nullptr;
    ID3D11Texture2D* stageGame = nullptr;
    ID3D11Texture2D* stageDepth = nullptr;
    ID3D11Buffer* stageStats = nullptr;
    uint32_t dumpHull = 0, dumpLight = 0;
    // Separate mode. This frame: the main-pass draws given the own discard (hull, decals), and the decals re-issued.
    uint32_t ownDraws = 0, ownDecals = 0, decals = 0;
    // The capture frame only: the game's G-buffer RT0 and eye depth (references from the seed to the frame's end), and
    // their staging copies after the eye's G-buffer.
    ID3D11Texture2D* gameRt0 = nullptr;
    ID3D11Texture2D* gameDepth = nullptr;
    ID3D11Texture2D* stageGbuf0 = nullptr;
    ID3D11Texture2D* stageMainDepth = nullptr;
    // Step 3 (separate). Kept across frames (remade on a size or format change): the tonemap re-issue's target T (R8G8B8A8
    // typeless: an RTV in the game's own view format, a UNORM SRV), the cover C and the private depth beside it Z.
    ID3D11Texture2D* tone = nullptr;
    ID3D11RenderTargetView* toneRtv = nullptr;
    ID3D11ShaderResourceView* toneSrv = nullptr;
    DXGI_FORMAT toneRtvFmt = DXGI_FORMAT_UNKNOWN;
    uint32_t toneW = 0, toneH = 0;
    ID3D11Texture2D* cover = nullptr;
    ID3D11ShaderResourceView* coverSrv = nullptr;
    ID3D11UnorderedAccessView* coverUav = nullptr;
    ID3D11Texture2D* coverZ = nullptr;
    ID3D11ShaderResourceView* coverZSrv = nullptr;
    ID3D11UnorderedAccessView* coverZUav = nullptr;
    // Shader views over the private depth and the game's eye depth, kept while the texture is the same (a view holds its texture).
    ID3D11ShaderResourceView* privZSrv = nullptr;
    const void* privZOf = nullptr;
    ID3D11ShaderResourceView* mainZSrv = nullptr;
    const void* mainZOf = nullptr;
    // This frame: the game's eye depth (a reference from the seed to the frame's end). Across: the layer frame the tonemap
    // re-issue and the composite last ran on, the game's tonemap output then (identity), and whether the composite was applied.
    ID3D11Texture2D* mainDepth = nullptr;
    uint32_t toneFrame = ~0u, compFrame = ~0u;
    const void* toneOut = nullptr;
    bool compApplied = false;
    // The capture frame only: C and the composited output eye.
    ID3D11Texture2D* stageTone = nullptr;
    ID3D11Texture2D* stageComp = nullptr;
    bool dumpApplied = false;
};

// Step 3: the TAA's history and the composited eye, per VR eye (the temporal pass's index), at the upscaler's output size.
struct Comp {
    ID3D11Texture2D* hist[2] = {};
    ID3D11ShaderResourceView* histSrv[2] = {};
    ID3D11UnorderedAccessView* histUav[2] = {};
    ID3D11Texture2D* out = nullptr;
    ID3D11UnorderedAccessView* outUav = nullptr;
    uint32_t outW = 0, outH = 0, renderW = 0, renderH = 0;
    uint32_t read = 0;            // hist[read] holds last frame's
    uint32_t lastFrame = ~0u;
    bool have = false;
};

// Step 3: a GPU time by D3D11 timestamps, read back a few frames later (never a stall); kTimerRing sets in flight.
struct GpuTimer {
    struct Set {
        ID3D11Query* disjoint = nullptr;
        ID3D11Query* t0 = nullptr;
        ID3D11Query* t1 = nullptr;
        bool pending = false;
    };
    Set set[kTimerRing];
    int open = -1;
    double sumMs = 0.0;
    uint32_t samples = 0;
};

// Step 3: why a frame in separate mode kept the own ship in the main pass, and why an eye's composite declined.
enum Fallback : int { kFbFrameGen = 0, kFbOutside, kFbMaskPending, kFbMaskLarge, kFbStreak, kFallbacks };
enum Declined : int { kDcStale = 0, kDcSize, kDcGpu, kDeclines };

struct Window {
    ULONGLONG startMs = 0;
    uint32_t frames = 0, eyeFrames = 0, hullSeen = 0, seeds = 0, noSeed = 0, noSlot = 0, noShader = 0, refused = 0;
    uint32_t reissued = 0, noSlotDraws = 0, clears = 0, filters = 0, readbacks = 0, readbackBusy = 0, terrainFrames = 0;
    uint32_t unknownPairs = 0, autoDumps = 0;
    uint32_t subst[kStages] = {}, noCounterpart[kStages] = {}, ended[kEnds] = {};
    std::vector<uint32_t> own, filtered, hullDraws, lightDraws;
    // Separate mode (the "main pass" line): main-pass draws given the own discard (hull, decals), draws in the window that
    // could not take it (no slot target, no variant), eye-frames whose main pass kept the own ship (no mask: the seed was
    // refused, or had no slot target or shader), and the own decals re-issued into the private layer (and those declined:
    // no slot exported).
    uint32_t ownDraws = 0, ownDecals = 0, noVariant = 0, fallbackFrames = 0, decalsReissued = 0, decalNoSlot = 0;
    // Step 3 (the "composite" line): frames decided with the discard on, and kept by reason; eye-frames composited (the
    // discard on) or made ready (a fallback frame); misses by stage (an eye-frame whose seed failed is counted, not held);
    // composites declined; tonemap re-issues made and declined; history restarts; eyes matched by index, not identity;
    // the largest own mask a large-mask fallback saw.
    uint32_t separateFrames = 0, fallback[kFallbacks] = {};
    uint32_t compositedEyes = 0, readyEyes = 0, failSeed = 0, failLit = 0, failTone = 0, failComposite = 0;
    uint32_t declined[kDeclines] = {};
    uint32_t toned = 0, toneDeclined = 0, historyResets = 0, byIndex = 0, ownMax = 0;
    uint32_t siteAfterResolve = 0, siteUpscalerOut = 0;   // applied composites by where they landed (HullLayerCompositeInputs)
};

// Separate mode: what hullLayerBeforeMainDraw bound over the game's issue, which hullLayerAfterMainDraw puts back.
struct MainPending {
    bool active = false;
    ID3D11PixelShader* ps = nullptr;          // the substitute engine motion had bound (a reference)
    ID3D11ShaderResourceView* srv = nullptr;  // the game's PS t127 (a reference, or null)
};

// The pending re-issue: what Begin bound over, which End puts back.
struct Pending {
    bool active = false;
    int eye = -1;
    int stage = -1;   // -1: a hull draw
    ID3D11RenderTargetView* rtv[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
    ID3D11DepthStencilView* dsv = nullptr;
    ID3D11ShaderResourceView* srv[kSrvSlots] = {};
    bool srvSwapped = false;
    const void* hdrGame = nullptr;   // a lighting draw's RT0 (the game's HDR), for the dump
    const void* toneOut = nullptr;   // the tonemap re-issue (stage kToneStage): the game's tonemap output (identity)
};

// The device objects the passes need, made once at the first seed.
struct Gpu {
    bool tried = false, ok = false;
    ID3D11ComputeShader* seedCs = nullptr;
    ID3D11ComputeShader* countCs = nullptr;
    ID3D11VertexShader* filterVs = nullptr;
    ID3D11PixelShader* filterPs = nullptr;
    ID3D11DepthStencilState* depthAlways = nullptr;
    ID3D11RasterizerState* noCull = nullptr;
    ID3D11Buffer* seedCb = nullptr;
    ID3D11Buffer* passCb = nullptr;
    ID3D11Buffer* inst = nullptr;   // the seed's instance window, kMaxSeedInstances * 8 bytes, raw
    ID3D11ShaderResourceView* instSrv = nullptr;
    ID3D11Buffer* pool = nullptr;   // the pool's copy at the seed, raw
    ID3D11ShaderResourceView* poolSrv = nullptr;
    uint32_t poolBytes = 0;
};

// Step 3's device objects, made once at the first tonemap re-issue (apart from Gpu, so a failure here leaves Steps 1-2 running).
struct CompGpu {
    bool tried = false, ok = false;
    ID3D11ComputeShader* coverCs = nullptr;
    ID3D11ComputeShader* taaCs = nullptr;
    ID3D11ComputeShader* sharpCs = nullptr;
    ID3D11SamplerState* linear = nullptr;
    ID3D11Buffer* coverCb = nullptr;
    ID3D11Buffer* taaCb = nullptr;
    ID3D11Buffer* sharpCb = nullptr;
};

std::string g_mode = "off";
ID3D11Device* g_device = nullptr;   // a reference
std::vector<Mirror> g_mirrors;
std::vector<ViewEntry> g_views;
ClearValue g_clearValues[8];
uint32_t g_clearNext = 0;
Eye g_eyes[2];
int g_inEye = -1;      // the eye whose private G-buffer window is open
int g_stageEye = -1;   // the eye whose screen-space stage ran last this frame
Pending g_pending;
MainPending g_main;
Gpu g_gpu;
Window g_win;
bool g_noCpFrame[kStages] = {};
bool g_terrainFrame = false, g_seededFrame = false;
uint32_t g_frame = 0;
uint32_t g_lastOwn = 0;
bool g_haveOwn = false;
uint64_t g_unknownPairs[kMaxUnknownPairs][2] = {};
uint32_t g_unknownCount = 0;
bool g_saidMirrorCap = false, g_saidViewCap = false, g_saidCreateFail = false, g_saidMsaa = false, g_saidNoSlot = false, g_saidRefused = false,
     g_saidPool = false, g_saidUnknownCap = false, g_saidReadback = false, g_saidNoVariant = false;
// The dump: armed -> the next frame is captured -> written at the boundary after it.
enum class Dump : uint8_t { Idle, Armed, Capturing };
Dump g_dump = Dump::Idle;
wchar_t g_dumpStamp[32] = L"";
uint32_t g_dumpFrame = 0;
// The auto dump (session-wide).
uint32_t g_autoStreak = 0, g_autoDumps = 0;
ULONGLONG g_lastAutoMs = 0;
// Step 3 (separate): this frame's decision (made at the boundary before it), the hold after a miss, and the GPU objects.
bool g_discard = false;
uint32_t g_hold = 0;
Comp g_comp[2];
CompGpu g_cgpu;
GpuTimer g_toneTimer, g_compTimer;
bool g_saidFrameGen = false, g_saidMaskLarge = false, g_saidToneDecline = false, g_saidDecline[kDeclines] = {};

template <class T>
void releaseRef(T*& p) {
    if (p) { p->Release(); p = nullptr; }
}

const void* resourceOf(ID3D11View* view) {
    if (!view) return nullptr;
    ID3D11Resource* res = nullptr;
    view->GetResource(&res);
    if (res) res->Release();   // an identity only: the view keeps the resource alive while it is bound
    return res;
}

void releaseTargets(ID3D11RenderTargetView** rtv, ID3D11DepthStencilView*& dsv) {
    for (UINT i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i) releaseRef(rtv[i]);
    releaseRef(dsv);
}

UINT boundCount(ID3D11RenderTargetView* const* rtv) {
    UINT n = 0;
    for (UINT i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i)
        if (rtv[i]) n = i + 1;
    return n;
}

void releaseStaging(Eye& e) {
    releaseRef(e.stageHull);
    releaseRef(e.stageGame);
    releaseRef(e.stageDepth);
    releaseRef(e.stageStats);
    releaseRef(e.stageGbuf0);
    releaseRef(e.stageMainDepth);
    releaseRef(e.stageTone);
    releaseRef(e.stageComp);
    e.dumpHull = e.dumpLight = 0;
    e.dumpApplied = false;
}

void releaseSeedViews(Eye& e) {
    for (auto*& v : e.privRtv) releaseRef(v);
    releaseRef(e.privSlot);
    releaseRef(e.privDsv);
    releaseRef(e.gameRt0);
    releaseRef(e.gameDepth);
    releaseRef(e.mainDepth);
}

// Step 3: the eye's tonemap target, cover and depth views.
void releaseTone(Eye& e) {
    releaseRef(e.toneRtv);
    releaseRef(e.toneSrv);
    releaseRef(e.tone);
    releaseRef(e.coverSrv);
    releaseRef(e.coverUav);
    releaseRef(e.cover);
    releaseRef(e.coverZSrv);
    releaseRef(e.coverZUav);
    releaseRef(e.coverZ);
    releaseRef(e.privZSrv);
    releaseRef(e.mainZSrv);
    e.privZOf = e.mainZOf = nullptr;
    e.toneRtvFmt = DXGI_FORMAT_UNKNOWN;
    e.toneW = e.toneH = 0;
}

void releaseComp(Comp& c) {
    for (int i = 0; i < 2; ++i) {
        releaseRef(c.histSrv[i]);
        releaseRef(c.histUav[i]);
        releaseRef(c.hist[i]);
    }
    releaseRef(c.outUav);
    releaseRef(c.out);
    c = Comp{};
}

void releaseTimer(GpuTimer& t) {
    for (auto& s : t.set) {
        releaseRef(s.disjoint);
        releaseRef(s.t0);
        releaseRef(s.t1);
        s.pending = false;
    }
    t = GpuTimer{};
}

void releaseEyeGpu(Eye& e) {
    releaseSeedViews(e);
    releaseRef(e.maskSrv);
    releaseRef(e.maskUav);
    releaseRef(e.mask);
    e.maskRecords = 0;
    releaseRef(e.statsUav);
    releaseRef(e.stats);
    for (auto& r : e.ring) {
        releaseRef(r.staging);
        r.pending = false;
    }
    releaseRef(e.slotSrv);
    e.slotSrvOf = nullptr;
    releaseTone(e);
    releaseStaging(e);
}

// The per-frame fields back to the start of a frame (the identity and the GPU objects stay).
void resetFrame(Eye& e) {
    releaseSeedViews(e);
    e.seen = false;
    e.shell = false;
    e.gb = Gb::Before;
    e.hull = e.noSlotDraws = e.lightDraws = e.lightSpan = 0;
    e.lightOpen = false;
    e.stagesDone = 0;
    e.hdrGame = e.slotGame = nullptr;
    e.clearDepth = 0.0f;
    e.records = e.seedInstances = 0;
    e.ownDraws = e.ownDecals = e.decals = 0;
}

void releaseGpu() {
    releaseRef(g_gpu.seedCs);
    releaseRef(g_gpu.countCs);
    releaseRef(g_gpu.filterVs);
    releaseRef(g_gpu.filterPs);
    releaseRef(g_gpu.depthAlways);
    releaseRef(g_gpu.noCull);
    releaseRef(g_gpu.seedCb);
    releaseRef(g_gpu.passCb);
    releaseRef(g_gpu.instSrv);
    releaseRef(g_gpu.inst);
    releaseRef(g_gpu.poolSrv);
    releaseRef(g_gpu.pool);
    g_gpu = Gpu{};
    releaseRef(g_cgpu.coverCs);
    releaseRef(g_cgpu.taaCs);
    releaseRef(g_cgpu.sharpCs);
    releaseRef(g_cgpu.linear);
    releaseRef(g_cgpu.coverCb);
    releaseRef(g_cgpu.taaCb);
    releaseRef(g_cgpu.sharpCb);
    g_cgpu = CompGpu{};
    for (auto& c : g_comp) releaseComp(c);
    releaseTimer(g_toneTimer);
    releaseTimer(g_compTimer);
}

void dropViewsOf(const void* game) {
    for (size_t i = 0; i < g_views.size();) {
        if (g_views[i].game == game) {
            if (g_views[i].priv) g_views[i].priv->Release();
            g_views.erase(g_views.begin() + static_cast<std::ptrdiff_t>(i));
        } else {
            ++i;
        }
    }
}

void releaseAll() {
    for (auto& v : g_views)
        if (v.priv) v.priv->Release();
    g_views.clear();
    for (auto& m : g_mirrors)
        if (m.priv) m.priv->Release();
    g_mirrors.clear();
    for (auto& e : g_eyes) {
        releaseEyeGpu(e);
        e = Eye{};
    }
    releaseGpu();
    g_inEye = g_stageEye = -1;
    if (g_device) { g_device->Release(); g_device = nullptr; }
    g_dump = Dump::Idle;
    g_autoStreak = 0;
    g_haveOwn = false;
    g_discard = false;
    g_hold = 0;
}

bool ensureDevice(ID3D11DeviceContext* ctx) {
    if (!g_device && ctx) ctx->GetDevice(&g_device);
    return g_device != nullptr;
}

Mirror* findMirror(const void* game) {
    for (auto& m : g_mirrors)
        if (m.game == game) {
            m.lastSeen = g_frame;
            return &m;
        }
    return nullptr;
}

void dropMirror(const void* game) {
    dropViewsOf(game);
    for (size_t i = 0; i < g_mirrors.size(); ++i) {
        if (g_mirrors[i].game == game) {
            if (g_mirrors[i].priv) g_mirrors[i].priv->Release();
            g_mirrors.erase(g_mirrors.begin() + static_cast<std::ptrdiff_t>(i));
            return;
        }
    }
}

Mirror* makeMirror(const void* game, const D3D11_TEXTURE2D_DESC& d) {
    if (g_mirrors.size() >= kMaxMirrors) {
        if (!g_saidMirrorCap) {
            g_saidMirrorCap = true;
            Log::get().note("hull layer: %u private copies held, the cap; a %ux%u format %u target gets none (said once)",
                            static_cast<unsigned>(g_mirrors.size()), d.Width, d.Height, static_cast<unsigned>(d.Format));
        }
        return nullptr;
    }
    if (d.SampleDesc.Count != 1) {
        if (!g_saidMsaa) {
            g_saidMsaa = true;
            Log::get().note("hull layer: a %ux%u format %u target is multisampled (%u); no private copy is made (said once)", d.Width, d.Height,
                            static_cast<unsigned>(d.Format), d.SampleDesc.Count);
        }
        return nullptr;
    }
    D3D11_TEXTURE2D_DESC pd = d;
    pd.Usage = D3D11_USAGE_DEFAULT;
    pd.CPUAccessFlags = 0;
    pd.MiscFlags &= ~static_cast<UINT>(D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX | D3D11_RESOURCE_MISC_GDI_COMPATIBLE |
                                       D3D11_RESOURCE_MISC_SHARED_NTHANDLE);
    ID3D11Texture2D* priv = nullptr;
    const HRESULT hr = g_device->CreateTexture2D(&pd, nullptr, &priv);
    if (FAILED(hr) || !priv) {
        if (!g_saidCreateFail) {
            g_saidCreateFail = true;
            Log::get().note("hull layer: the private copy of a %ux%u format %u target (bind 0x%X) could not be made (hr 0x%08lX; said once)", d.Width,
                            d.Height, static_cast<unsigned>(d.Format), d.BindFlags, static_cast<unsigned long>(hr));
        }
        return nullptr;
    }
    Mirror m;
    m.game = game;
    m.desc = d;
    m.priv = priv;
    m.lastSeen = g_frame;
    g_mirrors.push_back(m);
    Log::get().note("hull layer: private copy made of a %ux%u format %u target (bind 0x%X, %u held)", d.Width, d.Height,
                    static_cast<unsigned>(d.Format), d.BindFlags, static_cast<unsigned>(g_mirrors.size()));
    return &g_mirrors.back();
}

// The private counterpart of a game view: the mirror of its texture (made when `make` and missing), and a view over the
// mirror with the game view's own desc. Null when the view is not over a 2D texture, has no mirror and none is to be
// made, or a create failed. The view is the layer's (g_views holds the reference).
ID3D11View* privateView(ViewKind kind, ID3D11View* gameView, bool make) {
    if (!gameView || !g_device) return nullptr;
    ID3D11Resource* res = nullptr;
    gameView->GetResource(&res);
    if (!res) return nullptr;
    D3D11_RESOURCE_DIMENSION dim = D3D11_RESOURCE_DIMENSION_UNKNOWN;
    res->GetType(&dim);
    if (dim != D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
        res->Release();
        return nullptr;
    }
    D3D11_TEXTURE2D_DESC d{};
    static_cast<ID3D11Texture2D*>(res)->GetDesc(&d);
    const void* game = res;
    res->Release();
    Mirror* m = findMirror(game);
    if (m && std::memcmp(&m->desc, &d, sizeof d) != 0) {
        // The game's texture at this address is not the one mirrored (freed and remade): the old copy goes.
        dropMirror(game);
        m = nullptr;
    }
    if (!m && make) m = makeMirror(game, d);
    if (!m) return nullptr;
    ViewDesc vd;
    std::memset(&vd, 0, sizeof vd);
    switch (kind) {
    case ViewKind::Rtv: static_cast<ID3D11RenderTargetView*>(gameView)->GetDesc(&vd.rtv); break;
    case ViewKind::Dsv: static_cast<ID3D11DepthStencilView*>(gameView)->GetDesc(&vd.dsv); break;
    case ViewKind::Srv: static_cast<ID3D11ShaderResourceView*>(gameView)->GetDesc(&vd.srv); break;
    }
    for (auto& v : g_views)
        if (v.game == game && v.kind == kind && std::memcmp(&v.desc, &vd, sizeof vd) == 0) return v.priv;
    if (g_views.size() >= kMaxViews) {
        if (!g_saidViewCap) {
            g_saidViewCap = true;
            Log::get().note("hull layer: %u private views held, the cap; no more are made (said once)", static_cast<unsigned>(g_views.size()));
        }
        return nullptr;
    }
    ID3D11View* out = nullptr;
    HRESULT hr = E_FAIL;
    switch (kind) {
    case ViewKind::Rtv: hr = g_device->CreateRenderTargetView(m->priv, &vd.rtv, reinterpret_cast<ID3D11RenderTargetView**>(&out)); break;
    case ViewKind::Dsv: hr = g_device->CreateDepthStencilView(m->priv, &vd.dsv, reinterpret_cast<ID3D11DepthStencilView**>(&out)); break;
    case ViewKind::Srv: hr = g_device->CreateShaderResourceView(m->priv, &vd.srv, reinterpret_cast<ID3D11ShaderResourceView**>(&out)); break;
    }
    if (FAILED(hr) || !out) {
        if (!g_saidCreateFail) {
            g_saidCreateFail = true;
            Log::get().note("hull layer: a private view (kind %d) over a %ux%u format %u copy could not be made (hr 0x%08lX; said once)",
                            static_cast<int>(kind), d.Width, d.Height, static_cast<unsigned>(d.Format), static_cast<unsigned long>(hr));
        }
        return nullptr;
    }
    ViewEntry e;
    e.game = game;
    e.kind = kind;
    e.desc = vd;
    e.priv = out;
    g_views.push_back(e);
    return out;
}

// Mirrors whose game texture was not seen for kEvictFrames go (a re-created eye depth leaves its old targets behind).
void evictStale() {
    for (size_t i = 0; i < g_mirrors.size();) {
        Mirror& m = g_mirrors[i];
        if (g_frame - m.lastSeen > kEvictFrames) {
            const D3D11_TEXTURE2D_DESC d = m.desc;
            dropMirror(m.game);   // erases index i
            Log::get().note("hull layer: private copy of a %ux%u format %u target dropped, its game target unseen for %u frames (%u held)", d.Width,
                            d.Height, static_cast<unsigned>(d.Format), kEvictFrames, static_cast<unsigned>(g_mirrors.size()));
        } else {
            ++i;
        }
    }
}

// The eye whose depth texture this is; with `assign`, a slot is given to a depth not seen before (a slot not seen this
// frame is reused; its GPU objects stay, they are not tied to the depth). -1 when none.
int eyeFor(const void* depth, bool assign) {
    if (!depth) return -1;
    for (int i = 0; i < 2; ++i)
        if (g_eyes[i].depth == depth) {
            g_eyes[i].seen = true;
            return i;
        }
    if (!assign) return -1;
    for (int i = 0; i < 2; ++i) {
        if (!g_eyes[i].depth || !g_eyes[i].seen) {
            releaseStaging(g_eyes[i]);
            resetFrame(g_eyes[i]);
            g_eyes[i].depth = depth;
            g_eyes[i].seen = true;
            return i;
        }
    }
    return -1;
}

float clearDepthOf(const void* res, uint8_t* stencil) {
    for (const auto& c : g_clearValues)
        if (c.res && c.res == res) {
            *stencil = c.stencil;
            return c.depth;
        }
    *stencil = 0;
    return 0.0f;   // reversed Z, GEQUAL: the census's DCL lines clear the eye depth to 0
}

// A staging copy of src: made on the first call (into `stage`), copied again on every later one, so the last copy wins.
void copyToStaging(ID3D11DeviceContext* ctx, ID3D11Texture2D* src, ID3D11Texture2D*& stage) {
    if (!src || !g_device) return;
    D3D11_TEXTURE2D_DESC d{};
    src->GetDesc(&d);
    if (d.SampleDesc.Count != 1) return;
    if (stage) {
        D3D11_TEXTURE2D_DESC s{};
        stage->GetDesc(&s);
        if (s.Width != d.Width || s.Height != d.Height || s.Format != d.Format) return;
    } else {
        d.Usage = D3D11_USAGE_STAGING;
        d.BindFlags = 0;
        d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        d.MiscFlags = 0;
        if (FAILED(g_device->CreateTexture2D(&d, nullptr, &stage)) || !stage) {
            stage = nullptr;
            return;
        }
    }
    vScreenCopyResourceRaw(ctx, stage, src);
}

ID3D11Buffer* makeStagingBuffer(uint32_t bytes) {
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = bytes;
    bd.Usage = D3D11_USAGE_STAGING;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Buffer* b = nullptr;
    if (FAILED(g_device->CreateBuffer(&bd, nullptr, &b))) return nullptr;
    return b;
}

// A default-usage raw buffer with an SRV and, when `uav`, a UAV over it.
bool makeRawBuffer(uint32_t bytes, bool uav, ID3D11Buffer** buf, ID3D11ShaderResourceView** srv, ID3D11UnorderedAccessView** uavOut) {
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = (bytes + 15u) & ~15u;
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_SHADER_RESOURCE | (uav ? D3D11_BIND_UNORDERED_ACCESS : 0u);
    bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
    if (FAILED(g_device->CreateBuffer(&bd, nullptr, buf)) || !*buf) return false;
    if (srv) {
        D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format = DXGI_FORMAT_R32_TYPELESS;
        sd.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
        sd.BufferEx.FirstElement = 0;
        sd.BufferEx.NumElements = bd.ByteWidth / 4;
        sd.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
        if (FAILED(g_device->CreateShaderResourceView(*buf, &sd, srv)) || !*srv) return false;
    }
    if (uav && uavOut) {
        D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
        ud.Format = DXGI_FORMAT_R32_TYPELESS;
        ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        ud.Buffer.FirstElement = 0;
        ud.Buffer.NumElements = bd.ByteWidth / 4;
        ud.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
        if (FAILED(g_device->CreateUnorderedAccessView(*buf, &ud, uavOut)) || !*uavOut) return false;
    }
    return true;
}

ID3D11Buffer* makeConstantBuffer(UINT bytes = 16) {
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = bytes;
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    ID3D11Buffer* b = nullptr;
    if (FAILED(g_device->CreateBuffer(&bd, nullptr, &b))) return nullptr;
    return b;
}

// d3dcompiler_47.dll's D3DCompile, loaded once (gpu_pass_profile.cpp's way). Raw types: the macro and include arguments are null.
typedef HRESULT(WINAPI* PfnD3DCompile)(LPCVOID, SIZE_T, LPCSTR, const void*, void*, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);
bool compileShader(const char* src, size_t len, const char* entry, const char* target, ID3DBlob** code, std::string* why) {
    static PfnD3DCompile compile = nullptr;
    if (!compile) {
        HMODULE mod = LoadLibraryW(L"d3dcompiler_47.dll");
        if (mod) compile = reinterpret_cast<PfnD3DCompile>(GetProcAddress(mod, "D3DCompile"));
        if (!compile) {
            *why = "d3dcompiler_47.dll or its D3DCompile is missing";
            return false;
        }
    }
    ID3DBlob* errors = nullptr;
    const HRESULT hr = compile(src, len, entry, nullptr, nullptr, entry, target, 0x8000u /* D3DCOMPILE_OPTIMIZATION_LEVEL3 */, 0, code, &errors);
    if (errors) {
        why->assign(static_cast<const char*>(errors->GetBufferPointer()), std::min<size_t>(errors->GetBufferSize(), 300));
        errors->Release();
    }
    if (FAILED(hr) || !*code) {
        if (why->empty()) *why = "D3DCompile failed";
        return false;
    }
    return true;
}

// The shaders, states and shared buffers, once. False (and said once) when any is missing: the layer seeds nothing.
bool ensureGpu() {
    if (g_gpu.tried) return g_gpu.ok;
    g_gpu.tried = true;
    struct Item {
        const char* src;
        size_t len;
        const char* entry;
        const char* target;
    };
    const Item items[4] = {{kSeedHlsl, sizeof(kSeedHlsl) - 1, "hull_layer_seed_cs", "cs_5_0"},
                           {kCountHlsl, sizeof(kCountHlsl) - 1, "hull_layer_count_cs", "cs_5_0"},
                           {kFilterVsHlsl, sizeof(kFilterVsHlsl) - 1, "hull_layer_filter_vs", "vs_5_0"},
                           {kFilterPsHlsl, sizeof(kFilterPsHlsl) - 1, "hull_layer_filter_ps", "ps_5_0"}};
    for (int i = 0; i < 4; ++i) {
        ID3DBlob* code = nullptr;
        std::string why;
        if (!compileShader(items[i].src, items[i].len, items[i].entry, items[i].target, &code, &why)) {
            Log::get().note("hull layer: the %s shader did not compile (%s); the layer seeds nothing this session", items[i].entry, why.c_str());
            if (code) code->Release();
            return false;
        }
        HRESULT hr = E_FAIL;
        switch (i) {
        case 0: hr = g_device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &g_gpu.seedCs); break;
        case 1: hr = g_device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &g_gpu.countCs); break;
        case 2: hr = g_device->CreateVertexShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &g_gpu.filterVs); break;
        case 3: hr = g_device->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &g_gpu.filterPs); break;
        }
        code->Release();
        if (FAILED(hr)) {
            Log::get().note("hull layer: the %s shader compiled but could not be created (hr 0x%08lX); the layer seeds nothing this session", items[i].entry,
                            static_cast<unsigned long>(hr));
            return false;
        }
    }
    D3D11_DEPTH_STENCIL_DESC dd{};
    dd.DepthEnable = TRUE;
    dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    dd.DepthFunc = D3D11_COMPARISON_ALWAYS;
    dd.StencilEnable = FALSE;
    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    g_gpu.seedCb = makeConstantBuffer();
    g_gpu.passCb = makeConstantBuffer();
    if (FAILED(g_device->CreateDepthStencilState(&dd, &g_gpu.depthAlways)) || FAILED(g_device->CreateRasterizerState(&rd, &g_gpu.noCull)) ||
        !g_gpu.seedCb || !g_gpu.passCb || !makeRawBuffer(kMaxSeedInstances * 8, false, &g_gpu.inst, &g_gpu.instSrv, nullptr)) {
        Log::get().note("hull layer: the passes' states or buffers could not be made; the layer seeds nothing this session");
        return false;
    }
    Log::get().note("hull layer: the seed, filter and count shaders compiled and made");
    g_gpu.ok = true;
    return true;
}

// The compute stage's slots the layer uses (t0-1, u0-1, b0, the shader), taken and put back.
struct CsSave {
    ID3D11ComputeShader* cs = nullptr;
    ID3D11ShaderResourceView* srv[2] = {};
    ID3D11UnorderedAccessView* uav[2] = {};
    ID3D11Buffer* cb = nullptr;
    void capture(ID3D11DeviceContext* c) {
        c->CSGetShader(&cs, nullptr, nullptr);
        c->CSGetShaderResources(0, 2, srv);
        c->CSGetUnorderedAccessViews(0, 2, uav);
        c->CSGetConstantBuffers(0, 1, &cb);
    }
    void restore(ID3D11DeviceContext* c) {
        static const UINT keep[2] = {~0u, ~0u};   // -1: an append buffer's counter stays as it is
        ID3D11UnorderedAccessView* none[2] = {};
        c->CSSetUnorderedAccessViews(0, 2, none, keep);   // the layer's UAVs off before the game's SRVs come back
        c->CSSetShader(cs, nullptr, 0);
        c->CSSetShaderResources(0, 2, srv);
        c->CSSetUnorderedAccessViews(0, 2, uav, keep);
        c->CSSetConstantBuffers(0, 1, &cb);
        releaseRef(cs);
        for (auto*& s : srv) releaseRef(s);
        for (auto*& u : uav) releaseRef(u);
        releaseRef(cb);
    }
};

// The graphics state the filter pass sets, taken and put back (desktop_window.cpp SavedState's shape, with the raw setters
// where vscreen has one so the binding shadow keeps describing the game's bindings).
struct GfxSave {
    ID3D11RenderTargetView* rtv[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
    ID3D11DepthStencilView* dsv = nullptr;
    ID3D11BlendState* blend = nullptr;
    FLOAT blendFactor[4] = {};
    UINT sampleMask = 0xFFFFFFFFu;
    ID3D11DepthStencilState* depth = nullptr;
    UINT stencilRef = 0;
    ID3D11RasterizerState* raster = nullptr;
    UINT viewportCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    D3D11_VIEWPORT viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {};
    ID3D11InputLayout* layout = nullptr;
    D3D11_PRIMITIVE_TOPOLOGY topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    ID3D11VertexShader* vs = nullptr;
    ID3D11HullShader* hs = nullptr;
    ID3D11DomainShader* ds = nullptr;
    ID3D11GeometryShader* gs = nullptr;
    ID3D11PixelShader* ps = nullptr;
    ID3D11ShaderResourceView* psSrv[2] = {};
    ID3D11Buffer* psCb = nullptr;
    void capture(ID3D11DeviceContext* c) {
        c->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtv, &dsv);
        c->OMGetBlendState(&blend, blendFactor, &sampleMask);
        c->OMGetDepthStencilState(&depth, &stencilRef);
        c->RSGetState(&raster);
        c->RSGetViewports(&viewportCount, viewports);
        c->IAGetInputLayout(&layout);
        c->IAGetPrimitiveTopology(&topology);
        c->VSGetShader(&vs, nullptr, nullptr);
        c->HSGetShader(&hs, nullptr, nullptr);
        c->DSGetShader(&ds, nullptr, nullptr);
        c->GSGetShader(&gs, nullptr, nullptr);
        c->PSGetShader(&ps, nullptr, nullptr);
        c->PSGetShaderResources(0, 2, psSrv);
        c->PSGetConstantBuffers(0, 1, &psCb);
    }
    void restore(ID3D11DeviceContext* c) {
        vScreenSetRenderTargetsRaw(c, boundCount(rtv), rtv, dsv);
        vScreenOMSetBlendStateRaw(c, blend, blendFactor, sampleMask);
        vScreenOMSetDepthStencilStateRaw(c, depth, stencilRef);
        c->RSSetState(raster);
        vScreenRSSetViewportsRaw(c, viewportCount, viewportCount ? viewports : nullptr);
        c->IASetInputLayout(layout);
        c->IASetPrimitiveTopology(topology);
        vScreenVSSetShaderRaw(c, vs, nullptr, 0);
        c->HSSetShader(hs, nullptr, 0);
        c->DSSetShader(ds, nullptr, 0);
        c->GSSetShader(gs, nullptr, 0);
        vScreenPSSetShaderRaw(c, ps, nullptr, 0);
        vScreenPSSetShaderResourcesRaw(c, 0, 2, psSrv);
        vScreenPSSetConstantBuffersRaw(c, 0, 1, &psCb);
        releaseTargets(rtv, dsv);
        releaseRef(blend);
        releaseRef(depth);
        releaseRef(raster);
        releaseRef(layout);
        releaseRef(vs);
        releaseRef(hs);
        releaseRef(ds);
        releaseRef(gs);
        releaseRef(ps);
        for (auto*& s : psSrv) releaseRef(s);
        releaseRef(psCb);
    }
};

// ---- the seed --------------------------------------------------------------------------------------------------------

void sayRefused(const char* why) {
    if (g_saidRefused) return;
    g_saidRefused = true;
    Log::get().note("hull layer: a seed was refused: %s (said once)", why);
}

// The bytes of one buffer element a view counts in (for its FirstElement): the structure stride, or the format's size.
uint32_t elementBytes(const D3D11_BUFFER_DESC& bd, DXGI_FORMAT f, bool raw) {
    if (bd.MiscFlags & D3D11_RESOURCE_MISC_BUFFER_STRUCTURED) return bd.StructureByteStride;
    if (raw) return 4;
    switch (f) {
    case DXGI_FORMAT_R32_TYPELESS: case DXGI_FORMAT_R32_UINT: case DXGI_FORMAT_R32_SINT: case DXGI_FORMAT_R32_FLOAT: return 4;
    case DXGI_FORMAT_R32G32_TYPELESS: case DXGI_FORMAT_R32G32_UINT: case DXGI_FORMAT_R32G32_FLOAT: return 8;
    case DXGI_FORMAT_R32G32B32A32_TYPELESS: case DXGI_FORMAT_R32G32B32A32_UINT: case DXGI_FORMAT_R32G32B32A32_FLOAT: return 16;
    default: return 0;
    }
}

// At the seed draw, with its bindings: the instance window and the pool copied, and the seed dispatch writes the eye's own
// mask and stats. False with the reason when it cannot run; nothing is left bound either way.
bool seedOwnMask(ID3D11DeviceContext* ctx, Eye& e, UINT instances, UINT startInstance, const char** why) {
    // The instance stream: the stride-8 vertex buffer among slots 0-3 (object_probe.cpp ledgerLearn: record index first).
    ID3D11Buffer* vbs[4] = {};
    UINT strides[4] = {}, offsets[4] = {};
    ctx->IAGetVertexBuffers(0, 4, vbs, strides, offsets);
    int pick = -1;
    for (int i = 0; i < 4 && pick < 0; ++i)
        if (vbs[i] && strides[i] == 8) pick = i;
    ID3D11Buffer* instBuf = pick >= 0 ? vbs[pick] : nullptr;
    const UINT instOffset = pick >= 0 ? offsets[pick] : 0;
    for (int i = 0; i < 4; ++i)
        if (i != pick) releaseRef(vbs[i]);
    // The pool at VS t33.
    ID3D11ShaderResourceView* poolSrv = nullptr;
    ctx->VSGetShaderResources(kPoolSlot, 1, &poolSrv);
    ID3D11Buffer* poolBuf = nullptr;
    if (poolSrv) {
        ID3D11Resource* res = nullptr;
        poolSrv->GetResource(&res);
        if (res) {
            res->QueryInterface(__uuidof(ID3D11Buffer), reinterpret_cast<void**>(&poolBuf));
            res->Release();
        }
    }
    const auto fail = [&](const char* reason) {
        *why = reason;
        releaseRef(instBuf);
        releaseRef(poolBuf);
        releaseRef(poolSrv);
        return false;
    };
    if (!instBuf) return fail("no instance stream (no stride-8 vertex buffer in slots 0-3)");
    if (!poolBuf) return fail("no pool buffer at VS t33");
    D3D11_BUFFER_DESC ib{}, pb{};
    instBuf->GetDesc(&ib);
    poolBuf->GetDesc(&pb);
    D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
    poolSrv->GetDesc(&sd);
    uint32_t first = 0, count = 0;
    bool raw = false;
    if (sd.ViewDimension == D3D11_SRV_DIMENSION_BUFFER) {
        first = sd.Buffer.FirstElement;
        count = sd.Buffer.NumElements;
    } else if (sd.ViewDimension == D3D11_SRV_DIMENSION_BUFFEREX) {
        first = sd.BufferEx.FirstElement;
        count = sd.BufferEx.NumElements;
        raw = (sd.BufferEx.Flags & D3D11_BUFFEREX_SRV_FLAG_RAW) != 0;
    } else {
        return fail("VS t33 is not a buffer view");
    }
    const uint32_t elem = elementBytes(pb, sd.Format, raw);
    if (elem == 0 && first != 0) return fail("VS t33's view starts past element 0 in a format whose size is not known");
    const uint64_t base = static_cast<uint64_t>(first) * elem;
    uint64_t len = elem ? static_cast<uint64_t>(count) * elem : pb.ByteWidth - base;
    if (base >= pb.ByteWidth) return fail("VS t33's view starts past its buffer");
    len = std::min<uint64_t>(len, pb.ByteWidth - base);
    const uint32_t records = static_cast<uint32_t>(std::min<uint64_t>(len / kRecordBytes, kMaxRecords));
    if (records == 0) return fail("the pool holds no whole 336-byte record");
    const uint32_t poolBytes = records * kRecordBytes;
    const uint32_t seeds = std::min<uint32_t>(instances, kMaxSeedInstances);
    const uint64_t instBegin = static_cast<uint64_t>(instOffset) + static_cast<uint64_t>(startInstance) * 8u;
    const uint64_t instEnd = instBegin + static_cast<uint64_t>(seeds) * 8u;
    if (seeds == 0 || instEnd > ib.ByteWidth) return fail("the seed's instance window runs past the instance stream");
    if (!g_saidPool) {
        g_saidPool = true;
        Log::get().note("hull layer: the seed reads the pool at VS t33 (%u bytes, view dimension %d, format %u, first element %u, %u elements of %u bytes: "
                        "%u records of 336) and the instance stream at vertex slot %d (%u bytes, offset %u); said once",
                        pb.ByteWidth, static_cast<int>(sd.ViewDimension), static_cast<unsigned>(sd.Format), first, count, elem, records, pick,
                        ib.ByteWidth, instOffset);
    }
    // The layer's copies: the pool (made at its size), the eye's mask (one uint a record) and stats.
    if (g_gpu.poolBytes < poolBytes) {
        releaseRef(g_gpu.poolSrv);
        releaseRef(g_gpu.pool);
        g_gpu.poolBytes = 0;
        if (!makeRawBuffer(poolBytes, false, &g_gpu.pool, &g_gpu.poolSrv, nullptr)) return fail("the pool's copy could not be made");
        g_gpu.poolBytes = poolBytes;
    }
    if (e.maskRecords < records) {
        releaseRef(e.maskSrv);
        releaseRef(e.maskUav);
        releaseRef(e.mask);
        e.maskRecords = 0;
        if (!makeRawBuffer(records * 4u, true, &e.mask, &e.maskSrv, &e.maskUav)) return fail("the own mask could not be made");
        e.maskRecords = records;
    }
    if (!e.statsUav) {
        releaseRef(e.stats);
        if (!makeRawBuffer(16, true, &e.stats, nullptr, &e.statsUav)) return fail("the stats buffer could not be made");
    }
    D3D11_BOX box{};
    box.left = static_cast<UINT>(base);
    box.right = static_cast<UINT>(base + poolBytes);
    box.top = 0;
    box.bottom = 1;
    box.front = 0;
    box.back = 1;
    vScreenCopySubresourceRegionRaw(ctx, g_gpu.pool, 0, poolBuf, &box);
    box.left = static_cast<UINT>(instBegin);
    box.right = static_cast<UINT>(instEnd);
    vScreenCopySubresourceRegionRaw(ctx, g_gpu.inst, 0, instBuf, &box);
    float radius2 = kOwnRadius * kOwnRadius;
    uint32_t cb[4] = {seeds, records, 0u, 0u};
    std::memcpy(&cb[2], &radius2, sizeof radius2);
    vScreenUpdateSubresourceRaw(ctx, g_gpu.seedCb, 0, nullptr, cb, 0, 0);
    CsSave save;
    save.capture(ctx);
    ID3D11ShaderResourceView* srvs[2] = {g_gpu.instSrv, g_gpu.poolSrv};
    ID3D11UnorderedAccessView* uavs[2] = {e.maskUav, e.statsUav};
    static const UINT keep[2] = {~0u, ~0u};
    ctx->CSSetShader(g_gpu.seedCs, nullptr, 0);
    ctx->CSSetShaderResources(0, 2, srvs);
    ctx->CSSetUnorderedAccessViews(0, 2, uavs, keep);
    ctx->CSSetConstantBuffers(0, 1, &g_gpu.seedCb);
    ctx->Dispatch(1, 1, 1);
    save.restore(ctx);
    e.records = records;
    e.seedInstances = seeds;
    releaseRef(instBuf);
    releaseRef(poolBuf);
    releaseRef(poolSrv);
    return true;
}

// ---- the private G-buffer --------------------------------------------------------------------------------------------

void noteShell(ID3D11DeviceContext* ctx);
void finishGbuffer(ID3D11DeviceContext* ctx, int ei, End why);

// The seed: eye `ei`'s first hull-family G-buffer draw after the shell, with that draw's targets (`rtv`, `dsv`: the
// caller's references) and instance window bound. The own mask is dispatched, the private G-buffer made and cleared, and
// the eye's window opened (Gb::In); false leaves the eye Off for this frame (no slot target, no shader, refused).
// Dump mode runs it right after the seed draw's issue, separate mode right before it (hullLayerBeforeMainDraw).
bool seedEye(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* const* rtv, ID3D11DepthStencilView* dsv, int ei, UINT instances, UINT startInstance) {
    Eye& e = g_eyes[ei];
    const void* depthRes = resourceOf(dsv);
    {
        if (g_inEye >= 0 && g_inEye != ei) finishGbuffer(ctx, g_inEye, kEndShell);
        e.gb = Gb::Off;   // until the seed has run
        bool slotOk = false;
        if (engineVelocityLiveNow() && rtv[kSlotTarget]) {
            ID3D11Resource* res = nullptr;
            rtv[kSlotTarget]->GetResource(&res);
            D3D11_RESOURCE_DIMENSION dim = D3D11_RESOURCE_DIMENSION_UNKNOWN;
            if (res) res->GetType(&dim);
            if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
                D3D11_TEXTURE2D_DESC d{};
                static_cast<ID3D11Texture2D*>(res)->GetDesc(&d);
                slotOk = d.Format == DXGI_FORMAT_R32G32_FLOAT;
            }
            if (res) res->Release();
        }
        if (!slotOk) {
            ++g_win.noSlot;
            if (!g_saidNoSlot) {
                g_saidNoSlot = true;
                Log::get().note("hull layer: no-slot-target: the seed draw has no R32G32_FLOAT slot target at render target 6 (engine motion %s); the "
                                "layer does nothing on such an eye-frame (said once)",
                                engineVelocityLiveNow() ? "live" : "not live: it comes with fix.temporal_aa");
            }
            return false;
        }
        if (!ensureDevice(ctx) || !ensureGpu()) {
            ++g_win.noShader;
            return false;
        }
        ID3D11View* priv[6] = {};
        for (int i = 0; i < 4; ++i) priv[i] = privateView(ViewKind::Rtv, rtv[i], true);
        priv[4] = privateView(ViewKind::Rtv, rtv[kSlotTarget], true);
        priv[5] = privateView(ViewKind::Dsv, dsv, true);
        for (auto* p : priv)
            if (!p) {
                ++g_win.refused;
                sayRefused("a private target could not be made");
                return false;
            }
        const char* why = nullptr;
        if (!seedOwnMask(ctx, e, instances, startInstance, &why)) {
            ++g_win.refused;
            sayRefused(why ? why : "?");
            return false;
        }
        // The private G-buffer, cleared: colour to 0, the slot to -1 (no slot), depth and stencil to the game's.
        static const float zero[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        static const float noSlot[4] = {-1.0f, 0.0f, 0.0f, 0.0f};
        for (int i = 0; i < 4; ++i) {
            e.privRtv[i] = static_cast<ID3D11RenderTargetView*>(priv[i]);
            e.privRtv[i]->AddRef();
            vScreenClearRenderTargetViewRaw(ctx, e.privRtv[i], zero);
        }
        e.privSlot = static_cast<ID3D11RenderTargetView*>(priv[4]);
        e.privSlot->AddRef();
        vScreenClearRenderTargetViewRaw(ctx, e.privSlot, noSlot);
        e.privDsv = static_cast<ID3D11DepthStencilView*>(priv[5]);
        e.privDsv->AddRef();
        uint8_t stencil = 0;
        e.clearDepth = clearDepthOf(depthRes, &stencil);
        ctx->ClearDepthStencilView(e.privDsv, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, e.clearDepth, stencil);
        ++g_win.clears;
        e.hdrGame = resourceOf(rtv[3]);
        e.slotGame = resourceOf(rtv[kSlotTarget]);
        // Step 3: the game's eye depth, which the cover pass reads after the tonemap (the cockpit in front of the hull).
        if (detail::g_hullLayerSeparate) {
            ID3D11Resource* res = nullptr;
            dsv->GetResource(&res);
            if (res && FAILED(res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&e.mainDepth)))) e.mainDepth = nullptr;
            releaseRef(res);
        }
        // Separate mode's dump: the game's G-buffer RT0 and eye depth, copied after the eye's G-buffer (finishGbuffer).
        if (detail::g_hullLayerSeparate && g_dump == Dump::Capturing) {
            ID3D11Resource* res = nullptr;
            rtv[0]->GetResource(&res);
            if (res && FAILED(res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&e.gameRt0)))) e.gameRt0 = nullptr;
            releaseRef(res);
            dsv->GetResource(&res);
            if (res && FAILED(res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&e.gameDepth)))) e.gameDepth = nullptr;
            releaseRef(res);
        }
        e.gb = Gb::In;
        g_inEye = ei;
        g_seededFrame = true;
        ++g_win.seeds;
    }
    return true;
}

// After the game's issue of a hull-family G-buffer draw (or, separate mode, a hull decal): the private re-issue's
// bindings. Dump mode seeds here; separate mode seeded before the seed draw's issue, and re-issues the decals too.
bool beginHullDraw(ID3D11DeviceContext* ctx, UINT instances, UINT startInstance, bool decal) {
    ID3D11RenderTargetView* rtv[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
    ID3D11DepthStencilView* dsv = nullptr;
    ctx->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtv, &dsv);
    const auto decline = [&] {
        releaseTargets(rtv, dsv);
        return false;
    };
    if (!(rtv[0] && rtv[1] && rtv[2] && rtv[3] && dsv)) return decline();   // the G-buffer's four targets: not a shadow map or another pass
    if (!decal) ++g_win.hullSeen;
    const int ei = eyeFor(resourceOf(dsv), false);
    if (ei < 0) return decline();
    Eye& e = g_eyes[ei];
    if (e.gb == Gb::Before) {
        // A hull draw before this eye's shell is not the own ship's pass; a decal never seeds; separate mode seeds before the issue.
        if (decal || !e.shell || detail::g_hullLayerSeparate) return decline();
        if (!seedEye(ctx, rtv, dsv, ei, instances, startInstance)) return decline();
    } else if (e.gb != Gb::In || g_inEye != ei) {
        return decline();
    }
    // A decal goes into the private layer only when it exports the slot (its slot target bound): the filter can then
    // clear it where it is a neighbour's, and an unpatched decal would paint wherever the private depth lets it.
    if (decal && !(rtv[kSlotTarget] && resourceOf(rtv[kSlotTarget]) == e.slotGame)) {
        ++g_win.decalNoSlot;
        return decline();
    }
    // Every hull-family G-buffer draw of the window: into the private RT0-3, the private slot target when the draw exports
    // the slot (engine motion substituted it: its slot target is bound at RT6), and the private depth. Every other state is
    // the game's, still bound from its draw.
    ID3D11RenderTargetView* bind[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
    for (int i = 0; i < 4; ++i) {
        bind[i] = static_cast<ID3D11RenderTargetView*>(privateView(ViewKind::Rtv, rtv[i], false));
        if (!bind[i]) return decline();
    }
    auto* bindDsv = static_cast<ID3D11DepthStencilView*>(privateView(ViewKind::Dsv, dsv, false));
    if (!bindDsv) return decline();
    UINT n = 4;
    if (rtv[kSlotTarget] && resourceOf(rtv[kSlotTarget]) == e.slotGame) {
        bind[kSlotTarget] = static_cast<ID3D11RenderTargetView*>(privateView(ViewKind::Rtv, rtv[kSlotTarget], false));
        if (bind[kSlotTarget]) n = kSlotTarget + 1;
    }
    if (n <= kSlotTarget) {
        ++e.noSlotDraws;
        ++g_win.noSlotDraws;
    }
    vScreenSetRenderTargetsRaw(ctx, n, bind, bindDsv);
    if (decal) {
        ++e.decals;
        ++g_win.decalsReissued;
    } else {
        ++e.hull;
    }
    g_pending = Pending{};
    g_pending.active = true;
    g_pending.eye = ei;
    g_pending.stage = -1;
    for (UINT i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i) g_pending.rtv[i] = rtv[i];   // the references pass to End
    g_pending.dsv = dsv;
    return true;
}

// The end of an eye's private G-buffer: the filter pass clears the neighbours' pixels, the count pass counts them, and the
// stats are copied for the readback (and, on the dump frame, the private depth and the stats for the dump).
void finishGbuffer(ID3D11DeviceContext* ctx, int ei, End why) {
    if (ei < 0) return;
    Eye& e = g_eyes[ei];
    if (g_inEye == ei) g_inEye = -1;
    if (e.gb != Gb::In) return;
    e.gb = Gb::Done;
    ++g_win.ended[why];
    if (!e.privDsv || !e.privSlot || !e.maskSrv || !e.stats) return;
    ID3D11Resource* slotRes = nullptr;
    e.privSlot->GetResource(&slotRes);
    ID3D11Resource* depthRes = nullptr;
    e.privDsv->GetResource(&depthRes);
    if (!slotRes || !depthRes) {
        releaseRef(slotRes);
        releaseRef(depthRes);
        return;
    }
    if (e.slotSrvOf != slotRes) {
        releaseRef(e.slotSrv);
        e.slotSrvOf = nullptr;
        if (SUCCEEDED(g_device->CreateShaderResourceView(slotRes, nullptr, &e.slotSrv)) && e.slotSrv) e.slotSrvOf = slotRes;
    }
    D3D11_TEXTURE2D_DESC dd{};
    static_cast<ID3D11Texture2D*>(depthRes)->GetDesc(&dd);
    if (e.slotSrv) {
        uint32_t cb[4] = {e.records, 0u, dd.Width, dd.Height};
        std::memcpy(&cb[1], &e.clearDepth, sizeof e.clearDepth);
        vScreenUpdateSubresourceRaw(ctx, g_gpu.passCb, 0, nullptr, cb, 0, 0);
        // The filter: RT0-3 and the depth of every neighbour pixel back to clear.
        {
            GfxSave save;
            save.capture(ctx);
            vScreenSetRenderTargetsRaw(ctx, 4, e.privRtv, e.privDsv);
            vScreenOMSetBlendStateRaw(ctx, nullptr, nullptr, 0xFFFFFFFFu);
            vScreenOMSetDepthStencilStateRaw(ctx, g_gpu.depthAlways, 0);
            ctx->RSSetState(g_gpu.noCull);
            D3D11_VIEWPORT vp{};
            vp.Width = static_cast<float>(dd.Width);
            vp.Height = static_cast<float>(dd.Height);
            vp.MaxDepth = 1.0f;
            vScreenRSSetViewportsRaw(ctx, 1, &vp);
            ctx->IASetInputLayout(nullptr);
            ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            vScreenVSSetShaderRaw(ctx, g_gpu.filterVs, nullptr, 0);
            ctx->HSSetShader(nullptr, nullptr, 0);
            ctx->DSSetShader(nullptr, nullptr, 0);
            ctx->GSSetShader(nullptr, nullptr, 0);
            vScreenPSSetShaderRaw(ctx, g_gpu.filterPs, nullptr, 0);
            ID3D11ShaderResourceView* srvs[2] = {e.slotSrv, e.maskSrv};
            vScreenPSSetShaderResourcesRaw(ctx, 0, 2, srvs);
            vScreenPSSetConstantBuffersRaw(ctx, 0, 1, &g_gpu.passCb);
            vScreenDrawRaw(ctx, 3, 0);
            save.restore(ctx);
        }
        // The count: stats[1] = the filtered pixels.
        {
            CsSave save;
            save.capture(ctx);
            ID3D11ShaderResourceView* srvs[2] = {e.slotSrv, e.maskSrv};
            ID3D11UnorderedAccessView* uavs[2] = {e.statsUav, nullptr};
            static const UINT keep[2] = {~0u, ~0u};
            ctx->CSSetShader(g_gpu.countCs, nullptr, 0);
            ctx->CSSetShaderResources(0, 2, srvs);
            ctx->CSSetUnorderedAccessViews(0, 2, uavs, keep);
            ctx->CSSetConstantBuffers(0, 1, &g_gpu.passCb);
            ctx->Dispatch((dd.Width + 7u) / 8u, (dd.Height + 7u) / 8u, 1);
            save.restore(ctx);
        }
        ++g_win.filters;
    }
    // The readback, a few frames later (never a stall).
    Readback* slot = nullptr;
    for (auto& r : e.ring)
        if (!r.pending) {
            slot = &r;
            break;
        }
    if (slot) {
        if (!slot->staging) slot->staging = makeStagingBuffer(16);
        if (slot->staging) {
            vScreenCopyResourceRaw(ctx, slot->staging, e.stats);
            slot->frame = g_frame;
            slot->pending = true;
        }
    } else {
        ++g_win.readbackBusy;
    }
    if (g_dump == Dump::Capturing) {
        // Separate mode: the game's G-buffer RT0 and main depth as the eye's G-buffer left them (the own ship discarded).
        if (e.gameRt0) copyToStaging(ctx, e.gameRt0, e.stageGbuf0);
        if (e.gameDepth) copyToStaging(ctx, e.gameDepth, e.stageMainDepth);
        copyToStaging(ctx, static_cast<ID3D11Texture2D*>(depthRes), e.stageDepth);
        if (!e.stageStats) e.stageStats = makeStagingBuffer(16);
        if (e.stageStats) vScreenCopyResourceRaw(ctx, e.stageStats, e.stats);
        e.dumpHull = e.hull;
    }
    releaseRef(slotRes);
    releaseRef(depthRes);
}

void noteShell(ID3D11DeviceContext* ctx) {
    ID3D11RenderTargetView* rtv[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
    ID3D11DepthStencilView* dsv = nullptr;
    ctx->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtv, &dsv);
    if (rtv[0] && rtv[1] && rtv[2] && rtv[3] && dsv) {   // the G-buffer's four targets: not a shadow map or another pass
        const int e = eyeFor(resourceOf(dsv), true);
        if (e >= 0) {
            if (g_inEye >= 0 && g_inEye != e) finishGbuffer(ctx, g_inEye, kEndShell);   // the next eye's shell ends the last one's G-buffer
            if (g_eyes[e].gb == Gb::Before) g_eyes[e].shell = true;
        }
    }
    releaseTargets(rtv, dsv);
}

// ---- the stages -------------------------------------------------------------------------------------------------------

// The eye's HDR target named by a draw's RT0 (the game's), staged on the dump frame (the last call wins).
void stageGameHdr(ID3D11DeviceContext* ctx, Eye& e, ID3D11RenderTargetView* rt0) {
    if (g_dump != Dump::Capturing || !rt0) return;
    ID3D11Resource* hdr = nullptr;
    rt0->GetResource(&hdr);
    D3D11_RESOURCE_DIMENSION dim = D3D11_RESOURCE_DIMENSION_UNKNOWN;
    if (hdr) hdr->GetType(&dim);
    if (hdr && dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) copyToStaging(ctx, static_cast<ID3D11Texture2D*>(hdr), e.stageGame);
    releaseRef(hdr);
}

bool beginStage(ID3D11DeviceContext* ctx, Stage stage, uint8_t bit) {
    ID3D11RenderTargetView* rtv[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
    ID3D11DepthStencilView* dsv = nullptr;
    ID3D11ShaderResourceView* srv[kSrvSlots] = {};
    ctx->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtv, &dsv);
    ctx->PSGetShaderResources(0, kSrvSlots, srv);
    const auto decline = [&](bool counterpartMissing) {
        if (counterpartMissing) g_noCpFrame[stage] = true;
        releaseTargets(rtv, dsv);
        for (auto*& s : srv) releaseRef(s);
        return false;
    };
    // The stage's eye: the eye depth bound as its depth target, else read as one of its resources (the depth copy); a
    // lighting draw that shows neither is the eye whose stage ran last, while its lighting window is open (a lighting
    // pass of another view, outside the window, gets no private targets).
    int ei = dsv ? eyeFor(resourceOf(dsv), false) : -1;
    for (UINT i = 0; ei < 0 && i < kSrvSlots; ++i)
        if (srv[i]) ei = eyeFor(resourceOf(srv[i]), false);
    if (ei < 0 && stage == kLight && g_stageEye >= 0 && g_eyes[g_stageEye].lightOpen) ei = g_stageEye;
    if (ei < 0 || !g_device) return decline(true);
    Eye& e = g_eyes[ei];
    if (e.gb == Gb::In) finishGbuffer(ctx, ei, stage == kCopy ? kEndCopy : kEndLight);   // the eye's G-buffer is over; the game's state is back
    g_stageEye = ei;
    if (stage != kCopy) {
        e.lightOpen = true;   // the shadow mask opens the lighting window; the sky closes it
        e.lightSpan = 0;
    }
    if (stage == kLight) stageGameHdr(ctx, e, rtv[0]);   // the game's HDR after this lighting draw: the last one is the comparison
    if (e.gb != Gb::Done) return decline(true);
    if (bit) {
        if (e.stagesDone & bit) return decline(false);   // once per eye per frame: a later use of the same pair is not the eye's
        e.stagesDone |= bit;
    }
    // Targets: every bound one gets a private counterpart (made now for the stage's own: f39, f60, the scratch R16).
    ID3D11RenderTargetView* bindRtv[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
    uint32_t swapped = 0;
    for (UINT i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i) {
        if (!rtv[i]) continue;
        bindRtv[i] = static_cast<ID3D11RenderTargetView*>(privateView(ViewKind::Rtv, rtv[i], true));
        if (!bindRtv[i]) return decline(true);
        ++swapped;
    }
    // The depth target must be the eye's (mirrored at the seed); it is never made here.
    ID3D11DepthStencilView* bindDsv = nullptr;
    if (dsv) {
        bindDsv = static_cast<ID3D11DepthStencilView*>(privateView(ViewKind::Dsv, dsv, false));
        if (!bindDsv) return decline(true);
        ++swapped;
    }
    // Resources: the G-buffer, depth, depth copy, shadow mask and HDR have counterparts; everything else (the shadow map,
    // the light list, the lookups) stays the game's.
    ID3D11ShaderResourceView* bindSrv[kSrvSlots] = {};
    bool srvSwapped = false;
    for (UINT i = 0; i < kSrvSlots; ++i) {
        bindSrv[i] = srv[i];
        if (!srv[i] || !findMirror(resourceOf(srv[i]))) continue;
        auto* p = static_cast<ID3D11ShaderResourceView*>(privateView(ViewKind::Srv, srv[i], false));
        if (!p) return decline(true);
        bindSrv[i] = p;
        srvSwapped = true;
        ++swapped;
    }
    if (srvSwapped) vScreenPSSetShaderResourcesRaw(ctx, 0, kSrvSlots, bindSrv);
    vScreenSetRenderTargetsRaw(ctx, boundCount(bindRtv), bindRtv, bindDsv);
    g_win.subst[stage] += swapped;
    if (stage == kLight) ++e.lightDraws;
    g_pending = Pending{};
    g_pending.active = true;
    g_pending.eye = ei;
    g_pending.stage = stage;
    for (UINT i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i) g_pending.rtv[i] = rtv[i];
    g_pending.dsv = dsv;
    for (UINT i = 0; i < kSrvSlots; ++i) g_pending.srv[i] = srv[i];
    g_pending.srvSwapped = srvSwapped;
    g_pending.hdrGame = rtv[0] ? resourceOf(rtv[0]) : nullptr;
    return true;
}

// A draw inside an eye's lighting window that is no lighting or stage draw: the sky closes the window; one that writes
// the eye's HDR is an unknown lighting pair, said once a pair.
void noteInLightWindow(ID3D11DeviceContext* ctx, uint64_t vs) {
    Eye& e = g_eyes[g_stageEye];
    if (vs == kSkyVs || ++e.lightSpan > kLightSpan) {
        e.lightOpen = false;
        return;
    }
    if (!e.hdrGame) return;
    ID3D11RenderTargetView* rt0 = nullptr;
    ctx->OMGetRenderTargets(1, &rt0, nullptr);
    const bool hdr = rt0 && resourceOf(rt0) == e.hdrGame;
    releaseRef(rt0);
    if (!hdr) return;
    const uint64_t ps = bindingShaderHash(BindSlot::Ps);
    for (uint32_t i = 0; i < g_unknownCount; ++i)
        if (g_unknownPairs[i][0] == vs && g_unknownPairs[i][1] == ps) return;
    if (g_unknownCount >= kMaxUnknownPairs) {
        if (!g_saidUnknownCap) {
            g_saidUnknownCap = true;
            Log::get().note("hull layer: %u unknown lighting pairs said; no more are (said once)", kMaxUnknownPairs);
        }
        return;
    }
    g_unknownPairs[g_unknownCount][0] = vs;
    g_unknownPairs[g_unknownCount][1] = ps;
    ++g_unknownCount;
    ++g_win.unknownPairs;
    Log::get().note("hull layer: unknown lighting pair vs %016llX ps %016llX writes the eye's HDR between the shadow mask and the sky (eye %d, frame %u; "
                    "not re-issued; said once a pair, %u so far)",
                    static_cast<unsigned long long>(vs), static_cast<unsigned long long>(ps), g_stageEye, g_frame, g_unknownCount);
}

// ---- Step 3: the tonemap re-issue, the cover, the composite -----------------------------------------------------------

void sayToneDeclined(const char* why) {
    ++g_win.toneDeclined;
    if (g_saidToneDecline) return;
    g_saidToneDecline = true;
    Log::get().note("hull layer: tonemap re-issue declined: %s (said once; such an eye-frame has no hull to composite, and separate mode falls back after it)",
                    why);
}

void sayDeclined(Declined why, int eye, const char* detail) {
    ++g_win.declined[why];
    if (g_saidDecline[why]) return;
    g_saidDecline[why] = true;
    Log::get().note("hull layer: composite declined for eye %d: %s (said once a reason)", eye, detail);
}

void timerBegin(GpuTimer& t, ID3D11DeviceContext* ctx) {
    t.open = -1;
    for (UINT i = 0; i < kTimerRing; ++i) {
        GpuTimer::Set& s = t.set[i];
        if (s.pending) continue;
        if (!s.disjoint) {
            D3D11_QUERY_DESC qd{};
            qd.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
            D3D11_QUERY_DESC qt{};
            qt.Query = D3D11_QUERY_TIMESTAMP;
            if (FAILED(g_device->CreateQuery(&qd, &s.disjoint)) || FAILED(g_device->CreateQuery(&qt, &s.t0)) || FAILED(g_device->CreateQuery(&qt, &s.t1))) {
                releaseRef(s.disjoint);
                releaseRef(s.t0);
                releaseRef(s.t1);
                return;
            }
        }
        ctx->Begin(s.disjoint);
        ctx->End(s.t0);
        t.open = static_cast<int>(i);
        return;
    }
}

void timerEnd(GpuTimer& t, ID3D11DeviceContext* ctx) {
    if (t.open < 0) return;
    GpuTimer::Set& s = t.set[t.open];
    ctx->End(s.t1);
    ctx->End(s.disjoint);
    s.pending = true;
    t.open = -1;
}

void timerCollect(GpuTimer& t, ID3D11DeviceContext* ctx) {
    for (auto& s : t.set) {
        if (!s.pending) continue;
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
        UINT64 a = 0, b = 0;
        if (ctx->GetData(s.disjoint, &dj, sizeof dj, D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK ||
            ctx->GetData(s.t0, &a, sizeof a, D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK || ctx->GetData(s.t1, &b, sizeof b, D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK)
            continue;
        s.pending = false;
        if (dj.Disjoint || dj.Frequency == 0 || b < a) continue;
        t.sumMs += static_cast<double>(b - a) * 1000.0 / static_cast<double>(dj.Frequency);
        ++t.samples;
    }
}

// The window's mean ms a timed call (-1: none timed), and the window's sums back to zero.
double timerTake(GpuTimer& t) {
    const double ms = t.samples ? t.sumMs / static_cast<double>(t.samples) : -1.0;
    t.sumMs = 0.0;
    t.samples = 0;
    return ms;
}

// The cover and TAA shaders, the sampler and their constant buffers, once.
bool ensureCompGpu() {
    if (g_cgpu.tried) return g_cgpu.ok;
    g_cgpu.tried = true;
    struct Item {
        const char* src;
        size_t len;
        const char* entry;
        ID3D11ComputeShader** out;
    };
    const Item items[3] = {{kCoverHlsl, sizeof(kCoverHlsl) - 1, "hull_layer_cover_cs", &g_cgpu.coverCs},
                           {kTaaHlsl, sizeof(kTaaHlsl) - 1, "hull_layer_taa_cs", &g_cgpu.taaCs},
                           {kSharpHlsl, sizeof(kSharpHlsl) - 1, "hull_layer_sharp_cs", &g_cgpu.sharpCs}};
    for (const Item& it : items) {
        ID3DBlob* code = nullptr;
        std::string why;
        if (!compileShader(it.src, it.len, it.entry, "cs_5_0", &code, &why)) {
            Log::get().note("hull layer: the %s shader did not compile (%s); separate mode keeps the own ship in the main pass this session", it.entry, why.c_str());
            if (code) code->Release();
            return false;
        }
        const HRESULT hr = g_device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, it.out);
        code->Release();
        if (FAILED(hr)) {
            Log::get().note("hull layer: the %s shader compiled but could not be created (hr 0x%08lX); separate mode keeps the own ship in the main pass this "
                            "session",
                            it.entry, static_cast<unsigned long>(hr));
            return false;
        }
    }
    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    g_cgpu.coverCb = makeConstantBuffer(16);
    g_cgpu.taaCb = makeConstantBuffer(160);
    g_cgpu.sharpCb = makeConstantBuffer(16);
    if (FAILED(g_device->CreateSamplerState(&sd, &g_cgpu.linear)) || !g_cgpu.coverCb || !g_cgpu.taaCb || !g_cgpu.sharpCb) {
        Log::get().note("hull layer: the composite's sampler or constant buffers could not be made; separate mode keeps the own ship in the main pass this session");
        return false;
    }
    Log::get().note("hull layer: the tone cover and composite shaders compiled and made (the hull layer's own TAA, head motion, current weight %.2f)",
                    static_cast<double>(kTaaCurrent));
    g_cgpu.ok = true;
    return true;
}

bool rgba8Family(DXGI_FORMAT f) {
    return f == DXGI_FORMAT_R8G8B8A8_TYPELESS || f == DXGI_FORMAT_R8G8B8A8_UNORM || f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
}

// A 2D texture with a default SRV and, when `uav`, a default UAV (both in the texture's own format).
bool makeTexture(uint32_t w, uint32_t h, DXGI_FORMAT fmt, bool uav, ID3D11Texture2D** tex, ID3D11ShaderResourceView** srv, ID3D11UnorderedAccessView** uavOut) {
    D3D11_TEXTURE2D_DESC td{};
    td.Width = w;
    td.Height = h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = fmt;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = (srv ? D3D11_BIND_SHADER_RESOURCE : 0u) | (uav ? D3D11_BIND_UNORDERED_ACCESS : 0u);
    if (FAILED(g_device->CreateTexture2D(&td, nullptr, tex)) || !*tex) return false;
    if (srv && (FAILED(g_device->CreateShaderResourceView(*tex, nullptr, srv)) || !*srv)) return false;
    if (uav && uavOut && (FAILED(g_device->CreateUnorderedAccessView(*tex, nullptr, uavOut)) || !*uavOut)) return false;
    return true;
}

// The eye's tone target T (the game's tonemap output's size, its view format for the RTV), cover C and depth Z.
bool ensureTone(int ei, uint32_t w, uint32_t h, DXGI_FORMAT rtvFmt) {
    Eye& e = g_eyes[ei];
    if (e.tone && e.toneW == w && e.toneH == h && e.toneRtvFmt == rtvFmt) return true;
    releaseTone(e);
    D3D11_TEXTURE2D_DESC td{};
    td.Width = w;
    td.Height = h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    D3D11_RENDER_TARGET_VIEW_DESC rd{};
    rd.Format = rtvFmt;
    rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    sd.Texture2D.MipLevels = 1;
    const bool ok = SUCCEEDED(g_device->CreateTexture2D(&td, nullptr, &e.tone)) && e.tone &&
                    SUCCEEDED(g_device->CreateRenderTargetView(e.tone, &rd, &e.toneRtv)) && e.toneRtv &&
                    SUCCEEDED(g_device->CreateShaderResourceView(e.tone, &sd, &e.toneSrv)) && e.toneSrv &&
                    makeTexture(w, h, DXGI_FORMAT_R8G8B8A8_UNORM, true, &e.cover, &e.coverSrv, &e.coverUav) &&
                    makeTexture(w, h, DXGI_FORMAT_R32_FLOAT, true, &e.coverZ, &e.coverZSrv, &e.coverZUav);
    if (!ok) {
        releaseTone(e);
        return false;
    }
    e.toneW = w;
    e.toneH = h;
    e.toneRtvFmt = rtvFmt;
    Log::get().note("hull layer: tone targets made for eye %d: %ux%u, the tonemap's view format %u (T R8G8B8A8, cover R8G8B8A8_UNORM, depth R32_FLOAT)", ei, w,
                    h, static_cast<unsigned>(rtvFmt));
    return true;
}

// A shader view over a depth texture (its depth plane), kept while the texture is the same. False when the texture refuses one.
bool depthView(ID3D11Resource* res, ID3D11ShaderResourceView*& srv, const void*& of) {
    if (!res) return false;
    if (of == res && srv) return true;
    releaseRef(srv);
    of = nullptr;
    D3D11_RESOURCE_DIMENSION dim = D3D11_RESOURCE_DIMENSION_UNKNOWN;
    res->GetType(&dim);
    if (dim != D3D11_RESOURCE_DIMENSION_TEXTURE2D) return false;
    D3D11_TEXTURE2D_DESC d{};
    static_cast<ID3D11Texture2D*>(res)->GetDesc(&d);
    if (!(d.BindFlags & D3D11_BIND_SHADER_RESOURCE) || d.SampleDesc.Count != 1) return false;
    DXGI_FORMAT f = DXGI_FORMAT_UNKNOWN;
    switch (d.Format) {
    case DXGI_FORMAT_R32G8X24_TYPELESS: f = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS; break;
    case DXGI_FORMAT_R32_TYPELESS: f = DXGI_FORMAT_R32_FLOAT; break;
    case DXGI_FORMAT_R24G8_TYPELESS: f = DXGI_FORMAT_R24_UNORM_X8_TYPELESS; break;
    case DXGI_FORMAT_R16_TYPELESS: f = DXGI_FORMAT_R16_UNORM; break;
    default: return false;
    }
    D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Format = f;
    sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    sd.Texture2D.MipLevels = 1;
    if (FAILED(g_device->CreateShaderResourceView(res, &sd, &srv)) || !srv) {
        srv = nullptr;
        return false;
    }
    of = res;
    return true;
}

// Right after the game's own issue of a 3-vertex draw: is it an eye's tonemap (the structure, and a PS slot 0-3 over the
// HDR an eye's lighting just lit)? True: T is cleared and bound as RT0, the private HDR at that slot; the caller re-issues
// the draw and hullLayerEnd puts the game's bindings back and runs the cover pass.
bool beginTone(ID3D11DeviceContext* ctx) {
    const uint64_t vs = bindingShaderHash(BindSlot::Vs);
    const uint64_t ps = bindingShaderHash(BindSlot::Ps);
    ToneAdmit ta;
    if (!tonemapAdmitStructure(ctx, ps, ta) || !tonemapAdmitFull(ta, vs)) return false;
    int ei = -1;
    UINT slot = 0;
    ID3D11ShaderResourceView* hdrView = nullptr;
    for (UINT s = 0; s < 4 && ei < 0; ++s) {
        ID3D11ShaderResourceView* v = nullptr;
        ctx->PSGetShaderResources(s, 1, &v);
        if (!v) continue;
        D3D11_SHADER_RESOURCE_VIEW_DESC vd{};
        v->GetDesc(&vd);
        const void* res = vd.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2D ? resourceOf(v) : nullptr;
        for (int i = 0; i < 2 && res; ++i)
            if (g_eyes[i].hdrGame == res && g_eyes[i].gb == Gb::Done) {
                ei = i;
                slot = s;
                break;
            }
        if (ei >= 0)
            hdrView = v;   // the reference is kept until the private view is found
        else
            v->Release();
    }
    if (ei < 0) return false;
    Eye& e = g_eyes[ei];
    const auto decline = [&](const char* why) {
        if (why) sayToneDeclined(why);
        releaseRef(hdrView);
        return false;
    };
    if (e.toneFrame == g_frame) return decline(nullptr);   // once an eye a frame: the first tonemap of its HDR
    if (e.lightDraws == 0) return decline("the eye's private HDR was not lit this frame");
    ID3D11Texture2D* out = nullptr;
    D3D11_TEXTURE2D_DESC od{};
    if (ta.rtvRes && SUCCEEDED(static_cast<ID3D11Resource*>(ta.rtvRes)->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&out))) && out) {
        out->GetDesc(&od);
        out->Release();
    }
    if (!out || !rgba8Family(od.Format) || !rgba8Family(ta.rtvFmt)) return decline("the tonemap's output is not an R8G8B8A8 texture");
    if (!ensureCompGpu()) return decline("the composite's shaders are missing");
    auto* privHdr = static_cast<ID3D11ShaderResourceView*>(privateView(ViewKind::Srv, hdrView, false));
    if (!privHdr) return decline("the private HDR has no shader view");
    if (!ensureTone(ei, od.Width, od.Height, ta.rtvFmt)) return decline("a private tone target could not be made");
    releaseRef(hdrView);
    timerBegin(g_toneTimer, ctx);
    g_pending = Pending{};
    ctx->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, g_pending.rtv, &g_pending.dsv);
    ctx->PSGetShaderResources(0, kSrvSlots, g_pending.srv);
    ID3D11ShaderResourceView* bindSrv[kSrvSlots];
    for (UINT i = 0; i < kSrvSlots; ++i) bindSrv[i] = g_pending.srv[i];
    bindSrv[slot] = privHdr;
    static const float clear[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    vScreenClearRenderTargetViewRaw(ctx, e.toneRtv, clear);
    vScreenPSSetShaderResourcesRaw(ctx, 0, kSrvSlots, bindSrv);
    vScreenSetRenderTargetsRaw(ctx, 1, &e.toneRtv, nullptr);
    g_pending.active = true;
    g_pending.eye = ei;
    g_pending.stage = kToneStage;
    g_pending.srvSwapped = true;
    g_pending.toneOut = ta.rtvRes;
    return true;
}

// After the tonemap re-issue, the game's bindings back: the cover pass into C and Z. The eye-frame is toned when it ran.
void coverPass(ID3D11DeviceContext* ctx, int ei, const void* toneOut) {
    Eye& e = g_eyes[ei];
    ID3D11Resource* privZ = nullptr;
    if (e.privDsv) e.privDsv->GetResource(&privZ);
    const bool views = privZ && e.mainDepth && depthView(privZ, e.privZSrv, e.privZOf) && depthView(e.mainDepth, e.mainZSrv, e.mainZOf);
    releaseRef(privZ);
    if (!views) {
        sayToneDeclined("the private or the game's eye depth refuses a shader view");
        return;
    }
    D3D11_TEXTURE2D_DESC md{};
    e.mainDepth->GetDesc(&md);
    if (md.Width != e.toneW || md.Height != e.toneH) {
        sayToneDeclined("the eye depth and the tonemap's output differ in size");
        return;
    }
    uint32_t cb[4] = {0u, e.toneW, e.toneH, 0u};
    std::memcpy(&cb[0], &e.clearDepth, sizeof e.clearDepth);
    vScreenUpdateSubresourceRaw(ctx, g_cgpu.coverCb, 0, nullptr, cb, 0, 0);
    {
        CsStageSave save;
        save.save(ctx);
        ID3D11ShaderResourceView* srvs[3] = {e.toneSrv, e.privZSrv, e.mainZSrv};
        ID3D11UnorderedAccessView* uavs[CsStageSave::kUavs] = {e.coverUav, e.coverZUav};
        ctx->CSSetUnorderedAccessViews(0, CsStageSave::kUavs, uavs, nullptr);
        ctx->CSSetShader(g_cgpu.coverCs, nullptr, 0);
        ctx->CSSetShaderResources(0, 3, srvs);
        ctx->CSSetConstantBuffers(0, 1, &g_cgpu.coverCb);
        ctx->Dispatch((e.toneW + 7u) / 8u, (e.toneH + 7u) / 8u, 1);
        ID3D11UnorderedAccessView* none[CsStageSave::kUavs] = {};
        ctx->CSSetUnorderedAccessViews(0, CsStageSave::kUavs, none, nullptr);   // C and Z off before the game's views come back
        save.restore(ctx);
    }
    e.toneFrame = g_frame;
    e.toneOut = toneOut;
    ++g_win.toned;
}

// The TAA's history pair and the composited eye at the output size, for VR eye `eye`.
bool ensureCompTargets(int eye, uint32_t w, uint32_t h) {
    Comp& c = g_comp[eye];
    if (c.out && c.outW == w && c.outH == h) return true;
    releaseComp(c);
    bool ok = true;
    for (int i = 0; i < 2 && ok; ++i) ok = makeTexture(w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, true, &c.hist[i], &c.histSrv[i], &c.histUav[i]);
    ok = ok && makeTexture(w, h, DXGI_FORMAT_R8G8B8A8_UNORM, true, &c.out, nullptr, &c.outUav);
    if (!ok) {
        releaseComp(c);
        return false;
    }
    c.outW = w;
    c.outH = h;
    Log::get().note("hull layer: composite targets made for eye %d: %ux%u (history R16G16B16A16_FLOAT x2, out R8G8B8A8_UNORM)", eye, w, h);
    return true;
}

// The frame that just ended (g_frame not yet advanced), separate mode: an eye that seeded and drew the own ship must have
// been lit, tonemapped and composited (or made ready, on a fallback frame). A miss holds the discard off.
void judgeFrame() {
    bool miss = false;
    for (auto& e : g_eyes) {
        if (!e.seen || !e.shell || e.gb == Gb::Before) continue;   // no own ship drawn into this eye: nothing to put back
        if (e.gb == Gb::Off) {
            ++g_win.failSeed;   // no mask: nothing was discarded on this eye-frame (Step 2's fallback), so no hold
            continue;
        }
        if (e.lightDraws == 0) {
            ++g_win.failLit;
            miss = true;
        } else if (e.toneFrame != g_frame) {
            ++g_win.failTone;
            miss = true;
        } else if (e.compFrame != g_frame) {
            ++g_win.failComposite;
            miss = true;
        } else if (e.compApplied) {
            ++g_win.compositedEyes;
        } else {
            ++g_win.readyEyes;
        }
    }
    if (miss)
        g_hold = kHoldFrames;
    else if (g_hold)
        --g_hold;
}

bool frameGenOn() { return temporalPassFrameGenMode() != 0 || frameGenShadowOn(); }

// The next frame's decision, made before any of its draws.
void decideNext() {
    g_discard = false;
    Fallback why = kFallbacks;
    if (!runtimeVrProfile() || nativeOnFootActive()) {
        why = kFbOutside;
    } else if (frameGenOn()) {
        why = kFbFrameGen;
        if (!g_saidFrameGen) {
            g_saidFrameGen = true;
            Log::get().note("hull layer: separate mode stands aside while frame generation is on (fix.frame_generation or fix.frame_generation_shadow): the own "
                            "ship stays in the main pass (said once)");
        }
    } else if (!g_haveOwn) {
        why = kFbMaskPending;
    } else if (g_lastOwn > kOwnMaskMax) {
        why = kFbMaskLarge;
        g_win.ownMax = std::max(g_win.ownMax, g_lastOwn);
        if (!g_saidMaskLarge) {
            g_saidMaskLarge = true;
            Log::get().note("hull layer: separate mode falls back: the own mask holds %u records (over %u: docking, or a station or ship within %.0f m); the own "
                            "ship stays in the main pass while it does (said once a window)",
                            g_lastOwn, kOwnMaskMax, static_cast<double>(kOwnRadius));
        }
    } else if (g_hold) {
        why = kFbStreak;
    } else {
        g_discard = true;
    }
    if (g_discard)
        ++g_win.separateFrames;
    else
        ++g_win.fallback[why];
    // A fallback other than the hold itself (mask pending or large, frame generation, outside) re-arms the hold, so
    // separate resumes only after kHoldFrames clean frames, never on the first frame the reason clears.
    if (!g_discard && why != kFbStreak) g_hold = kHoldFrames;
}

// ---- the readbacks, the dump ------------------------------------------------------------------------------------------

void readBacks(ID3D11DeviceContext* ctx) {
    for (auto& e : g_eyes) {
        for (auto& r : e.ring) {
            if (!r.pending || !r.staging || g_frame - r.frame < kReadbackLag) continue;
            D3D11_MAPPED_SUBRESOURCE map{};
            const HRESULT hr = ctx->Map(r.staging, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &map);
            if (hr == DXGI_ERROR_WAS_STILL_DRAWING) continue;
            r.pending = false;
            if (FAILED(hr)) {
                if (!g_saidReadback) {
                    g_saidReadback = true;
                    Log::get().note("hull layer: a stats readback could not be mapped (hr 0x%08lX; said once)", static_cast<unsigned long>(hr));
                }
                continue;
            }
            uint32_t v[4] = {};
            std::memcpy(v, map.pData, sizeof v);
            ctx->Unmap(r.staging, 0);
            g_win.own.push_back(v[0]);
            g_win.filtered.push_back(v[1]);
            ++g_win.readbacks;
            g_lastOwn = v[0];
            g_haveOwn = true;
        }
    }
}

const char* writeTexture(ID3D11DeviceContext* ctx, ID3D11Texture2D* staging, const std::wstring& dir, const wchar_t* name, int eye) {
    if (!staging) return "missing";
    D3D11_TEXTURE2D_DESC d{};
    staging->GetDesc(&d);
    uint32_t bytes = 0;
    switch (d.Format) {
    case DXGI_FORMAT_R11G11B10_FLOAT: bytes = 4; break;
    case DXGI_FORMAT_R32G8X24_TYPELESS: case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: bytes = 8; break;
    case DXGI_FORMAT_R16G16B16A16_FLOAT: bytes = 8; break;
    case DXGI_FORMAT_R32_TYPELESS: case DXGI_FORMAT_D32_FLOAT: case DXGI_FORMAT_R32_FLOAT: bytes = 4; break;
    case DXGI_FORMAT_R24G8_TYPELESS: case DXGI_FORMAT_D24_UNORM_S8_UINT: bytes = 4; break;
    // The G-buffer's RT0 (GameGbuf0, separate mode): f23 in the census, or another 32-bit colour format.
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: case DXGI_FORMAT_R10G10B10A2_UNORM: case DXGI_FORMAT_R10G10B10A2_UINT: bytes = 4; break;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: bytes = 4; break;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: bytes = 4; break;
    default: return "format unsupported";
    }
    D3D11_MAPPED_SUBRESOURCE map{};
    if (FAILED(ctx->Map(staging, 0, D3D11_MAP_READ, 0, &map))) return "map failed";
    wchar_t path[MAX_PATH];
    _snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%s\\eye_%s_%s%s.bin", dir.c_str(), g_dumpStamp, name, eye ? L"_e1" : L"");
    FILE* f = nullptr;
    bool ok = false;
    if (_wfopen_s(&f, path, L"wb") == 0 && f) {
        // EDVRTEX1: the magic, then version, width, height, DXGI format, row bytes, frame, eye, UI-bound, UI-flags (tools/eye_inputs.py).
        const uint32_t header[9] = {1u, d.Width, d.Height, static_cast<uint32_t>(d.Format), d.Width * bytes, g_dumpFrame, static_cast<uint32_t>(eye), 0u, 0u};
        ok = std::fwrite("EDVRTEX1", 1, 8, f) == 8 && std::fwrite(header, sizeof(header), 1, f) == 1;
        for (uint32_t y = 0; y < d.Height && ok; ++y)
            ok = std::fwrite(static_cast<const char*>(map.pData) + static_cast<size_t>(y) * map.RowPitch, 1, d.Width * bytes, f) == d.Width * bytes;
        ok = (std::fclose(f) == 0) && ok;
    }
    ctx->Unmap(staging, 0);
    return ok ? "written" : "write failed";
}

void writeDump(ID3D11DeviceContext* ctx) {
    const std::wstring dir = Log::get().dir() + L"\\eyes";
    CreateDirectoryW(dir.c_str(), nullptr);
    bool any = false;
    for (int i = 0; i < 2; ++i) {
        Eye& e = g_eyes[i];
        if (!e.stageHull && !e.stageGame && !e.stageDepth && !e.stageGbuf0 && !e.stageMainDepth && !e.stageTone && !e.stageComp) continue;
        any = true;
        uint32_t w = 0, h = 0, fmt = 0;
        if (e.stageHull) {
            D3D11_TEXTURE2D_DESC d{};
            e.stageHull->GetDesc(&d);
            w = d.Width;
            h = d.Height;
            fmt = static_cast<uint32_t>(d.Format);
        }
        // The dump frame's own stats (a stall here is the dump's, like the textures' maps).
        long own = -1, filtered = -1, seedRead = -1, seedInst = -1;
        D3D11_MAPPED_SUBRESOURCE map{};
        if (e.stageStats && SUCCEEDED(ctx->Map(e.stageStats, 0, D3D11_MAP_READ, 0, &map))) {
            uint32_t v[4] = {};
            std::memcpy(v, map.pData, sizeof v);
            ctx->Unmap(e.stageStats, 0);
            own = static_cast<long>(v[0]);
            filtered = static_cast<long>(v[1]);
            seedRead = static_cast<long>(v[2]);
            seedInst = static_cast<long>(v[3]);
        }
        const char* hull = writeTexture(ctx, e.stageHull, dir, L"HullHdr", i);
        const char* game = writeTexture(ctx, e.stageGame, dir, L"GameHdrLit", i);
        const char* depth = writeTexture(ctx, e.stageDepth, dir, L"HullDepth", i);
        Log::get().note("hull layer: dump %ls eye %d frame %u: HullHdr %ux%u format %u %s, GameHdrLit %s, HullDepth %s; own=%ld filtered_px=%ld "
                        "seed=%ld/%ld hull_draws=%u light_draws=%u",
                        g_dumpStamp, i, g_dumpFrame, w, h, fmt, hull, game, depth, own, filtered, seedRead, seedInst, e.dumpHull, e.dumpLight);
        if (detail::g_hullLayerSeparate) {
            // The main pass with the own ship discarded: the game's G-buffer RT0 and its eye depth after the eye's G-buffer.
            const char* gbuf = writeTexture(ctx, e.stageGbuf0, dir, L"GameGbuf0", i);
            const char* mainDepth = writeTexture(ctx, e.stageMainDepth, dir, L"GameDepth", i);
            Log::get().note("hull layer: dump %ls eye %d: GameGbuf0 %s, GameDepth %s (separate mode: the game's G-buffer RT0 and main depth after the "
                            "eye's G-buffer; own_draws=%u decal_draws=%u decals_reissued=%u)",
                            g_dumpStamp, i, gbuf, mainDepth, e.ownDraws, e.ownDecals, e.decals);
            // Step 3: the cover at render size (the tonemapped hull where shown, alpha 1) and the composited eye at output size, after the
            // composite and before the UI layer's door; on a fallback frame it is the composite that was made and not applied.
            const char* tone = writeTexture(ctx, e.stageTone, dir, L"HullTone", i);
            const char* comp = writeTexture(ctx, e.stageComp, dir, L"HullComposite", i);
            Log::get().note("hull layer: dump %ls eye %d: HullTone %s, HullComposite %s (separate mode, Step 3: the composite %s)", g_dumpStamp, i, tone, comp,
                            !e.stageComp ? "did not run" : e.dumpApplied ? "applied" : "made, not applied (a fallback frame)");
        }
        releaseStaging(e);
    }
    if (!any)
        Log::get().note("hull layer: dump %ls frame %u: nothing captured (no eye seeded its private G-buffer and drew its lighting in the frame)",
                        g_dumpStamp, g_dumpFrame);
}

// ---- the window line --------------------------------------------------------------------------------------------------

void loMidHi(std::vector<uint32_t> v, uint32_t* lo, uint32_t* mid, uint32_t* hi) {
    *lo = *mid = *hi = 0;
    if (v.empty()) return;
    std::sort(v.begin(), v.end());
    *lo = v.front();
    *mid = v[(v.size() - 1) / 2];
    *hi = v.back();
}

void report() {
    uint32_t o[3], f[3], hd[3], ld[3];
    loMidHi(g_win.own, &o[0], &o[1], &o[2]);
    loMidHi(g_win.filtered, &f[0], &f[1], &f[2]);
    loMidHi(g_win.hullDraws, &hd[0], &hd[1], &hd[2]);
    loMidHi(g_win.lightDraws, &ld[0], &ld[1], &ld[2]);
    const char* state = g_win.seeds ? (g_win.readbacks == 0 ? "mask-pending" : g_win.subst[kLight] == 0 ? "partial" : "applied")
                        : g_win.noShader    ? "no-shader"
                        : g_win.noSlot      ? "no-slot-target"
                        : g_win.refused     ? "seed-refused"
                        : g_win.eyeFrames   ? "no-seed"
                        : g_win.hullSeen    ? "no-shell"
                                            : "no-eye-pass";
    Log::get().note("hull layer: state=%s frames=%u eye_frames=%u seeds=%u no_seed=%u no_slot=%u refused=%u own=%u/%u/%u readbacks=%u "
                    "filtered_px=%u/%u/%u hull_draws=%u/%u/%u reissued=%u no_slot_draws=%u clears=%u filters=%u "
                    "subst=copy:%u,mask:%u,light:%u light_draws=%u/%u/%u no_counterpart=copy:%u,mask:%u,light:%u unknown_light_pairs=%u "
                    "ended=copy:%u,shell:%u,light:%u,frame:%u terrain_frames=%u auto_dumps=%u",
                    state, g_win.frames, g_win.eyeFrames, g_win.seeds, g_win.noSeed, g_win.noSlot, g_win.refused, o[0], o[1], o[2], g_win.readbacks,
                    f[0], f[1], f[2], hd[0], hd[1], hd[2], g_win.reissued, g_win.noSlotDraws, g_win.clears, g_win.filters, g_win.subst[kCopy],
                    g_win.subst[kMask], g_win.subst[kLight], ld[0], ld[1], ld[2], g_win.noCounterpart[kCopy], g_win.noCounterpart[kMask],
                    g_win.noCounterpart[kLight], g_win.unknownPairs, g_win.ended[kEndCopy], g_win.ended[kEndShell], g_win.ended[kEndLight],
                    g_win.ended[kEndFrame], g_win.terrainFrames, g_win.autoDumps);
    if (detail::g_hullLayerSeparate)
        Log::get().note("hull layer: main pass: own_draws=%u decal_draws=%u no_variant_draws=%u fallback_frames=%u decals_reissued=%u decal_no_slot=%u "
                        "(separate mode: draws issued with the own discard; window draws that could not take it; eye-frames whose main pass kept the own "
                        "ship, no mask)",
                        g_win.ownDraws, g_win.ownDecals, g_win.noVariant, g_win.fallbackFrames, g_win.decalsReissued, g_win.decalNoSlot);
    if (detail::g_hullLayerSeparate) {
        const double toneMs = timerTake(g_toneTimer), compMs = timerTake(g_compTimer);
        Log::get().note("hull layer: composite: separate_frames=%u composited_eyes=%u ready_eyes=%u fallback=framegen:%u,outside:%u,mask_pending:%u,"
                        "mask_large:%u,streak:%u failed=seed:%u,lit:%u,tone:%u,composite:%u declined=stale:%u,size:%u,gpu:%u toned=%u tone_declined=%u "
                        "history_resets=%u matched_by_index=%u own_max=%u site=after_ui_resolve:%u,upscaler_out:%u aa=hull_taa gpu_ms=tone:%.3f,composite:%.3f "
                        "(separate mode, Step 3: frames whose main pass may discard the own ship, and those kept by reason; eye-frames composited after the "
                        "upscaler, or made ready on a fallback frame; misses by stage hold the discard off %u frames; site: where applied composites landed, "
                        "upscaler_out only when no UI resolve can run; gpu_ms a call, -1 when none was timed)",
                        g_win.separateFrames, g_win.compositedEyes, g_win.readyEyes, g_win.fallback[kFbFrameGen], g_win.fallback[kFbOutside],
                        g_win.fallback[kFbMaskPending], g_win.fallback[kFbMaskLarge], g_win.fallback[kFbStreak], g_win.failSeed, g_win.failLit,
                        g_win.failTone, g_win.failComposite, g_win.declined[kDcStale], g_win.declined[kDcSize], g_win.declined[kDcGpu], g_win.toned,
                        g_win.toneDeclined, g_win.historyResets, g_win.byIndex, g_win.ownMax, g_win.siteAfterResolve, g_win.siteUpscalerOut, toneMs,
                        compMs, kHoldFrames);
        g_saidMaskLarge = false;   // the large-mask line once a window
    }
}

}  // namespace

void hullLayerConfigure(Config& cfg) {
    std::string mode = cfg.getString("diag.hull_layer", "off");
    if (mode != "off" && mode != "dump" && mode != "separate") {
        Log::get().note("hull layer: diag.hull_layer=%s is not off, dump or separate; read as off", mode.c_str());
        mode = "off";
    }
    if (mode != g_mode) {
        g_mode = mode;
        Log::get().note("hull layer: diag.hull_layer=%s (TEMPORARY; docs\\research-own-hull-layer-2026-11-08.md, Steps 1-3: the own hull, picked per "
                        "record by a GPU own mask, re-lit into private targets and written into the eye dump, with auto dumps; dump changes nothing the game "
                        "draws, separate also takes the own ship out of the game's main G-buffer and composites it back after the upscaler)",
                        mode.c_str());
        if (mode == "separate")
            Log::get().note("hull layer: separate mode takes the own ship out of the game's main pass and puts it back after the upscaler (Step 3: the "
                            "game's tonemap re-issued on the private HDR, the layer's own TAA, the composite before the UI layer); a frame that cannot "
                            "compose keeps the ship in the main pass");
    }
    detail::g_hullLayerOn = mode == "dump" || mode == "separate";
    detail::g_hullLayerSeparate = mode == "separate";
}

bool hullLayerBeforeMainDraw(ID3D11DeviceContext* ctx, UINT instances, UINT startInstance) {
    if (!detail::g_hullLayerSeparate || !ctx || g_pending.active || g_main.active) return false;
    const uint64_t vs = bindingShaderHash(BindSlot::Vs);
    const bool hull = isHullVs(vs), decal = vs == kDecalVs;
    if (!hull && !decal) return false;
    ID3D11RenderTargetView* rtv[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
    ID3D11DepthStencilView* dsv = nullptr;
    ctx->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtv, &dsv);
    const auto decline = [&] {
        releaseTargets(rtv, dsv);
        return false;
    };
    if (!(rtv[0] && rtv[1] && rtv[2] && rtv[3] && dsv)) return decline();   // the G-buffer's four targets
    const int ei = eyeFor(resourceOf(dsv), false);
    if (ei < 0) return decline();
    Eye& e = g_eyes[ei];
    // The seed draw is the own ship's: its mask is made before it is issued, so the seed draw is discarded too.
    if (hull && e.gb == Gb::Before && e.shell && !seedEye(ctx, rtv, dsv, ei, instances, startInstance)) return decline();
    // Step 3's safety: on a frame decided as a fallback (decideNext) the layer is still made, and the ship stays in the main pass.
    if (!g_discard) return decline();
    if (e.gb != Gb::In || g_inEye != ei || !e.maskSrv) return decline();   // no mask this eye-frame: the draw is the game's
    // The draw must be engine motion's substitute: its slot target at RT6, and its patched pixel shader bound.
    if (!(rtv[kSlotTarget] && resourceOf(rtv[kSlotTarget]) == e.slotGame)) {
        ++g_win.noVariant;
        return decline();
    }
    ID3D11PixelShader* ps = nullptr;
    ctx->PSGetShader(&ps, nullptr, nullptr);
    const char* why = nullptr;
    ID3D11PixelShader* variant = ps ? engineVelocityOwnDiscardPs(ctx, ps, &why) : nullptr;
    if (!variant) {
        ++g_win.noVariant;
        if (!g_saidNoVariant) {
            g_saidNoVariant = true;
            Log::get().note("hull layer: a %s draw in the window got no own-discard variant (%s); such draws keep the own ship in the main pass (said once)",
                            decal ? "decal" : "hull", why ? why : "?");
        }
        releaseRef(ps);
        return decline();
    }
    g_main.active = true;
    g_main.ps = ps;   // the reference passes to AfterMainDraw
    ctx->PSGetShaderResources(kEngineVelocityOwnMaskPsSlot, 1, &g_main.srv);
    vScreenPSSetShaderResourcesRaw(ctx, kEngineVelocityOwnMaskPsSlot, 1, &e.maskSrv);
    vScreenPSSetShaderRaw(ctx, variant, nullptr, 0);
    if (decal) {
        ++e.ownDecals;
        ++g_win.ownDecals;
    } else {
        ++e.ownDraws;
        ++g_win.ownDraws;
    }
    releaseTargets(rtv, dsv);   // the targets' references go; the variant stays bound for the game's issue
    return true;
}

void hullLayerAfterMainDraw(ID3D11DeviceContext* ctx) {
    if (!g_main.active) return;
    if (ctx) {
        vScreenPSSetShaderRaw(ctx, g_main.ps, nullptr, 0);
        vScreenPSSetShaderResourcesRaw(ctx, kEngineVelocityOwnMaskPsSlot, 1, &g_main.srv);
    }
    releaseRef(g_main.ps);
    releaseRef(g_main.srv);
    g_main = MainPending{};
}

bool hullLayerBegin(ID3D11DeviceContext* ctx, char kind, UINT count, UINT instances, UINT startInstance) {
    if (!detail::g_hullLayerOn || !ctx || g_pending.active) return false;
    // Step 3: the game's tonemap (a 3-vertex, one-instance draw, then admitted by structure and by the HDR it reads).
    if (detail::g_hullLayerSeparate && (kind == 'D' || kind == 'N') && count == 3 && instances == 1 && startInstance == 0 && g_device &&
        (g_eyes[0].gb == Gb::Done || g_eyes[1].gb == Gb::Done) && beginTone(ctx))
        return true;
    const uint64_t vs = bindingShaderHash(BindSlot::Vs);
    if (isTerrainVs(vs)) g_terrainFrame = true;
    if (vs == kShellVs) {
        noteShell(ctx);
        return false;
    }
    if (isHullVs(vs)) return beginHullDraw(ctx, instances, startInstance, false);
    if (vs == kDecalVs && detail::g_hullLayerSeparate) return beginHullDraw(ctx, instances, startInstance, true);   // the own decals (Step 2)
    if (isLightVs(vs)) return beginStage(ctx, kLight, 0);
    for (const auto& pair : kStagePairs) {
        if (pair.vs != vs) continue;
        if (bindingShaderHash(BindSlot::Ps) == pair.ps) return beginStage(ctx, pair.stage, pair.bit);
    }
    if (g_stageEye >= 0 && g_eyes[g_stageEye].lightOpen) noteInLightWindow(ctx, vs);
    return false;
}

void hullLayerEnd(ID3D11DeviceContext* ctx) {
    if (!g_pending.active) return;
    Pending& p = g_pending;
    if (p.srvSwapped) vScreenPSSetShaderResourcesRaw(ctx, 0, kSrvSlots, p.srv);
    vScreenSetRenderTargetsRaw(ctx, boundCount(p.rtv), p.rtv, p.dsv);
    if (p.stage < 0) ++g_win.reissued;
    if (p.stage == kToneStage && p.eye >= 0) {
        // Step 3: T is off the output merger now; the cover pass reads it.
        coverPass(ctx, p.eye, p.toneOut);
        timerEnd(g_toneTimer, ctx);
    }
    if (p.stage == kLight && p.eye >= 0 && g_dump == Dump::Capturing) {
        // The private HDR right after this private lighting draw: the last one is the comparison.
        Eye& e = g_eyes[p.eye];
        Mirror* m = findMirror(p.hdrGame);
        if (m) copyToStaging(ctx, m->priv, e.stageHull);
        e.dumpLight = e.lightDraws;
    }
    releaseTargets(p.rtv, p.dsv);
    for (auto*& s : p.srv) releaseRef(s);
    p = Pending{};
}

void hullLayerNoteClearRtv(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* rtv, const float colour[4]) {
    if (!detail::g_hullLayerOn || !rtv || !colour || !g_device || !findMirror(resourceOf(rtv))) return;
    auto* priv = static_cast<ID3D11RenderTargetView*>(privateView(ViewKind::Rtv, rtv, false));
    if (priv) vScreenClearRenderTargetViewRaw(ctx, priv, colour);
}

void hullLayerNoteClearDsv(ID3D11DeviceContext* ctx, ID3D11DepthStencilView* dsv, UINT flags, float depth, UINT8 stencil) {
    if (!detail::g_hullLayerOn || !dsv) return;
    const void* res = resourceOf(dsv);
    ClearValue* slot = nullptr;
    for (auto& c : g_clearValues)
        if (c.res == res) slot = &c;
    if (!slot) {
        slot = &g_clearValues[g_clearNext];
        g_clearNext = (g_clearNext + 1) % 8;
        *slot = ClearValue{};
        slot->res = res;
    }
    if (flags & D3D11_CLEAR_DEPTH) slot->depth = depth;
    if (flags & D3D11_CLEAR_STENCIL) slot->stencil = stencil;
    if (!g_device || !findMirror(res)) return;
    auto* priv = static_cast<ID3D11DepthStencilView*>(privateView(ViewKind::Dsv, dsv, false));
    if (priv) {
        VrWorldInternalScope internal;   // the hook forwards this clear raw and does not see it as the game's
        ctx->ClearDepthStencilView(priv, flags, depth, stencil);
    }
}

void hullLayerArmDump(const wchar_t* stamp) {
    if (!detail::g_hullLayerOn || g_dump != Dump::Idle) return;
    wcsncpy_s(g_dumpStamp, stamp ? stamp : L"", _TRUNCATE);
    g_dump = Dump::Armed;
    Log::get().note("hull layer: dump %ls armed for the next whole frame", g_dumpStamp);
}

void hullLayerCompositeEye(ID3D11DeviceContext* ctx, int eye, const HullLayerCompositeInputs& in) {
    if (!detail::g_hullLayerSeparate || !ctx || eye < 0 || eye > 1 || !g_device || !in.output || !in.outputSrv || !in.source || !in.tanNow) return;
    VrWorldInternalScope internal;   // the layer's own dispatch and copy
    // The hull eye: the one whose tonemap wrote the submitted texture this frame (the same index first, should both eyes share one
    // texture); else the eye of the same index, toned this frame.
    int hi = -1;
    for (int k = 0; k < 2 && hi < 0; ++k) {
        const int i = k == 0 ? eye : 1 - eye;
        if (g_eyes[i].toneFrame == g_frame && g_eyes[i].toneOut == in.source) hi = i;
    }
    const bool byIndex = hi < 0 && g_eyes[eye].toneFrame == g_frame;
    if (byIndex) hi = eye;
    if (hi < 0) {
        // Nothing toned this frame: the judge counts the miss. Toned on the frame before: the treat came after the frame boundary.
        for (const auto& e : g_eyes)
            if (e.toneFrame != ~0u && e.toneFrame + 1 == g_frame) {
                sayDeclined(kDcStale, eye, "the eye's tonemap re-issue is a frame old (the submit came after the frame boundary)");
                break;
            }
        return;
    }
    Eye& e = g_eyes[hi];
    if (!e.coverSrv || !e.coverZSrv || !in.renderW || !in.renderH || !in.outW || !in.outH || in.regionX + in.renderW > e.toneW ||
        in.regionY + in.renderH > e.toneH) {
        sayDeclined(kDcSize, eye, "the eye's region does not fit the tonemap's output");
        return;
    }
    if (!ensureCompGpu() || !ensureCompTargets(eye, in.outW, in.outH)) {
        sayDeclined(kDcGpu, eye, "the composite's shaders or targets are missing");
        return;
    }
    Comp& c = g_comp[eye];
    const bool history = c.have && c.lastFrame != ~0u && c.lastFrame + 1 == g_frame && c.renderW == in.renderW && c.renderH == in.renderH &&
                         in.tanPrev && in.headDelta;
    if (!history) ++g_win.historyResets;
    struct TaaCb {
        float tanNow[4], tanPrev[4], dRow0[4], dRow1[4], dRow2[4], headT[4], jit[4], depthAB[4];
        int32_t sizes[4], origin[4];
    };
    static_assert(sizeof(TaaCb) == 160, "the TAA constant buffer is HullLayerTaaCb's 160 bytes");
    TaaCb cb{};
    std::memcpy(cb.tanNow, in.tanNow, sizeof cb.tanNow);
    std::memcpy(cb.tanPrev, in.tanPrev ? in.tanPrev : in.tanNow, sizeof cb.tanPrev);
    float* rows[3] = {cb.dRow0, cb.dRow1, cb.dRow2};
    for (int r = 0; r < 3; ++r)
        for (int k = 0; k < 3; ++k) rows[r][k] = in.headDelta ? in.headDelta[r * 3 + k] : (r == k ? 1.0f : 0.0f);
    if (in.headTrans && in.useTrans) {
        for (int k = 0; k < 3; ++k) cb.headT[k] = in.headTrans[k];
        cb.headT[3] = 1.0f;
    }
    cb.jit[0] = in.jitterX;
    cb.jit[1] = in.jitterY;
    cb.jit[2] = history ? 1.0f : 0.0f;
    cb.jit[3] = kTaaCurrent;
    cb.depthAB[0] = in.depthA;
    cb.depthAB[1] = in.depthB;
    cb.sizes[0] = static_cast<int32_t>(in.renderW);
    cb.sizes[1] = static_cast<int32_t>(in.renderH);
    cb.sizes[2] = static_cast<int32_t>(in.outW);
    cb.sizes[3] = static_cast<int32_t>(in.outH);
    cb.origin[0] = static_cast<int32_t>(in.regionX);
    cb.origin[1] = static_cast<int32_t>(in.regionY);
    timerBegin(g_compTimer, ctx);
    vScreenUpdateSubresourceRaw(ctx, g_cgpu.taaCb, 0, nullptr, &cb, 0, 0);
    {
        CsStageSave save;
        save.save(ctx);
        ID3D11ShaderResourceView* srvs[4] = {e.coverSrv, e.coverZSrv, c.histSrv[c.read], in.outputSrv};
        // Every saved UAV slot is set: one the caller left bound over the upscaler's output would null its view at t3.
        ID3D11UnorderedAccessView* uavs[CsStageSave::kUavs] = {c.histUav[c.read ^ 1u], c.outUav};
        ctx->CSSetUnorderedAccessViews(0, CsStageSave::kUavs, uavs, nullptr);
        ctx->CSSetShader(g_cgpu.taaCs, nullptr, 0);
        ctx->CSSetShaderResources(0, 4, srvs);
        ctx->CSSetConstantBuffers(0, 1, &g_cgpu.taaCb);
        ctx->CSSetSamplers(0, 1, &g_cgpu.linear);
        ctx->Dispatch((in.outW + 7u) / 8u, (in.outH + 7u) / 8u, 1);
        ID3D11UnorderedAccessView* none[CsStageSave::kUavs] = {};
        ctx->CSSetUnorderedAccessViews(0, CsStageSave::kUavs, none, nullptr);   // the history and the out off before the caller's views come back
        // The sharpen and composite over the TAA result just written (kSharpHlsl): it rewrites the out the TAA made.
        const float sharpCb[4] = {kSharpAmount, 0.0f, 0.0f, 0.0f};
        vScreenUpdateSubresourceRaw(ctx, g_cgpu.sharpCb, 0, nullptr, sharpCb, 0, 0);
        ID3D11ShaderResourceView* noSrv[4] = {};
        ctx->CSSetShaderResources(0, 4, noSrv);   // the history just written comes back as a resource
        ID3D11ShaderResourceView* sharpSrv[2] = {c.histSrv[c.read ^ 1u], in.outputSrv};
        ID3D11UnorderedAccessView* sharpUav[CsStageSave::kUavs] = {c.outUav};
        ctx->CSSetUnorderedAccessViews(0, CsStageSave::kUavs, sharpUav, nullptr);
        ctx->CSSetShader(g_cgpu.sharpCs, nullptr, 0);
        ctx->CSSetShaderResources(0, 2, sharpSrv);
        ctx->CSSetConstantBuffers(0, 1, &g_cgpu.sharpCb);
        ctx->Dispatch((in.outW + 7u) / 8u, (in.outH + 7u) / 8u, 1);
        ctx->CSSetShaderResources(0, 4, noSrv);
        ctx->CSSetUnorderedAccessViews(0, CsStageSave::kUavs, none, nullptr);
        save.restore(ctx);
    }
    // Applied only when this frame's main pass discarded the own ship on this eye; otherwise it is made (the history stays warm,
    // and a miss is seen) and the upscaled eye keeps the game's own ship.
    const bool apply = g_discard && (e.ownDraws + e.ownDecals) > 0;
    if (apply) vScreenCopyResourceRaw(ctx, in.output, c.out);
    timerEnd(g_compTimer, ctx);
    if (g_dump == Dump::Capturing) {
        copyToStaging(ctx, e.cover, e.stageTone);
        copyToStaging(ctx, c.out, e.stageComp);
        e.dumpApplied = apply;
    }
    c.read ^= 1u;
    c.lastFrame = g_frame;
    c.renderW = in.renderW;
    c.renderH = in.renderH;
    c.have = true;
    e.compFrame = g_frame;
    e.compApplied = apply;
    if (byIndex) ++g_win.byIndex;
    if (apply) ++(in.afterUiResolve ? g_win.siteAfterResolve : g_win.siteUpscalerOut);
}

void hullLayerFrameBoundary(ID3D11DeviceContext* ctx) {
    if (g_main.active) hullLayerAfterMainDraw(ctx);   // never: Before and After are one call chain
    if (!detail::g_hullLayerOn) {
        if (g_device || !g_mirrors.empty()) releaseAll();
        g_win = Window{};
        return;
    }
    if (g_pending.active && ctx) hullLayerEnd(ctx);   // never: Begin and End are one call chain
    {
        VrWorldInternalScope internal;   // the passes, copies and maps here are the layer's own
        // The frame that just ended: an eye whose G-buffer window is still open is filtered now.
        if (ctx)
            for (int i = 0; i < 2; ++i)
                if (g_eyes[i].gb == Gb::In) finishGbuffer(ctx, i, kEndFrame);
        if (detail::g_hullLayerSeparate) judgeFrame();   // Step 3: before the frame number moves on (the tone and composite marks name it)
        ++g_frame;
        if (ctx) {
            readBacks(ctx);
            timerCollect(g_toneTimer, ctx);
            timerCollect(g_compTimer, ctx);
        }
        if (g_dump == Dump::Capturing && ctx) writeDump(ctx);
    }
    for (auto& e : g_eyes) {
        if (!e.seen || !e.shell) continue;
        ++g_win.eyeFrames;
        if (e.gb == Gb::Before) ++g_win.noSeed;
        if (e.gb == Gb::Off && detail::g_hullLayerSeparate) ++g_win.fallbackFrames;   // the seed ran and failed: the main pass drew the own ship
        if (e.gb == Gb::Done) {
            g_win.hullDraws.push_back(e.hull);
            g_win.lightDraws.push_back(e.lightDraws);
        }
    }
    ++g_win.frames;
    if (g_terrainFrame) ++g_win.terrainFrames;
    for (int s = 0; s < kStages; ++s) {
        if (g_noCpFrame[s]) ++g_win.noCounterpart[s];
        g_noCpFrame[s] = false;
    }
    // The auto dump's streak: a seeded frame over terrain with an own mask of a ship's size.
    const bool good = g_seededFrame && g_terrainFrame && g_haveOwn && g_lastOwn >= kAutoOwnLo && g_lastOwn <= kAutoOwnHi;
    g_autoStreak = good ? g_autoStreak + 1 : 0;
    if (g_dump == Dump::Capturing) {
        g_dump = Dump::Idle;   // written above
    } else if (g_dump == Dump::Armed) {
        g_dump = Dump::Capturing;
        g_dumpFrame = g_frame;
    }
    const ULONGLONG now = GetTickCount64();
    if (g_autoStreak >= kAutoFrames && g_autoDumps < kAutoMax && g_dump == Dump::Idle && (g_lastAutoMs == 0 || now - g_lastAutoMs >= kAutoGapMs)) {
        temporalPassArmEyeDump();   // the hotkey's own call: the eye run, its census and ledger, and hullLayerArmDump
        if (g_dump == Dump::Armed) {
            ++g_autoDumps;
            ++g_win.autoDumps;
            g_lastAutoMs = now;
            Log::get().note("hull layer: auto dump armed (reason: own mask %u records, seeded over terrain for %u frames in a row; %u of %u this session)",
                            g_lastOwn, g_autoStreak, g_autoDumps, kAutoMax);
            g_autoStreak = 0;
        }
    }
    // The next frame's eyes: the depth identities stay, so an eye keeps its index.
    for (auto& e : g_eyes) resetFrame(e);
    g_inEye = g_stageEye = -1;
    g_terrainFrame = g_seededFrame = false;
    // Step 3: whether the next frame's main pass may discard the own ship, from what is known before its first draw.
    if (detail::g_hullLayerSeparate)
        decideNext();
    else
        g_discard = false;
    evictStale();
    if (!g_win.startMs) g_win.startMs = now;
    if (now - g_win.startMs >= kReportMs) {
        report();
        g_win = Window{};
        g_win.startMs = now;
    }
}

void hullLayerShutdown() {
    g_pending = Pending{};   // references, if any, die with the device
    g_main = MainPending{};
    releaseAll();
    detail::g_hullLayerOn = false;
}

}  // namespace edvr
