// diag.hull_layer (TEMPORARY): the own hull as a private, lit layer, Step 1 v2 -- in the eye dump only. hull_layer.h says
// what it does; docs\research-own-hull-layer-2026-11-08.md ("Step 1 v2 build") says what the dump must show.
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
#include "binding_shadow.h"
#include "engine_velocity.h"  // engineVelocityLiveNow: the patched shaders that export the slot at MRT6
#include "temporal_pass.h"    // temporalPassArmEyeDump: the auto dump arms the eye dump the hotkey arms
#include "vr_world_route.h"   // VrWorldInternalScope: the layer's own clears, copies and passes step past the game-facing hooks
#include "vscreen.h"          // the raw setters: the binding shadow keeps describing the game's bindings

namespace edvr {

namespace detail {
bool g_hullLayerOn = false;
}  // namespace detail

namespace {

// The own ship's families (engine_velocity_families.h kFamilies: vs_66DE..., vs_61AE..., vs_AACF..., vs_DE54...).
constexpr uint64_t kHullVs[4] = {0x66DE2CADB1F4AE6Bull, 0x61AE8EB05FDC18DDull, 0xAACFDCF2FB9AD809ull, 0xDE545DC8EE4FBB87ull};
constexpr uint64_t kShellVs = 0xBFE51414CC3024B4ull;     // the cockpit shell (engine_velocity_families.h)
constexpr uint64_t kTerrainVs = 0x72BDD292154158ADull;   // the terrain's colour pass (celestial_motion.h kCelestialPatchVs)
constexpr uint32_t kTerrainVsHigh = 0x6C320453u;         // the other terrain family, known by its prefix only (the ledgers)
constexpr uint64_t kSkyVs = 0xF8FA801F2CB1E27Cull;       // the sky, the first forward draw after the lighting (flat_projection_recipes.h)
// The lighting: the full-screen sun and ambient, then the instanced volumes (the doc's "The binding map", the mt= census).
constexpr uint64_t kLightVs[6] = {0x7E38A6AA1269C901ull, 0x8289669D93A18C1Dull, 0x963B52C73B4143ACull,
                                  0x24DE25E496342EB8ull, 0xB121A79E457669E8ull, 0x0357BBB2DEE43C1Full};
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
constexpr uint32_t kAutoFrames = 600, kAutoMax = 3, kAutoOwnLo = 20, kAutoOwnHi = 200;
constexpr ULONGLONG kAutoGapMs = 60000;
constexpr ULONGLONG kReportMs = 30000;

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
    "        uint own = (valid > 0 && all(isfinite(p)) && dot(d, d) <= radius2) ? 1u : 0u;\n"
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
};

struct Window {
    ULONGLONG startMs = 0;
    uint32_t frames = 0, eyeFrames = 0, hullSeen = 0, seeds = 0, noSeed = 0, noSlot = 0, noShader = 0, refused = 0;
    uint32_t reissued = 0, noSlotDraws = 0, clears = 0, filters = 0, readbacks = 0, readbackBusy = 0, terrainFrames = 0;
    uint32_t unknownPairs = 0, autoDumps = 0;
    uint32_t subst[kStages] = {}, noCounterpart[kStages] = {}, ended[kEnds] = {};
    std::vector<uint32_t> own, filtered, hullDraws, lightDraws;
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
     g_saidPool = false, g_saidUnknownCap = false, g_saidReadback = false;
// The dump: armed -> the next frame is captured -> written at the boundary after it.
enum class Dump : uint8_t { Idle, Armed, Capturing };
Dump g_dump = Dump::Idle;
wchar_t g_dumpStamp[32] = L"";
uint32_t g_dumpFrame = 0;
// The auto dump (session-wide).
uint32_t g_autoStreak = 0, g_autoDumps = 0;
ULONGLONG g_lastAutoMs = 0;

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
    e.dumpHull = e.dumpLight = 0;
}

void releaseSeedViews(Eye& e) {
    for (auto*& v : e.privRtv) releaseRef(v);
    releaseRef(e.privSlot);
    releaseRef(e.privDsv);
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

ID3D11Buffer* makeConstantBuffer() {
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = 16;
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

bool beginHullDraw(ID3D11DeviceContext* ctx, UINT instances, UINT startInstance) {
    ID3D11RenderTargetView* rtv[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
    ID3D11DepthStencilView* dsv = nullptr;
    ctx->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtv, &dsv);
    const auto decline = [&] {
        releaseTargets(rtv, dsv);
        return false;
    };
    if (!(rtv[0] && rtv[1] && rtv[2] && rtv[3] && dsv)) return decline();   // the G-buffer's four targets: not a shadow map or another pass
    ++g_win.hullSeen;
    const void* depthRes = resourceOf(dsv);
    const int ei = eyeFor(depthRes, false);
    if (ei < 0) return decline();
    Eye& e = g_eyes[ei];
    if (e.gb == Gb::Before) {
        if (!e.shell) return decline();   // a hull draw before this eye's shell: not the own ship's pass
        // The seed: this eye's first hull-family G-buffer draw after the shell.
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
            return decline();
        }
        if (!ensureDevice(ctx) || !ensureGpu()) {
            ++g_win.noShader;
            return decline();
        }
        ID3D11View* priv[6] = {};
        for (int i = 0; i < 4; ++i) priv[i] = privateView(ViewKind::Rtv, rtv[i], true);
        priv[4] = privateView(ViewKind::Rtv, rtv[kSlotTarget], true);
        priv[5] = privateView(ViewKind::Dsv, dsv, true);
        for (auto* p : priv)
            if (!p) {
                ++g_win.refused;
                sayRefused("a private target could not be made");
                return decline();
            }
        const char* why = nullptr;
        if (!seedOwnMask(ctx, e, instances, startInstance, &why)) {
            ++g_win.refused;
            sayRefused(why ? why : "?");
            return decline();
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
        e.gb = Gb::In;
        g_inEye = ei;
        g_seededFrame = true;
        ++g_win.seeds;
    } else if (e.gb != Gb::In || g_inEye != ei) {
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
    ++e.hull;
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
        if (!e.stageHull && !e.stageGame && !e.stageDepth) continue;
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
}

}  // namespace

void hullLayerConfigure(Config& cfg) {
    std::string mode = cfg.getString("diag.hull_layer", "off");
    if (mode != "off" && mode != "dump") {
        Log::get().note("hull layer: diag.hull_layer=%s is not off or dump; read as off", mode.c_str());
        mode = "off";
    }
    if (mode != g_mode) {
        g_mode = mode;
        Log::get().note("hull layer: diag.hull_layer=%s (TEMPORARY; docs\\research-own-hull-layer-2026-11-08.md, Step 1 v2: the own hull, picked per "
                        "record by a GPU own mask, re-lit into private targets and written into the eye dump, with auto dumps; nothing the game draws changes)",
                        mode.c_str());
    }
    detail::g_hullLayerOn = mode == "dump";
}

bool hullLayerBegin(ID3D11DeviceContext* ctx, UINT instances, UINT startInstance) {
    if (!detail::g_hullLayerOn || !ctx || g_pending.active) return false;
    const uint64_t vs = bindingShaderHash(BindSlot::Vs);
    if (isTerrainVs(vs)) g_terrainFrame = true;
    if (vs == kShellVs) {
        noteShell(ctx);
        return false;
    }
    if (isHullVs(vs)) return beginHullDraw(ctx, instances, startInstance);
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

void hullLayerFrameBoundary(ID3D11DeviceContext* ctx) {
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
        ++g_frame;
        if (ctx) readBacks(ctx);
        if (g_dump == Dump::Capturing && ctx) writeDump(ctx);
    }
    for (auto& e : g_eyes) {
        if (!e.seen || !e.shell) continue;
        ++g_win.eyeFrames;
        if (e.gb == Gb::Before) ++g_win.noSeed;
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
    releaseAll();
    detail::g_hullLayerOn = false;
}

}  // namespace edvr
