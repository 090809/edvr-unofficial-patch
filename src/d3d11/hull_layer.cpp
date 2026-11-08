// diag.hull_layer (TEMPORARY): the own hull as a private, lit layer, Step 1 -- in the eye dump only. hull_layer.h says
// what it does; docs\research-own-hull-layer-2026-11-08.md ("Step 1 build") says what the dump must show.
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
#include "vr_world_route.h"   // VrWorldInternalScope: the layer's own clears and copies step past the game-facing hooks
#include "vscreen.h"          // the raw setters: the binding shadow keeps describing the game's bindings

namespace edvr {

namespace detail {
bool g_hullLayerOn = false;
}  // namespace detail

namespace {

// The own-ship block's families (engine_velocity_families.h kFamilies: vs_66DE..., vs_61AE..., vs_AACF..., vs_DE54...).
constexpr uint64_t kHullVs[4] = {0x66DE2CADB1F4AE6Bull, 0x61AE8EB05FDC18DDull, 0xAACFDCF2FB9AD809ull, 0xDE545DC8EE4FBB87ull};
constexpr uint64_t kShellVs = 0xBFE51414CC3024B4ull;     // the cockpit shell (engine_velocity_families.h)
constexpr uint64_t kTerrainVs = 0x72BDD292154158ADull;   // the terrain's colour pass (celestial_motion.h kCelestialPatchVs)
constexpr uint32_t kTerrainVsHigh = 0x6C320453u;         // the other terrain family, known by its prefix only (the ledgers)
constexpr uint32_t kMaxGap = 6;                          // non-hull pool draws the block may hold in a row (the ledgers: up to 4)
constexpr UINT kPoolSlot = 33;                           // VS t33: the pool a pool draw reads
constexpr UINT kSrvSlots = 16;                           // PS resource slots a stage is searched and restored over
constexpr size_t kMaxMirrors = 16;
constexpr size_t kMaxViews = 64;
constexpr ULONGLONG kReportMs = 30000;

enum Stage : int { kCopy = 0, kMask = 1, kSun = 2, kStages = 3 };
struct StagePair {
    uint64_t vs, ps;
    Stage stage;
    uint8_t bit;   // once per eye per frame
};
// The doc's binding map ("The binding map"); flat_forward_light.h, flat_inert_pairs.h and flat_deferred_camera.h name the same pairs.
constexpr StagePair kStagePairs[] = {
    {0xDEF19B035D5EDEDCull, 0xCB95394B50D737D6ull, kCopy, 1},   // the depth copy into f39
    {0x53211E8C072CD02Eull, 0x7EAC71963E66C5FEull, kMask, 2},   // the shadow mask, first draw
    {0x53211E8C072CD02Eull, 0xB403F48CB35D9739ull, kMask, 4},   // ...and second
    {0x7E38A6AA1269C901ull, 0x7CECABDE34FFBE9Eull, kSun, 8},    // the sun and ambient light
};
const char* const kStageNames[kStages] = {"copy", "mask", "sun"};

enum End : int { kEndTerrain = 0, kEndNonPool, kEndGap, kEndFrame, kEnds };

bool isHullVs(uint64_t vs) {
    for (uint64_t h : kHullVs)
        if (h == vs) return true;
    return false;
}

// A private copy of one game texture: the same desc, default usage, nothing shared. `game` is an identity only.
struct Mirror {
    const void* game = nullptr;
    D3D11_TEXTURE2D_DESC desc{};
    ID3D11Texture2D* priv = nullptr;
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

enum class Run : uint8_t { Before, In, Done };
struct Eye {
    const void* depth = nullptr;   // the game's eye depth texture (identity): the eye's key
    bool seen = false;             // this frame
    bool shell = false;            // the cockpit shell was drawn into this depth this frame
    Run run = Run::Before;
    uint32_t hull = 0, gap = 0, gapDraws = 0;
    uint8_t stagesDone = 0;
    // The dump's staging copies (the capture frame only).
    ID3D11Texture2D* stageHull = nullptr;
    ID3D11Texture2D* stageGame = nullptr;
    ID3D11Texture2D* stageDepth = nullptr;
    uint32_t dumpHull = 0;
};

struct Window {
    ULONGLONG startMs = 0;
    uint32_t frames = 0, eyeFrames = 0, blocks = 0, notFound = 0, gapDraws = 0, reissued = 0, clears = 0;
    uint32_t subst[kStages] = {}, noCounterpart[kStages] = {}, ended[kEnds] = {};
    uint32_t hist[257] = {};
};

// The pending re-issue: what Begin bound over, which End puts back.
struct Pending {
    bool active = false;
    int eye = -1;
    int stage = -1;   // -1: a block draw
    ID3D11RenderTargetView* rtv[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
    ID3D11DepthStencilView* dsv = nullptr;
    ID3D11ShaderResourceView* srv[kSrvSlots] = {};
    bool srvSwapped = false;
    const void* hdrGame = nullptr;   // the sun's RT0 (the game's HDR), for the dump
};

std::string g_mode = "off";
ID3D11Device* g_device = nullptr;   // a reference
std::vector<Mirror> g_mirrors;
std::vector<ViewEntry> g_views;
ClearValue g_clearValues[8];
uint32_t g_clearNext = 0;
Eye g_eyes[2];
int g_runEye = -1;
Pending g_pending;
Window g_win;
bool g_noCpFrame[kStages] = {};
uint32_t g_frame = 0;
bool g_saidMirrorCap = false, g_saidViewCap = false, g_saidCreateFail = false, g_saidMsaa = false;
// The dump: armed -> the next frame is captured -> written at the boundary after it.
enum class Dump : uint8_t { Idle, Armed, Capturing };
Dump g_dump = Dump::Idle;
wchar_t g_dumpStamp[32] = L"";
uint32_t g_dumpFrame = 0;

const void* resourceOf(ID3D11View* view) {
    if (!view) return nullptr;
    ID3D11Resource* res = nullptr;
    view->GetResource(&res);
    if (res) res->Release();   // an identity only: the view keeps the resource alive while it is bound
    return res;
}

void releaseTargets(ID3D11RenderTargetView** rtv, ID3D11DepthStencilView*& dsv) {
    for (UINT i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i)
        if (rtv[i]) { rtv[i]->Release(); rtv[i] = nullptr; }
    if (dsv) { dsv->Release(); dsv = nullptr; }
}

UINT boundCount(ID3D11RenderTargetView* const* rtv) {
    UINT n = 0;
    for (UINT i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i)
        if (rtv[i]) n = i + 1;
    return n;
}

void releaseStaging(Eye& e) {
    if (e.stageHull) { e.stageHull->Release(); e.stageHull = nullptr; }
    if (e.stageGame) { e.stageGame->Release(); e.stageGame = nullptr; }
    if (e.stageDepth) { e.stageDepth->Release(); e.stageDepth = nullptr; }
    e.dumpHull = 0;
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
        releaseStaging(e);
        e = Eye{};
    }
    g_runEye = -1;
    if (g_device) { g_device->Release(); g_device = nullptr; }
    g_dump = Dump::Idle;
}

bool ensureDevice(ID3D11DeviceContext* ctx) {
    if (!g_device && ctx) ctx->GetDevice(&g_device);
    return g_device != nullptr;
}

Mirror* findMirror(const void* game) {
    for (auto& m : g_mirrors)
        if (m.game == game) return &m;
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
    g_mirrors.push_back(m);
    Log::get().note("hull layer: private copy made of a %ux%u format %u target (bind 0x%X, %u held)", d.Width, d.Height,
                    static_cast<unsigned>(d.Format), d.BindFlags, static_cast<unsigned>(g_mirrors.size()));
    return &g_mirrors.back();
}

// The private counterpart of a game view: the mirror of its texture (made when `make` and missing), and a view over the
// mirror with the game view's own desc. Null when the view is not over a 2D texture, has no mirror and none is to be
// made, or a create failed.
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

// The eye whose depth texture this is; with `assign`, a slot is given to a depth not seen before (a slot not seen this
// frame is reused). -1 when none.
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
            g_eyes[i] = Eye{};
            g_eyes[i].depth = depth;
            g_eyes[i].seen = true;
            return i;
        }
    }
    return -1;
}

void endRun(int eye, End why) {
    if (eye < 0) return;
    g_eyes[eye].run = Run::Done;
    ++g_win.ended[why];
    if (g_runEye == eye) g_runEye = -1;
}

bool clearValueOf(const void* res, float* depth, uint8_t* stencil) {
    for (const auto& c : g_clearValues)
        if (c.res && c.res == res) {
            *depth = c.depth;
            *stencil = c.stencil;
            return true;
        }
    *depth = 0.0f;   // reversed Z, GEQUAL: the census's DCL lines clear the eye depth to 0
    *stencil = 0;
    return false;
}

ID3D11Texture2D* stagingCopy(ID3D11DeviceContext* ctx, ID3D11Texture2D* src, ID3D11Texture2D* existing) {
    if (existing || !src || !g_device) return existing;
    D3D11_TEXTURE2D_DESC d{};
    src->GetDesc(&d);
    if (d.SampleDesc.Count != 1) return nullptr;
    d.Usage = D3D11_USAGE_STAGING;
    d.BindFlags = 0;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    d.MiscFlags = 0;
    ID3D11Texture2D* staging = nullptr;
    if (FAILED(g_device->CreateTexture2D(&d, nullptr, &staging)) || !staging) return nullptr;
    ctx->CopyResource(staging, src);
    return staging;
}

ID3D11Texture2D* mirrorTexture(const void* game) {
    Mirror* m = findMirror(game);
    return m ? m->priv : nullptr;
}

// ---- the block --------------------------------------------------------------------------------------------------------

void noteShell(ID3D11DeviceContext* ctx) {
    ID3D11RenderTargetView* rtv[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
    ID3D11DepthStencilView* dsv = nullptr;
    ctx->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtv, &dsv);
    if (rtv[0] && rtv[1] && rtv[2] && rtv[3] && dsv) {   // the G-buffer's four targets: not a shadow map or another pass
        const int e = eyeFor(resourceOf(dsv), true);
        if (e >= 0 && g_eyes[e].run == Run::Before) g_eyes[e].shell = true;
    }
    releaseTargets(rtv, dsv);
}

void noteGap(ID3D11DeviceContext* ctx, uint64_t vs) {
    Eye& e = g_eyes[g_runEye];
    if (vs == kTerrainVs || static_cast<uint32_t>(vs >> 32) == kTerrainVsHigh) {
        endRun(g_runEye, kEndTerrain);
        return;
    }
    ID3D11ShaderResourceView* pool = nullptr;
    ctx->VSGetShaderResources(kPoolSlot, 1, &pool);
    if (!pool) {
        endRun(g_runEye, kEndNonPool);
        return;
    }
    pool->Release();
    if (++e.gap > kMaxGap) {
        endRun(g_runEye, kEndGap);
        return;
    }
    ++e.gapDraws;
}

bool beginBlockDraw(ID3D11DeviceContext* ctx) {
    ID3D11RenderTargetView* rtv[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
    ID3D11DepthStencilView* dsv = nullptr;
    ctx->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtv, &dsv);
    const auto decline = [&] {
        releaseTargets(rtv, dsv);
        return false;
    };
    if (!(rtv[0] && rtv[1] && rtv[2] && rtv[3] && dsv)) return decline();
    const void* depthRes = resourceOf(dsv);
    const int ei = eyeFor(depthRes, false);
    if (ei < 0) return decline();
    Eye& e = g_eyes[ei];
    if (e.run == Run::Done || (e.run == Run::Before && !e.shell)) return decline();   // other ships, or a hull draw before the shell
    if (!ensureDevice(ctx)) return decline();
    // The private G-buffer (RT0-3: the last is the game's HDR target itself) and depth, made from the game's own.
    ID3D11View* priv[5] = {};
    for (int i = 0; i < 4; ++i) priv[i] = privateView(ViewKind::Rtv, rtv[i], true);
    priv[4] = privateView(ViewKind::Dsv, dsv, true);
    if (!priv[0] || !priv[1] || !priv[2] || !priv[3] || !priv[4]) {
        if (e.run == Run::In) endRun(ei, kEndFrame);
        else e.run = Run::Done;
        return decline();
    }
    if (e.run == Run::Before) {
        if (g_runEye >= 0 && g_runEye != ei) endRun(g_runEye, kEndFrame);
        e.run = Run::In;
        g_runEye = ei;
        e.hull = e.gap = e.gapDraws = 0;
        // Cleared once per eye per frame, before the block's first draw: colour to 0, depth and stencil to the game's.
        static const float zero[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        for (int i = 0; i < 4; ++i) vScreenClearRenderTargetViewRaw(ctx, static_cast<ID3D11RenderTargetView*>(priv[i]), zero);
        float depth = 0.0f;
        uint8_t stencil = 0;
        clearValueOf(depthRes, &depth, &stencil);
        ctx->ClearDepthStencilView(static_cast<ID3D11DepthStencilView*>(priv[4]), D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, depth, stencil);
        ++g_win.clears;
    } else if (g_runEye != ei) {
        return decline();
    }
    e.gap = 0;
    ++e.hull;
    // Bind: the private RT0-3 alone (a substituted pool draw also has engine velocity's slot target at RT6, which must not
    // be written twice) and the private depth. Every other state is the game's, still bound from its draw.
    ID3D11RenderTargetView* bind[4] = {static_cast<ID3D11RenderTargetView*>(priv[0]), static_cast<ID3D11RenderTargetView*>(priv[1]),
                                       static_cast<ID3D11RenderTargetView*>(priv[2]), static_cast<ID3D11RenderTargetView*>(priv[3])};
    vScreenSetRenderTargetsRaw(ctx, 4, bind, static_cast<ID3D11DepthStencilView*>(priv[4]));
    g_pending = Pending{};
    g_pending.active = true;
    g_pending.eye = ei;
    g_pending.stage = -1;
    for (UINT i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i) g_pending.rtv[i] = rtv[i];   // the references pass to End
    g_pending.dsv = dsv;
    return true;
}

// ---- the stages -------------------------------------------------------------------------------------------------------

bool beginStage(ID3D11DeviceContext* ctx, const StagePair& pair) {
    ID3D11RenderTargetView* rtv[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
    ID3D11DepthStencilView* dsv = nullptr;
    ID3D11ShaderResourceView* srv[kSrvSlots] = {};
    ctx->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtv, &dsv);
    ctx->PSGetShaderResources(0, kSrvSlots, srv);
    const auto decline = [&](bool counterpartMissing) {
        if (counterpartMissing) g_noCpFrame[pair.stage] = true;
        releaseTargets(rtv, dsv);
        for (auto*& s : srv)
            if (s) { s->Release(); s = nullptr; }
        return false;
    };
    // The stage's eye: the eye depth bound as its depth target, else read as one of its resources (the depth copy).
    int ei = dsv ? eyeFor(resourceOf(dsv), false) : -1;
    for (UINT i = 0; ei < 0 && i < kSrvSlots; ++i)
        if (srv[i]) ei = eyeFor(resourceOf(srv[i]), false);
    if (ei < 0 || g_eyes[ei].hull == 0 || !g_device) return decline(true);
    Eye& e = g_eyes[ei];
    if (e.stagesDone & pair.bit) return decline(false);   // once per eye per frame: a later use of the same pair is not the eye's
    e.stagesDone |= pair.bit;
    // Targets: every bound one gets a private counterpart (made now for the stage's own: f39, f60, the scratch R16).
    ID3D11RenderTargetView* bindRtv[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
    uint32_t swapped = 0;
    for (UINT i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i) {
        if (!rtv[i]) continue;
        bindRtv[i] = static_cast<ID3D11RenderTargetView*>(privateView(ViewKind::Rtv, rtv[i], true));
        if (!bindRtv[i]) return decline(true);
        ++swapped;
    }
    // The depth target must be the eye's (mirrored at the block); it is never made here.
    ID3D11DepthStencilView* bindDsv = nullptr;
    if (dsv) {
        bindDsv = static_cast<ID3D11DepthStencilView*>(privateView(ViewKind::Dsv, dsv, false));
        if (!bindDsv) return decline(true);
        ++swapped;
    }
    // Resources: the G-buffer, depth, depth copy, shadow mask and HDR have counterparts; everything else stays the game's.
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
    // The game's HDR right after the game's own sun draw: emissive plus sun, the dump's comparison.
    if (pair.stage == kSun && g_dump == Dump::Capturing && rtv[0] && !e.stageGame) {
        ID3D11Resource* hdr = nullptr;
        rtv[0]->GetResource(&hdr);
        D3D11_RESOURCE_DIMENSION dim = D3D11_RESOURCE_DIMENSION_UNKNOWN;
        if (hdr) hdr->GetType(&dim);
        if (hdr && dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) e.stageGame = stagingCopy(ctx, static_cast<ID3D11Texture2D*>(hdr), nullptr);
        if (hdr) hdr->Release();
    }
    if (srvSwapped) vScreenPSSetShaderResourcesRaw(ctx, 0, kSrvSlots, bindSrv);
    vScreenSetRenderTargetsRaw(ctx, boundCount(bindRtv), bindRtv, bindDsv);
    g_win.subst[pair.stage] += swapped;
    g_pending = Pending{};
    g_pending.active = true;
    g_pending.eye = ei;
    g_pending.stage = pair.stage;
    for (UINT i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i) g_pending.rtv[i] = rtv[i];
    g_pending.dsv = dsv;
    for (UINT i = 0; i < kSrvSlots; ++i) g_pending.srv[i] = srv[i];
    g_pending.srvSwapped = srvSwapped;
    g_pending.hdrGame = rtv[0] ? resourceOf(rtv[0]) : nullptr;
    return true;
}

// ---- the dump ---------------------------------------------------------------------------------------------------------

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
        const char* hull = writeTexture(ctx, e.stageHull, dir, L"HullHdr", i);
        const char* game = writeTexture(ctx, e.stageGame, dir, L"GameHdrSun", i);
        const char* depth = writeTexture(ctx, e.stageDepth, dir, L"HullDepth", i);
        Log::get().note("hull layer: dump %ls eye %d frame %u: HullHdr %ux%u format %u %s, GameHdrSun %s, HullDepth %s; block %u draws",
                        g_dumpStamp, i, g_dumpFrame, w, h, fmt, hull, game, depth, e.dumpHull);
        releaseStaging(e);
    }
    if (!any)
        Log::get().note("hull layer: dump %ls frame %u: nothing captured (no eye drew the block and the sun draw in the frame)", g_dumpStamp, g_dumpFrame);
}

// ---- the window line --------------------------------------------------------------------------------------------------

void report() {
    uint32_t lo = 0, mid = 0, hi = 0, n = 0;
    for (uint32_t i = 0; i <= 256; ++i) n += g_win.hist[i];
    if (n) {
        uint32_t seen = 0;
        bool haveLo = false, haveMid = false;
        for (uint32_t i = 0; i <= 256; ++i) {
            if (!g_win.hist[i]) continue;
            if (!haveLo) { lo = i; haveLo = true; }
            seen += g_win.hist[i];
            if (!haveMid && seen * 2 >= n) { mid = i; haveMid = true; }
            hi = i;
        }
    }
    const char* state = g_win.reissued == 0 ? (g_win.eyeFrames == 0 ? "no-eye-pass" : "no-block")
                        : g_win.subst[kSun] == 0 ? "partial"
                                                 : "applied";
    Log::get().note("hull layer: state=%s frames=%u eye_frames=%u blocks=%u not_found=%u hull_draws=%u/%u/%u gap_draws=%u reissued=%u clears=%u "
                    "subst=copy:%u,mask:%u,sun:%u no_counterpart=copy:%u,mask:%u,sun:%u ended=terrain:%u,non_pool:%u,gap:%u,frame:%u",
                    state, g_win.frames, g_win.eyeFrames, g_win.blocks, g_win.notFound, lo, mid, hi, g_win.gapDraws, g_win.reissued, g_win.clears,
                    g_win.subst[kCopy], g_win.subst[kMask], g_win.subst[kSun], g_win.noCounterpart[kCopy], g_win.noCounterpart[kMask],
                    g_win.noCounterpart[kSun], g_win.ended[kEndTerrain], g_win.ended[kEndNonPool], g_win.ended[kEndGap], g_win.ended[kEndFrame]);
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
        Log::get().note("hull layer: diag.hull_layer=%s (TEMPORARY; docs\\research-own-hull-layer-2026-11-08.md, Step 1: the own hull re-lit into private "
                        "targets and written into the eye dump; nothing the game draws changes)",
                        mode.c_str());
    }
    detail::g_hullLayerOn = mode == "dump";
}

bool hullLayerBegin(ID3D11DeviceContext* ctx) {
    if (!detail::g_hullLayerOn || !ctx || g_pending.active) return false;
    const uint64_t vs = bindingShaderHash(BindSlot::Vs);
    if (vs == kShellVs) {
        noteShell(ctx);
        return false;
    }
    if (isHullVs(vs)) return beginBlockDraw(ctx);
    if (g_runEye >= 0) noteGap(ctx, vs);
    for (const auto& pair : kStagePairs) {
        if (pair.vs != vs) continue;
        if (bindingShaderHash(BindSlot::Ps) == pair.ps) return beginStage(ctx, pair);
    }
    return false;
}

void hullLayerEnd(ID3D11DeviceContext* ctx) {
    if (!g_pending.active) return;
    Pending& p = g_pending;
    if (p.srvSwapped) vScreenPSSetShaderResourcesRaw(ctx, 0, kSrvSlots, p.srv);
    vScreenSetRenderTargetsRaw(ctx, boundCount(p.rtv), p.rtv, p.dsv);
    ++g_win.reissued;
    if (p.stage == kSun && p.eye >= 0) {
        Eye& e = g_eyes[p.eye];
        if (g_dump == Dump::Capturing) {
            // The private HDR right after the private sun draw, and the private depth.
            if (!e.stageHull) e.stageHull = stagingCopy(ctx, mirrorTexture(p.hdrGame), nullptr);
            if (!e.stageDepth) e.stageDepth = stagingCopy(ctx, mirrorTexture(e.depth), nullptr);
            e.dumpHull = e.hull;
        }
    }
    releaseTargets(p.rtv, p.dsv);
    for (auto*& s : p.srv)
        if (s) { s->Release(); s = nullptr; }
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
    ++g_frame;
    // The frame that just ended.
    bool anyBlock = false;
    for (auto& e : g_eyes) {
        if (!e.seen) continue;
        if (e.run == Run::In) ++g_win.ended[kEndFrame];
        if (e.shell) {
            ++g_win.eyeFrames;
            if (e.hull) {
                ++g_win.blocks;
                ++g_win.hist[std::min<uint32_t>(e.hull, 256)];
                anyBlock = true;
            }
        }
        g_win.gapDraws += e.gapDraws;
    }
    ++g_win.frames;
    if (!anyBlock) ++g_win.notFound;
    for (int s = 0; s < kStages; ++s) {
        if (g_noCpFrame[s]) ++g_win.noCounterpart[s];
        g_noCpFrame[s] = false;
    }
    if (g_dump == Dump::Capturing) {
        VrWorldInternalScope internal;   // the maps are the layer's own
        if (ctx) writeDump(ctx);
        g_dump = Dump::Idle;
    } else if (g_dump == Dump::Armed) {
        g_dump = Dump::Capturing;
        g_dumpFrame = g_frame;
    }
    // The next frame's eyes: the depth identities stay, so an eye keeps its index.
    for (auto& e : g_eyes) {
        e.seen = false;
        e.shell = false;
        e.run = Run::Before;
        e.hull = e.gap = e.gapDraws = 0;
        e.stagesDone = 0;
    }
    g_runEye = -1;
    const ULONGLONG now = GetTickCount64();
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
