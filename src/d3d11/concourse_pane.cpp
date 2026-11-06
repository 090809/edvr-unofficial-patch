// The concourse's glass panes: see concourse_pane.h. TEMPORARY (docs\concourse-cull-2026-11-05.md): removed when the arc closes.
#include "concourse_pane.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

#include "../common/config.h"
#include "../common/hotkey.h"
#include "../common/log.h"
#include "../common/runtime_profile.h"
#include "binding_shadow.h"
#include "concourse_cull_core.h"
#include "exposure_fix.h"
#include "flat_compute_readback.h"
#include "journal_watch.h"
#include "shader_swap.h"

namespace edvr {
namespace detail {
std::atomic<bool> g_concoursePaneArmed{false};
}  // namespace detail

namespace {

namespace cc = edvr::concourse;

enum Mode : uint32_t { kModeMirror = 0, kModeMirrorSoft = 1, kModeTint = 2, kModeOff = 3, kModeLit = 4, kModeBright = 5, kModeBrightest = 6,
                       kModeSteady25 = 7, kModeSteady50 = 8, kModeSteady100 = 9, kModeSteady200 = 10, kModeSteady400 = 11 };

// The lit modes' glow pass, as a factor on the white, rough material: how much of the room's own light the pane gives back on top of its mirror. That pass is the
// glass shader, so it carries the sun and its shadow map, and they move as the station turns.
float glowOf(uint32_t m) noexcept { return m == kModeLit ? 1.0f : m == kModeBright ? 2.0f : m == kModeBrightest ? 4.0f : 0.0f; }
// The steady modes' glow pass: a flat HDR colour added over the mirror, with no light or shadow in it, so it does not change as the station turns. The scene's HDR scale
// has not been measured, so the five steps bracket a guess of 1.0 by four times each way.
float steadyOf(uint32_t m) noexcept {
    return m == kModeSteady25 ? 0.25f : m == kModeSteady50 ? 0.5f : m == kModeSteady100 ? 1.0f : m == kModeSteady200 ? 2.0f : m == kModeSteady400 ? 4.0f : 0.0f;
}
constexpr float kSteadyColour[3] = {0.95f, 0.97f, 0.92f};   // a pale white, a little to the green of the station's light

const char* modeText(uint32_t m) noexcept {
    switch (m) {
        case kModeMirrorSoft: return "mirror-soft";
        case kModeTint: return "tint";
        case kModeOff: return "off";
        case kModeLit: return "mirror-lit";
        case kModeBright: return "mirror-bright";
        case kModeBrightest: return "mirror-brightest";
        case kModeSteady25: return "mirror-steady-25";
        case kModeSteady50: return "mirror-steady-50";
        case kModeSteady100: return "mirror-steady-100";
        case kModeSteady200: return "mirror-steady-200";
        case kModeSteady400: return "mirror-steady-400";
        default: return "mirror";
    }
}

bool parseMode(const std::string& t, uint32_t* out) noexcept {
    if (t == "mirror") *out = kModeMirror;
    else if (t == "mirror-soft") *out = kModeMirrorSoft;
    else if (t == "tint") *out = kModeTint;
    else if (t == "off") *out = kModeOff;
    else if (t == "mirror-lit") *out = kModeLit;
    else if (t == "mirror-bright") *out = kModeBright;
    else if (t == "mirror-brightest") *out = kModeBrightest;
    else if (t == "mirror-steady-25") *out = kModeSteady25;
    else if (t == "mirror-steady-50") *out = kModeSteady50;
    else if (t == "mirror-steady-100") *out = kModeSteady100;
    else if (t == "mirror-steady-200") *out = kModeSteady200;
    else if (t == "mirror-steady-400") *out = kModeSteady400;
    else return false;
    return true;
}

// Bits of ConcoursePaneScope::flags_.
constexpr uint32_t kFlagBlend = 1u << 0;
constexpr uint32_t kFlagPs = 1u << 1;
constexpr uint32_t kFlagSrv = 1u << 2;
constexpr uint32_t kFlagDepth = 1u << 3;
constexpr uint32_t kFlagCb = 1u << 4;
constexpr uint32_t kFlagQuery = 1u << 5;
constexpr uint32_t kFlagCapQuery = 1u << 6;
constexpr uint32_t kFlagTestDrawn = 1u << 7;    // the side test's first query (the game's glass draw) is open
constexpr uint32_t kFlagTestMirror = 1u << 8;   // the side test's second query (the mirror draw) is open

// ---- The list, the mode and the feed (the draw threads read them; the tick and the feed write them) --------------------------------------------
std::atomic<uint32_t> g_mode{kModeSteady100};
cc::PaneTriple g_list[cc::kPaneListCap] = {{cc::kPaneFamilyVs, cc::kPaneFamilyPs, 0}};   // entry 0 is the built-in glass family; diag.concourse_panes adds
std::atomic<uint32_t> g_listN{1};
std::atomic<bool> g_feedMeasure{false}, g_feedTreat{false}, g_feedWithhold{false}, g_feedSealed{false};
std::atomic<bool> g_withholdSealedNow{false};   // the hangar is sealed: only the families marked sealedToo are not issued
std::atomic<float> g_bayOpacity{1.0f};
std::atomic<bool> g_treatNow{false};      // treat fed, a mode other than off, and the replacement states made
std::atomic<bool> g_withholdNow{false};   // withhold fed (the cull is on): outside draws are not issued, whatever the mode; the mirror itself needs a mode other than off
std::atomic<bool> g_shareOn{false};       // measure fed and the query pool made
std::atomic<ID3D11DeviceContext*> g_immediate{nullptr};   // not held: the device's immediate context, the one the binding shadow follows
std::atomic<bool> g_capturing{false};     // a capture frame is open
std::atomic<uint64_t> g_treated{0};
std::atomic<uint64_t> g_withheld{0};
// One counter per withheld family (the pixel shader list, then the vertex shader list), read and cleared every 30 s.
constexpr size_t kWithheldFamilies = cc::kWithheldPsCount + cc::kWithheldVsCount;
std::atomic<uint64_t> g_withheldFam[kWithheldFamilies] = {};
std::atomic<uint64_t> g_farLimited{0};   // glows limited by the mark (farLimitedPs), read and cleared with the families
std::atomic<bool> g_surveyRequested{false};   // an F3 press, for the cull's names survey
uint64_t g_withheldLogMs = 0;
std::atomic<uint32_t> g_frameNow{0};
std::atomic<uint32_t> g_hashEpoch{1};

// The replacement states, made by the tick for the game's device.
std::atomic<ID3D11BlendState*> g_bsFactor{nullptr};   // src = BLEND_FACTOR, dest = ZERO (tint)
std::atomic<ID3D11BlendState*> g_bsMirror{nullptr};   // src = ONE, dest = ZERO (a fully opaque pane)
std::atomic<ID3D11BlendState*> g_bsGlow{nullptr};     // src = BLEND_FACTOR, dest = ONE (a lit mode's glow, over the mirror)
std::atomic<ID3D11BlendState*> g_bsFade{nullptr};     // src = BLEND_FACTOR, dest = INV_BLEND_FACTOR (a pane below full opacity)
std::atomic<ID3D11PixelShader*> g_psWhite{nullptr};   // tint: the blend factor multiplies this, so it is the pane's colour
std::atomic<ID3D11ShaderResourceView*> g_mirrorSrv[3] = {};   // t8, t9, t10
std::atomic<ID3D11ShaderResourceView*> g_glowT10{nullptr};    // t10 of the glow pass; its t8 and t9 are the mirror's
std::atomic<ID3D11Buffer*> g_cbMirror{nullptr};       // mirror-soft: the pixel shader's constant buffer 2
std::atomic<bool> g_statesReady{false};
bool g_psWhiteTried = false;   // the tick's: one compile per device

// The mark pass's own objects (made by the tick; the draw threads read the atomics).
std::atomic<ID3D11VertexShader*> g_vsMark{nullptr};      // the fullscreen triangle at the beyond depth
std::atomic<ID3D11RasterizerState*> g_rsMark{nullptr};   // solid, no cull, depth clip, no scissor
std::atomic<ID3D11BlendState*> g_bsMark{nullptr};        // no colour write
std::atomic<ID3D11DepthStencilState*> g_dsMark{nullptr};   // depth test greater-equal, no depth write, stencil replace 0x40
std::atomic<bool> g_markReady{false};
std::atomic<float> g_beyondM{cc::kMirrorBeyondDefaultM};   // diag.concourse_mirror_beyond
float g_vsMarkM = 0.0f;       // the metres the compiled vertex shader holds (the tick's)
bool g_vsMarkTried = false;
bool g_markStatesTried = false;
std::string g_beyondAppliedText;
bool g_beyondTextSeen = false;
constexpr UINT kMarkRef = 0x40;
std::atomic<uint64_t> g_markPasses{0}, g_markRefused{0}, g_testsRead{0};
// The frame the plain mirror phase last began. For kMarkSettleFrames after it, a pane known to be exterior stays whole: the frame in flight may still hold the
// depth of the structure the cull has just taken away, and the mark would open holes in the pane for that frame.
constexpr uint32_t kMarkSettleFrames = 8;
std::atomic<uint32_t> g_plainFrom{0};

constexpr UINT kPaneSrvFirst = 8;
constexpr UINT kPaneSrvCount = 3;
constexpr UINT kPaneCbSlot = 2;
constexpr UINT kPaneCbBytes = 48;

// ---- The capture ------------------------------------------------------------------------------------------------------------------------------
constexpr size_t kPool = 1024;
struct CapDraw {
    uint64_t vs = 0, ps = 0;
    uint32_t n = 0;
    uint32_t vpW = 0, vpH = 0;
    uint32_t src = 0, dst = 0;
    int depthWrite = 0;
    uint32_t stencil = 0;
    bool arrived = false;
    uint64_t samples = 0;
};
ID3D11Query* g_pool[kPool] = {};
CapDraw g_cap[kPool];
size_t g_capN = 0;           // under g_capMutex while capturing; the tick's after
uint64_t g_capDropped = 0;   // qualifying draws past the pool
std::mutex g_capMutex;       // taken only while a capture is open
enum class CapState : uint8_t { kIdle, kOpen, kReading };
CapState g_capState = CapState::kIdle;
uint32_t g_capFrame = 0;
bool g_poolMade = false;
bool g_capPressPending = false;

// ---- The share measurement: an occlusion query around each family draw on the immediate context -------------------------------------------------
constexpr size_t kShareSlots = 48;
constexpr size_t kBins = 8;
constexpr uint32_t kBinMaxN = 8;
struct ShareSlot {
    ID3D11Query* q = nullptr;
    uint8_t state = 0;   // 0 free, 1 open (between Begin and End), 2 ended and not yet read
    uint32_t frame = 0;
    uint32_t n = 0;
    double pixels = 0.0;
};
struct Bin {
    bool used = false;
    uint32_t frame = 0;
    uint32_t outstanding = 0;
    uint32_t count = 0;
    uint32_t n[kBinMaxN] = {};
    float share[kBinMaxN] = {};
};
std::mutex g_shareMutex;
ShareSlot g_slots[kShareSlots];
Bin g_bins[kBins];
bool g_slotsMade = false;
ConcoursePaneShares g_latest;
uint64_t g_latestMs = 0;
uint32_t g_latestFrame = 0;
uint32_t g_seqCounter = 0;
uint64_t g_shareDropped = 0;

// ---- The one-shot material log ---------------------------------------------------------------------------------------------------------------
constexpr size_t kMatMax = 4;
struct MatPending {
    ID3D11Buffer* staging = nullptr;
    uint32_t dueFrame = 0;
    uint32_t tries = 0;
    char text[760] = {};
};
std::mutex g_matMutex;
uint32_t g_matSeenN[kMatMax] = {};
size_t g_matSeen = 0;
MatPending g_mat[kMatMax];

// ---- The tint colours: one per distinct index count, in the order seen -------------------------------------------------------------------------------
std::mutex g_tintMutex;
uint32_t g_tintN[cc::kPaneColourCount] = {};
size_t g_tintCount = 0;

// ---- The side tests and the table of each pane's side ----------------------------------------------------------------------------------------------
// A test is two occlusion queries around two draws of one pane: the game's glass, then the mirror (marked). The ring holds 16 tests; the tick reads what arrived.
constexpr size_t kTestSlots = 16;
struct TestSlot {
    ID3D11Query* qDrawn = nullptr;
    ID3D11Query* qMirror = nullptr;
    uint8_t state = 0;   // 0 free, 1 the game's draw open, 2 closed and waiting for the mirror, 3 the mirror open, 4 both closed, not yet read
    uint32_t frame = 0;
    cc::PaneKey key;
};
std::mutex g_sideMutex;   // the table and the ring: the draw thread and the tick both touch them
TestSlot g_tests[kTestSlots];
cc::PaneSides g_sides;
bool g_testsMade = false;
std::atomic<bool> g_testsOn{false};
uint32_t g_sideLogExterior = 0, g_sideLogOther = 0;   // lines said this session, each capped
constexpr uint32_t kSideLogCap = 96;

// The mark pass runs at most once per (frame, depth view, viewport): the draw thread's (the immediate context only).
struct MarkKey {
    uint32_t frame = 0;
    ID3D11DepthStencilView* dsv = nullptr;   // identity only, never dereferenced
    float vp[4] = {};
    bool valid = false;
};
MarkKey g_markKey;
bool g_markLogged = false;
bool g_markNoStencilLogged = false;

// ---- The derived depth states: the game's, with depth writes on ----------------------------------------------------------------------------------
// Two variants per original state: `plain` (depth write on, stencil off) for an opaque pane drawn everywhere, and `marked` (depth write on, stencil read mask 0x40,
// equal to the reference, no stencil write) for an opaque pane drawn where the mark pass set bit 0x40.
struct DsEntry {
    ID3D11DepthStencilState* orig = nullptr;   // held, so the pointer cannot be reused for another state
    ID3D11DepthStencilState* plain = nullptr;
    ID3D11DepthStencilState* marked = nullptr;
    ID3D11DepthStencilState* farOut = nullptr;
};
enum class DsVariant { kPlain, kMarked, kFarOut };
std::mutex g_dsMutex;
DsEntry g_ds[16];
size_t g_dsN = 0;

// The tick's state.
ID3D11Device* g_device = nullptr;
bool g_social = false;
uint64_t g_nextPollMs = 0;
Hotkey* g_keyCapture = nullptr;
Hotkey* g_keyMode = nullptr;
std::string g_keyCaptureText, g_keyModeText;
bool g_keysBound = false;
std::string g_modeAppliedText, g_panesAppliedText;
bool g_modeTextSeen = false, g_panesTextSeen = false;
uint64_t g_lastTickMs = 0;

// ---- Shader hashes, cached by pointer per thread (cleared about once a second) ----------------------------------------------------------------
struct HashCache {
    void* key[64] = {};
    uint64_t hash[64] = {};
    uint32_t epoch = 0;
};
thread_local HashCache t_hashCache;

uint64_t cachedHash(void* shader) noexcept {
    HashCache& c = t_hashCache;
    const uint32_t epoch = g_hashEpoch.load(std::memory_order_relaxed);
    if (c.epoch != epoch) {
        std::memset(c.key, 0, sizeof(c.key));
        c.epoch = epoch;
    }
    const size_t i = (reinterpret_cast<uintptr_t>(shader) >> 4) & 63u;
    if (c.key[i] == shader) return c.hash[i];
    const uint64_t h = lookupShaderHash(shader);
    c.key[i] = shader;
    c.hash[i] = h;
    return h;
}

uint64_t pixelShaderHash(ID3D11DeviceContext* ctx) noexcept {
    ID3D11PixelShader* p = nullptr;
    ctx->PSGetShader(&p, nullptr, nullptr);
    if (!p) return 0;
    const uint64_t h = cachedHash(p);
    p->Release();
    return h;
}

uint64_t vertexShaderHash(ID3D11DeviceContext* ctx) noexcept {
    ID3D11VertexShader* v = nullptr;
    ctx->VSGetShader(&v, nullptr, nullptr);
    if (!v) return 0;
    const uint64_t h = cachedHash(v);
    v->Release();
    return h;
}

void shaderHashes(ID3D11DeviceContext* ctx, uint64_t* vs, uint64_t* ps) noexcept {
    ID3D11VertexShader* v = nullptr;
    ID3D11PixelShader* p = nullptr;
    ctx->VSGetShader(&v, nullptr, nullptr);
    ctx->PSGetShader(&p, nullptr, nullptr);
    *vs = v ? lookupShaderHash(v) : 0;
    *ps = p ? lookupShaderHash(p) : 0;
    if (v) v->Release();
    if (p) p->Release();
}

void releaseStates() noexcept {
    // The draw threads may hold the pointers for one more draw, and the old device owns the objects: not released (a device change is rare).
    g_statesReady.store(false, std::memory_order_release);
    g_bsFactor.store(nullptr, std::memory_order_release);
    g_bsMirror.store(nullptr, std::memory_order_release);
    g_bsFade.store(nullptr, std::memory_order_release);
    g_bsGlow.store(nullptr, std::memory_order_release);
    g_glowT10.store(nullptr, std::memory_order_release);
    g_psWhite.store(nullptr, std::memory_order_release);
    for (auto& s : g_mirrorSrv) s.store(nullptr, std::memory_order_release);
    g_cbMirror.store(nullptr, std::memory_order_release);
    g_psWhiteTried = false;
    g_markReady.store(false, std::memory_order_release);
    g_vsMark.store(nullptr, std::memory_order_release);
    g_rsMark.store(nullptr, std::memory_order_release);
    g_bsMark.store(nullptr, std::memory_order_release);
    g_dsMark.store(nullptr, std::memory_order_release);
    g_vsMarkTried = false;
    g_vsMarkM = 0.0f;
    g_markStatesTried = false;
    {
        std::lock_guard<std::mutex> lock(g_dsMutex);
        for (auto& e : g_ds) e = DsEntry{};
        g_dsN = 0;
    }
    {
        std::lock_guard<std::mutex> lock(g_sideMutex);
        g_testsOn.store(false, std::memory_order_release);
        for (auto& t : g_tests) t = TestSlot{};
        g_testsMade = false;
        g_sides.reset();
        g_markKey = MarkKey{};
    }
    {
        std::lock_guard<std::mutex> lock(g_shareMutex);
        g_shareOn.store(false, std::memory_order_release);
        for (auto& s : g_slots) s = ShareSlot{};
        for (auto& b : g_bins) b = Bin{};
        g_slotsMade = false;
        g_latest = ConcoursePaneShares{};
    }
    {
        std::lock_guard<std::mutex> lock(g_matMutex);
        for (auto& m : g_mat) m = MatPending{};
    }
}

// A failed compile leaves tint multiplying the game's own colour, and the log says so.
void makeWhitePs(ID3D11Device* device) noexcept {
    if (g_psWhiteTried) return;
    g_psWhiteTried = true;
    ID3D11DeviceContext* imm = nullptr;
    device->GetImmediateContext(&imm);
    if (!imm) return;
    static const char kWhite[] = "float4 main() : SV_Target { return float4(1.0, 1.0, 1.0, 1.0); }\n";
    ID3D11PixelShader* ps = shaderSwapCompilePs(imm, kWhite, sizeof(kWhite) - 1, "main", "concourse_pane_white", nullptr, "concourse pane");
    imm->Release();
    g_psWhite.store(ps, std::memory_order_release);
    if (!ps) Log::get().note("concourse pane: the white pixel shader did not compile; tint multiplies the game's own colour, so a dark pane stays dark");
}

bool makeBlendStates(ID3D11Device* device) noexcept {
    D3D11_BLEND_DESC d{};
    d.AlphaToCoverageEnable = FALSE;
    d.IndependentBlendEnable = FALSE;
    D3D11_RENDER_TARGET_BLEND_DESC& rt = d.RenderTarget[0];
    rt.BlendEnable = TRUE;
    rt.BlendOp = D3D11_BLEND_OP_ADD;
    rt.SrcBlendAlpha = D3D11_BLEND_ONE;
    rt.DestBlendAlpha = D3D11_BLEND_ZERO;
    rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
    rt.RenderTargetWriteMask = 0x7;   // red, green and blue: the alpha channel is left alone
    ID3D11BlendState* factor = nullptr;
    ID3D11BlendState* mirror = nullptr;
    ID3D11BlendState* fade = nullptr;
    ID3D11BlendState* glow = nullptr;
    rt.SrcBlend = D3D11_BLEND_BLEND_FACTOR;
    rt.DestBlend = D3D11_BLEND_ZERO;
    const HRESULT h1 = device->CreateBlendState(&d, &factor);
    rt.SrcBlend = D3D11_BLEND_ONE;
    const HRESULT h2 = device->CreateBlendState(&d, &mirror);
    rt.SrcBlend = D3D11_BLEND_BLEND_FACTOR;
    rt.DestBlend = D3D11_BLEND_INV_BLEND_FACTOR;
    const HRESULT h3 = device->CreateBlendState(&d, &fade);
    rt.DestBlend = D3D11_BLEND_ONE;
    const HRESULT h4 = device->CreateBlendState(&d, &glow);
    if (FAILED(h1) || FAILED(h2) || FAILED(h3) || FAILED(h4) || !factor || !mirror || !fade || !glow) {
        Log::get().note("concourse pane: the replacement blend states could not be made (hr %08X, %08X, %08X); no pane is treated", static_cast<unsigned>(h1),
                        static_cast<unsigned>(h2), static_cast<unsigned>(h3));
        if (factor) factor->Release();
        if (mirror) mirror->Release();
        if (fade) fade->Release();
        if (glow) glow->Release();
        return false;
    }
    g_bsMirror.store(mirror, std::memory_order_release);
    g_bsFactor.store(factor, std::memory_order_release);
    g_bsFade.store(fade, std::memory_order_release);
    g_bsGlow.store(glow, std::memory_order_release);
    return true;
}

bool makeMirrorTextures(ID3D11Device* device) noexcept {
    static uint8_t t8[cc::kPaneTexSize * cc::kPaneTexSize * 2];
    static uint8_t t9[cc::kPaneTexSize * cc::kPaneTexSize * 2];
    static uint8_t t10[cc::kPaneTexSize * cc::kPaneTexSize * 4];
    static uint8_t glow[cc::kPaneTexSize * cc::kPaneTexSize * 4];
    cc::fillPaneT8(t8);
    cc::fillPaneT9(t9);
    cc::fillPaneT10(t10);
    cc::fillPaneGlowT10(glow);
    struct Spec {
        DXGI_FORMAT format;
        const uint8_t* data;
        UINT bytes;
    };
    const Spec specs[4] = {{DXGI_FORMAT_R8G8_UNORM, t8, 2}, {DXGI_FORMAT_R8G8_UNORM, t9, 2}, {DXGI_FORMAT_R8G8B8A8_UNORM, t10, 4}, {DXGI_FORMAT_R8G8B8A8_UNORM, glow, 4}};
    ID3D11ShaderResourceView* made[4] = {};
    bool ok = true;
    for (int i = 0; i < 4 && ok; ++i) {
        D3D11_TEXTURE2D_DESC td{};
        td.Width = cc::kPaneTexSize;
        td.Height = cc::kPaneTexSize;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = specs[i].format;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_IMMUTABLE;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA sd{};
        sd.pSysMem = specs[i].data;
        sd.SysMemPitch = cc::kPaneTexSize * specs[i].bytes;
        ID3D11Texture2D* tex = nullptr;
        if (FAILED(device->CreateTexture2D(&td, &sd, &tex)) || !tex) {
            ok = false;
            break;
        }
        D3D11_SHADER_RESOURCE_VIEW_DESC vd{};
        vd.Format = specs[i].format;
        vd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
        vd.Texture2DArray.MostDetailedMip = 0;
        vd.Texture2DArray.MipLevels = 1;
        vd.Texture2DArray.FirstArraySlice = 0;
        vd.Texture2DArray.ArraySize = 1;
        const HRESULT h = device->CreateShaderResourceView(tex, &vd, &made[i]);
        tex->Release();
        if (FAILED(h) || !made[i]) ok = false;
    }
    if (!ok) {
        for (auto* v : made)
            if (v) v->Release();
        Log::get().note("concourse pane: the mirror textures could not be made; no pane is treated");
        return false;
    }
    g_glowT10.store(made[3], std::memory_order_release);
    for (int i = 0; i < 3; ++i) g_mirrorSrv[i].store(made[i], std::memory_order_release);
    return true;
}

bool makeMirrorCb(ID3D11Device* device) noexcept {
    static const float kRows[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1.8f, 0};   // row 2: F0 scale 1, roughness scale 1.8 (0.30 * 1.8 = 0.54)
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = kPaneCbBytes;
    bd.Usage = D3D11_USAGE_IMMUTABLE;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    D3D11_SUBRESOURCE_DATA sd{};
    sd.pSysMem = kRows;
    ID3D11Buffer* b = nullptr;
    const HRESULT h = device->CreateBuffer(&bd, &sd, &b);
    if (FAILED(h) || !b) {
        Log::get().note("concourse pane: the mirror constant buffer could not be made (hr %08X); mirror-soft is not available", static_cast<unsigned>(h));
        return false;
    }
    g_cbMirror.store(b, std::memory_order_release);
    return true;
}

// The mark pass's vertex shader: one fullscreen triangle at the depth of the beyond distance (reversed Z: 0.025 / metres). Recompiled when the key changes; the
// old shader is not released (a draw thread may hold it for one more draw, and a change of the key is a developer's act).
void ensureMarkVs(ID3D11Device* device) noexcept {
    const float want = g_beyondM.load(std::memory_order_relaxed);
    if (g_vsMarkTried && g_vsMarkM == want) return;
    g_vsMarkTried = true;
    g_vsMarkM = want;
    ID3D11DeviceContext* imm = nullptr;
    device->GetImmediateContext(&imm);
    if (!imm) return;
    const float z = cc::markDepthForDistance(want);
    char hlsl[400] = {};
    const int len = std::snprintf(hlsl, sizeof(hlsl),
                                  "float4 main(uint id : SV_VertexID) : SV_Position { float2 p = float2((id << 1) & 2, id & 2); return float4(p.x * 2 - 1, 1 - p.y * 2, %.9g, 1); }\n",
                                  static_cast<double>(z));
    ID3D11VertexShader* vs = len > 0 ? shaderSwapCompileVs(imm, hlsl, static_cast<size_t>(len), "main", "concourse_pane_mark", nullptr, "concourse pane") : nullptr;
    imm->Release();
    if (!vs) {
        Log::get().note("concourse pane: the mark vertex shader did not compile (beyond %.0f m); the mirror cannot be limited to the far pixels, so it does not draw", static_cast<double>(want));
        g_vsMark.store(nullptr, std::memory_order_release);
        return;
    }
    g_vsMark.store(vs, std::memory_order_release);
}

bool makeMarkStates(ID3D11Device* device) noexcept {
    if (g_markStatesTried) return g_rsMark.load(std::memory_order_acquire) != nullptr;
    g_markStatesTried = true;
    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    rd.ScissorEnable = FALSE;
    D3D11_BLEND_DESC bd{};
    bd.RenderTarget[0].BlendEnable = FALSE;
    bd.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
    bd.RenderTarget[0].DestBlend = D3D11_BLEND_ZERO;
    bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
    bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].RenderTargetWriteMask = 0;
    D3D11_DEPTH_STENCIL_DESC dd{};
    dd.DepthEnable = TRUE;
    dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    dd.DepthFunc = D3D11_COMPARISON_GREATER_EQUAL;
    dd.StencilEnable = TRUE;
    dd.StencilReadMask = 0;
    dd.StencilWriteMask = static_cast<UINT8>(kMarkRef);
    for (D3D11_DEPTH_STENCILOP_DESC* f : {&dd.FrontFace, &dd.BackFace}) {
        f->StencilFunc = D3D11_COMPARISON_ALWAYS;
        f->StencilFailOp = D3D11_STENCIL_OP_KEEP;
        f->StencilDepthFailOp = D3D11_STENCIL_OP_KEEP;
        f->StencilPassOp = D3D11_STENCIL_OP_REPLACE;
    }
    ID3D11RasterizerState* rs = nullptr;
    ID3D11BlendState* bs = nullptr;
    ID3D11DepthStencilState* ds = nullptr;
    const HRESULT h1 = device->CreateRasterizerState(&rd, &rs);
    const HRESULT h2 = device->CreateBlendState(&bd, &bs);
    const HRESULT h3 = device->CreateDepthStencilState(&dd, &ds);
    if (FAILED(h1) || FAILED(h2) || FAILED(h3) || !rs || !bs || !ds) {
        Log::get().note("concourse pane: the mark pass's states could not be made (hr %08X, %08X, %08X); the mirror does not draw", static_cast<unsigned>(h1),
                        static_cast<unsigned>(h2), static_cast<unsigned>(h3));
        if (rs) rs->Release();
        if (bs) bs->Release();
        if (ds) ds->Release();
        return false;
    }
    g_bsMark.store(bs, std::memory_order_release);
    g_dsMark.store(ds, std::memory_order_release);
    g_rsMark.store(rs, std::memory_order_release);
    return true;
}

bool makeTests(ID3D11Device* device) noexcept {
    std::lock_guard<std::mutex> lock(g_sideMutex);
    if (g_testsMade) return true;
    FlatComputeInternalScope internal;
    D3D11_QUERY_DESC qd{};
    qd.Query = D3D11_QUERY_OCCLUSION;
    for (size_t i = 0; i < kTestSlots; ++i) {
        if (FAILED(device->CreateQuery(&qd, &g_tests[i].qDrawn)) || !g_tests[i].qDrawn || FAILED(device->CreateQuery(&qd, &g_tests[i].qMirror)) || !g_tests[i].qMirror) {
            Log::get().note("concourse pane: the side test queries could not be made (test %u of %u); the panes' sides are not learned", static_cast<unsigned>(i),
                            static_cast<unsigned>(kTestSlots));
            for (size_t k = 0; k <= i; ++k) {
                if (g_tests[k].qDrawn) g_tests[k].qDrawn->Release();
                if (g_tests[k].qMirror) g_tests[k].qMirror->Release();
                g_tests[k] = TestSlot{};
            }
            return false;
        }
    }
    g_testsMade = true;
    return true;
}

bool makeStates(ID3D11Device* device) noexcept {
    if (!device) return false;
    if (g_statesReady.load(std::memory_order_acquire)) return true;
    makeWhitePs(device);
    makeMarkStates(device);
    ensureMarkVs(device);
    g_markReady.store(g_rsMark.load(std::memory_order_acquire) != nullptr && g_vsMark.load(std::memory_order_acquire) != nullptr, std::memory_order_release);
    if (!g_bsMirror.load(std::memory_order_acquire) && !makeBlendStates(device)) return false;
    if (!g_mirrorSrv[0].load(std::memory_order_acquire) && !makeMirrorTextures(device)) return false;
    if (!g_cbMirror.load(std::memory_order_acquire)) makeMirrorCb(device);   // mirror-soft only: a failure is logged and the plain mirror carries on
    g_statesReady.store(true, std::memory_order_release);
    return true;
}

bool makeSlots(ID3D11Device* device) noexcept {
    std::lock_guard<std::mutex> lock(g_shareMutex);
    if (g_slotsMade) return true;
    FlatComputeInternalScope internal;
    D3D11_QUERY_DESC qd{};
    qd.Query = D3D11_QUERY_OCCLUSION;
    for (size_t i = 0; i < kShareSlots; ++i) {
        if (FAILED(device->CreateQuery(&qd, &g_slots[i].q)) || !g_slots[i].q) {
            Log::get().note("concourse pane: the share query pool could not be made (query %u of %u); the shares are not measured", static_cast<unsigned>(i),
                            static_cast<unsigned>(kShareSlots));
            for (size_t k = 0; k < i; ++k) {
                if (g_slots[k].q) g_slots[k].q->Release();
                g_slots[k] = ShareSlot{};
            }
            return false;
        }
    }
    g_slotsMade = true;
    return true;
}

bool makePool(ID3D11Device* device) noexcept {
    if (g_poolMade) return true;
    FlatComputeInternalScope internal;
    D3D11_QUERY_DESC qd{};
    qd.Query = D3D11_QUERY_OCCLUSION;
    for (size_t i = 0; i < kPool; ++i) {
        if (FAILED(device->CreateQuery(&qd, &g_pool[i])) || !g_pool[i]) {
            Log::get().note("concourse pane capture: the query pool could not be made (query %u of %u)", static_cast<unsigned>(i), static_cast<unsigned>(kPool));
            for (size_t k = 0; k < i; ++k) {
                if (g_pool[k]) g_pool[k]->Release();
                g_pool[k] = nullptr;
            }
            return false;
        }
    }
    g_poolMade = true;
    return true;
}

// ---- The share bins (under g_shareMutex) -------------------------------------------------------------------------------------------------------------
Bin* binFor(uint32_t frame, bool create) noexcept {
    for (auto& b : g_bins)
        if (b.used && b.frame == frame) return &b;
    if (!create) return nullptr;
    Bin* pick = nullptr;
    for (auto& b : g_bins)
        if (!b.used) {
            pick = &b;
            break;
        }
    if (!pick) {
        pick = &g_bins[0];
        for (auto& b : g_bins)
            if (b.frame < pick->frame) pick = &b;
    }
    *pick = Bin{};
    pick->used = true;
    pick->frame = frame;
    return pick;
}

// Opens a query around a family draw (immediate context only). Returns the slot, or -1.
int openShareQuery(ID3D11DeviceContext* ctx, uint32_t n) noexcept {
    D3D11_VIEWPORT vp{};
    UINT nvp = 1;
    ctx->RSGetViewports(&nvp, &vp);
    if (nvp < 1 || !(vp.Width > 0.0f) || !(vp.Height > 0.0f)) return -1;
    int idx = -1;
    {
        std::lock_guard<std::mutex> lock(g_shareMutex);
        if (!g_slotsMade) return -1;
        for (size_t i = 0; i < kShareSlots; ++i)
            if (g_slots[i].state == 0 && g_slots[i].q) {
                idx = static_cast<int>(i);
                break;
            }
        if (idx < 0) {
            ++g_shareDropped;
            return -1;
        }
        const uint32_t frame = g_frameNow.load(std::memory_order_relaxed);
        ShareSlot& s = g_slots[idx];
        s.state = 1;
        s.frame = frame;
        s.n = n;
        s.pixels = static_cast<double>(vp.Width) * static_cast<double>(vp.Height);
        Bin* b = binFor(frame, true);
        if (b) ++b->outstanding;
    }
    FlatComputeInternalScope internal;
    ctx->Begin(g_slots[idx].q);
    return idx;
}

void closeShareQuery(ID3D11DeviceContext* ctx, int idx) noexcept {
    {
        FlatComputeInternalScope internal;
        ctx->End(g_slots[idx].q);
    }
    std::lock_guard<std::mutex> lock(g_shareMutex);
    g_slots[idx].state = 2;
}

// The tick: reads what arrived, closes the frames that are complete. `measuring` opens an (empty) bin for the frame just ended, so a frame with no family draw
// is a reading of zero rather than no reading.
void pollShares(ID3D11Device* device, uint32_t prevFrame, uint32_t frame, bool measuring, uint64_t nowMs) noexcept {
    ID3D11DeviceContext* imm = nullptr;
    device->GetImmediateContext(&imm);
    if (!imm) return;
    {
        std::lock_guard<std::mutex> lock(g_shareMutex);
        if (!g_slotsMade) {
            imm->Release();
            return;
        }
        if (measuring) binFor(prevFrame, true);
        FlatComputeInternalScope internal;
        for (auto& s : g_slots) {
            if (s.state != 2) continue;
            UINT64 samples = 0;
            if (imm->GetData(s.q, &samples, sizeof(samples), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) continue;
            Bin* b = binFor(s.frame, false);
            if (b) {
                const float share = s.pixels > 0.0 ? static_cast<float>(static_cast<double>(samples) / s.pixels) : 0.0f;
                uint32_t k = 0;
                while (k < b->count && b->n[k] != s.n) ++k;
                if (k == b->count && k < kBinMaxN) {
                    b->n[k] = s.n;
                    b->share[k] = share;
                    ++b->count;
                } else if (k < b->count && share > b->share[k]) {
                    b->share[k] = share;
                }
                if (b->outstanding > 0) --b->outstanding;
            }
            s.state = 0;
        }
        Bin* best = nullptr;
        for (auto& b : g_bins) {
            if (!b.used) continue;
            if (b.frame < frame && b.outstanding == 0) {
                if (!best || b.frame > best->frame) best = &b;
            } else if (frame - b.frame > 120u) {
                b.used = false;
            }
        }
        if (best && measuring && (!g_latest.valid || best->frame >= g_latestFrame)) {
            ConcoursePaneShares r;
            r.valid = true;
            r.seq = ++g_seqCounter;
            r.bay = 0.0f;
            r.count = best->count;
            for (uint32_t k = 0; k < best->count; ++k) {
                r.n[k] = best->n[k];
                r.share[k] = best->share[k];
                if (best->n[k] == cc::kBayIndexCount && best->share[k] > r.bay) r.bay = best->share[k];
            }
            g_latest = r;
            g_latestMs = nowMs;
            g_latestFrame = best->frame;
        }
        if (best) {
            for (auto& b : g_bins)
                if (b.used && b.frame <= best->frame && b.outstanding == 0 && b.frame < frame) b.used = false;
        }
        if (!measuring) {
            g_latest = ConcoursePaneShares{};
            for (auto& b : g_bins) b = Bin{};
        }
    }
    imm->Release();
}

// ---- The derived depth state -------------------------------------------------------------------------------------------------------------------
// nullptr when the variant cannot be made (a full cache, or the device refused): the caller does not draw the mirror then.
// kPlain and kMarked are the mirror's: depth write on, without and with the mark's test. kFarOut is the far-limited glows': the game's depth state as it is, and only
// the pixels the mark did not set.
ID3D11DepthStencilState* derivedDepth(ID3D11DeviceContext* ctx, ID3D11DepthStencilState* orig, DsVariant variant) noexcept {
    std::lock_guard<std::mutex> lock(g_dsMutex);
    size_t at = g_dsN;
    for (size_t i = 0; i < g_dsN; ++i)
        if (g_ds[i].orig == orig) {
            at = i;
            break;
        }
    if (at == g_dsN) {
        if (g_dsN >= sizeof(g_ds) / sizeof(g_ds[0])) return nullptr;
        orig->AddRef();
        g_ds[g_dsN] = DsEntry{};
        g_ds[g_dsN].orig = orig;
        ++g_dsN;
    }
    ID3D11DepthStencilState*& slot = variant == DsVariant::kFarOut ? g_ds[at].farOut : variant == DsVariant::kMarked ? g_ds[at].marked : g_ds[at].plain;
    if (slot) return slot;
    D3D11_DEPTH_STENCIL_DESC d{};
    orig->GetDesc(&d);
    if (variant != DsVariant::kFarOut) d.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    if (variant != DsVariant::kPlain) {
        d.StencilEnable = TRUE;
        d.StencilReadMask = static_cast<UINT8>(kMarkRef);
        d.StencilWriteMask = 0;
        for (D3D11_DEPTH_STENCILOP_DESC* f : {&d.FrontFace, &d.BackFace}) {
            f->StencilFunc = variant == DsVariant::kFarOut ? D3D11_COMPARISON_NOT_EQUAL : D3D11_COMPARISON_EQUAL;
            f->StencilFailOp = D3D11_STENCIL_OP_KEEP;
            f->StencilDepthFailOp = D3D11_STENCIL_OP_KEEP;
            f->StencilPassOp = D3D11_STENCIL_OP_KEEP;
        }
    } else {
        d.StencilEnable = FALSE;
        d.StencilWriteMask = 0;
    }
    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    ID3D11DepthStencilState* derived = nullptr;
    if (dev) {
        if (FAILED(dev->CreateDepthStencilState(&d, &derived))) derived = nullptr;
        dev->Release();
    }
    slot = derived;   // a refusal is remembered as null: the variant is not tried again
    return derived;
}

// ---- The tint colour of an index count ---------------------------------------------------------------------------------------------------------
size_t tintSlotOf(uint32_t n) noexcept {
    size_t slot = 0;
    bool fresh = false;
    {
        std::lock_guard<std::mutex> lock(g_tintMutex);
        size_t i = 0;
        while (i < g_tintCount && g_tintN[i] != n) ++i;
        if (i < g_tintCount) {
            slot = i;
        } else if (g_tintCount < cc::kPaneColourCount) {
            g_tintN[g_tintCount] = n;
            slot = g_tintCount++;
            fresh = true;
        } else {
            slot = cc::kPaneColourCount - 1;   // the table is full: the last colour repeats
        }
    }
    if (fresh) Log::get().note("concourse pane: n=%u colour %s", n, cc::kPaneColourNames[slot % cc::kPaneColourCount]);
    return slot;
}

// ---- The one-shot material log ---------------------------------------------------------------------------------------------------------------
const char* formatName(DXGI_FORMAT f) noexcept {
    switch (f) {
        case DXGI_FORMAT_R8G8_UNORM: return "R8G8_UNORM";
        case DXGI_FORMAT_R8G8B8A8_UNORM: return "R8G8B8A8_UNORM";
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return "R8G8B8A8_UNORM_SRGB";
        case DXGI_FORMAT_R8_UNORM: return "R8_UNORM";
        case DXGI_FORMAT_BC1_UNORM: return "BC1_UNORM";
        case DXGI_FORMAT_BC1_UNORM_SRGB: return "BC1_UNORM_SRGB";
        case DXGI_FORMAT_BC3_UNORM: return "BC3_UNORM";
        case DXGI_FORMAT_BC3_UNORM_SRGB: return "BC3_UNORM_SRGB";
        case DXGI_FORMAT_BC5_UNORM: return "BC5_UNORM";
        case DXGI_FORMAT_BC7_UNORM: return "BC7_UNORM";
        case DXGI_FORMAT_BC7_UNORM_SRGB: return "BC7_UNORM_SRGB";
        case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: return "D32_FLOAT_S8X24_UINT";
        case DXGI_FORMAT_D24_UNORM_S8_UINT: return "D24_UNORM_S8_UINT";
        case DXGI_FORMAT_D32_FLOAT: return "D32_FLOAT";
        case DXGI_FORMAT_D16_UNORM: return "D16_UNORM";
        case DXGI_FORMAT_R32G8X24_TYPELESS: return "R32G8X24_TYPELESS";
        case DXGI_FORMAT_R24G8_TYPELESS: return "R24G8_TYPELESS";
        case DXGI_FORMAT_UNKNOWN: return "UNKNOWN";
        default: return nullptr;
    }
}

void describeSrv(ID3D11ShaderResourceView* v, UINT slot, char* out, size_t cap) noexcept {
    if (!v) {
        std::snprintf(out, cap, "t%u none", slot);
        return;
    }
    D3D11_SHADER_RESOURCE_VIEW_DESC vd{};
    v->GetDesc(&vd);
    ID3D11Resource* r = nullptr;
    v->GetResource(&r);
    ID3D11Texture2D* t = nullptr;
    if (r) r->QueryInterface(IID_PPV_ARGS(&t));
    if (t) {
        D3D11_TEXTURE2D_DESC td{};
        t->GetDesc(&td);
        const char* fn = formatName(td.Format);
        char fmt[24] = {};
        if (fn) std::snprintf(fmt, sizeof(fmt), "%s", fn);
        else std::snprintf(fmt, sizeof(fmt), "format %u", static_cast<unsigned>(td.Format));
        std::snprintf(out, cap, "t%u %s %ux%u array=%u mips=%u view=%u", slot, fmt, td.Width, td.Height, td.ArraySize, td.MipLevels,
                      static_cast<unsigned>(vd.ViewDimension));
        t->Release();
    } else {
        std::snprintf(out, cap, "t%u not a Texture2D (view dimension %u)", slot, static_cast<unsigned>(vd.ViewDimension));
    }
    if (r) r->Release();
}

// The first sight of an index count of the family: the game's three textures at slots 8 to 10 and constant buffer 2, on the immediate context. The buffer is
// copied to a staging buffer (from its first-constant offset) and read a few frames later by the tick.
void materialOnce(ID3D11DeviceContext* ctx, uint32_t n) noexcept {
    {
        std::lock_guard<std::mutex> lock(g_matMutex);
        if (g_matSeen >= kMatMax) return;
        for (size_t i = 0; i < g_matSeen; ++i)
            if (g_matSeenN[i] == n) return;
        g_matSeenN[g_matSeen++] = n;
    }
    ID3D11ShaderResourceView* srv[3] = {};
    ctx->PSGetShaderResources(kPaneSrvFirst, kPaneSrvCount, srv);
    char d[3][160] = {};
    for (UINT i = 0; i < 3; ++i) describeSrv(srv[i], kPaneSrvFirst + i, d[i], sizeof(d[i]));
    for (auto* v : srv)
        if (v) v->Release();
    MatPending mp;
    ID3D11DeviceContext1* c1 = nullptr;
    ctx->QueryInterface(IID_PPV_ARGS(&c1));
    ID3D11Buffer* cb = nullptr;
    UINT first = 0, num = 0;
    if (c1) c1->PSGetConstantBuffers1(kPaneCbSlot, 1, &cb, &first, &num);
    else ctx->PSGetConstantBuffers(kPaneCbSlot, 1, &cb);
    if (c1) c1->Release();
    char tail[200] = {};
    ID3D11Buffer* staging = nullptr;
    if (!cb) {
        std::snprintf(tail, sizeof(tail), "cb2 not bound");
    } else {
        D3D11_BUFFER_DESC bd{};
        cb->GetDesc(&bd);
        const UINT off = first * 16u;
        if (off >= bd.ByteWidth) {
            std::snprintf(tail, sizeof(tail), "cb2[2] unreadable (first constant %u is past the buffer's %u bytes)", first, bd.ByteWidth);
        } else {
            const UINT len = std::min<UINT>(kPaneCbBytes, bd.ByteWidth - off);
            ID3D11Device* dev = nullptr;
            ctx->GetDevice(&dev);
            D3D11_BUFFER_DESC sd{};
            sd.ByteWidth = kPaneCbBytes;
            sd.Usage = D3D11_USAGE_STAGING;
            sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            if (dev && SUCCEEDED(dev->CreateBuffer(&sd, nullptr, &staging)) && staging) {
                D3D11_BOX box{};
                box.left = off;
                box.right = off + len;
                box.top = 0;
                box.bottom = 1;
                box.front = 0;
                box.back = 1;
                FlatComputeInternalScope internal;
                ctx->CopySubresourceRegion(staging, 0, 0, 0, 0, cb, 0, &box);
                std::snprintf(tail, sizeof(tail), "(cb2 buffer %u bytes, first constant %u, count %u, copied %u bytes)", bd.ByteWidth, first, num, len);
            } else {
                std::snprintf(tail, sizeof(tail), "cb2[2] unreadable (the staging buffer could not be made)");
            }
            if (dev) dev->Release();
        }
        cb->Release();
    }
    char head[620] = {};
    std::snprintf(head, sizeof(head), "concourse pane: n=%u material %s, %s, %s", n, d[0], d[1], d[2]);
    if (!staging) {
        Log::get().note("%s, %s", head, tail);
        return;
    }
    mp.staging = staging;
    mp.dueFrame = g_frameNow.load(std::memory_order_relaxed) + 2;
    // The text up to the values; the tick adds the floats.
    std::snprintf(mp.text, sizeof(mp.text), "%s, cb2[2] = %%g %%g %%g %%g %s", head, tail);
    std::lock_guard<std::mutex> lock(g_matMutex);
    for (auto& slot : g_mat)
        if (!slot.staging) {
            slot = mp;
            return;
        }
    staging->Release();
}

void readMaterials(ID3D11Device* device, uint32_t frame) noexcept {
    ID3D11DeviceContext* imm = nullptr;
    std::lock_guard<std::mutex> lock(g_matMutex);
    for (auto& m : g_mat) {
        if (!m.staging || frame < m.dueFrame) continue;
        if (!imm) {
            device->GetImmediateContext(&imm);
            if (!imm) return;
        }
        FlatComputeInternalScope internal;
        D3D11_MAPPED_SUBRESOURCE ms{};
        const HRESULT h = imm->Map(m.staging, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &ms);
        if (SUCCEEDED(h)) {
            float f[12] = {};
            std::memcpy(f, ms.pData, sizeof(f));
            imm->Unmap(m.staging, 0);
            char line[900] = {};
            std::snprintf(line, sizeof(line), m.text, static_cast<double>(f[8]), static_cast<double>(f[9]), static_cast<double>(f[10]), static_cast<double>(f[11]));
            Log::get().note("%s", line);
            m.staging->Release();
            m = MatPending{};
        } else if (++m.tries > 120) {
            Log::get().note("concourse pane: a material read-back never completed (hr %08X)", static_cast<unsigned>(h));
            m.staging->Release();
            m = MatPending{};
        }
    }
    if (imm) imm->Release();
}

// ---- The capture's qualifying draw ---------------------------------------------------------------------------------------------------------------
// RT0 blending on, a colour target bound, a viewport at least 1000 wide. Opens a query around the draw.
int captureBegin(ID3D11DeviceContext* ctx, UINT indexCount) noexcept {
    ID3D11BlendState* bs = nullptr;
    FLOAT factor[4] = {};
    UINT mask = 0;
    ctx->OMGetBlendState(&bs, factor, &mask);
    if (!bs) return -1;
    D3D11_BLEND_DESC bd{};
    bs->GetDesc(&bd);
    bs->Release();
    if (!bd.RenderTarget[0].BlendEnable) return -1;
    ID3D11RenderTargetView* rtv = nullptr;
    ctx->OMGetRenderTargets(1, &rtv, nullptr);
    if (!rtv) return -1;
    rtv->Release();
    D3D11_VIEWPORT vp{};
    UINT nvp = 1;
    ctx->RSGetViewports(&nvp, &vp);
    if (nvp < 1 || !(vp.Width >= 1000.0f)) return -1;
    CapDraw d;
    d.n = indexCount;
    shaderHashes(ctx, &d.vs, &d.ps);
    d.vpW = static_cast<uint32_t>(vp.Width);
    d.vpH = static_cast<uint32_t>(vp.Height);
    d.src = static_cast<uint32_t>(bd.RenderTarget[0].SrcBlend);
    d.dst = static_cast<uint32_t>(bd.RenderTarget[0].DestBlend);
    ID3D11DepthStencilState* ds = nullptr;
    UINT ref = 0;
    ctx->OMGetDepthStencilState(&ds, &ref);
    if (ds) {
        D3D11_DEPTH_STENCIL_DESC dd{};
        ds->GetDesc(&dd);
        ds->Release();
        d.depthWrite = dd.DepthEnable && dd.DepthWriteMask != D3D11_DEPTH_WRITE_MASK_ZERO ? 1 : 0;
        d.stencil = dd.StencilEnable ? 1u : 0u;
    }
    int idx = -1;
    {
        std::lock_guard<std::mutex> lock(g_capMutex);
        if (g_capN < kPool && g_pool[g_capN]) {
            idx = static_cast<int>(g_capN++);
            g_cap[idx] = d;
        } else {
            ++g_capDropped;
        }
    }
    if (idx < 0) return -1;
    FlatComputeInternalScope internal;
    ctx->Begin(g_pool[idx]);
    return idx;
}

// ---- The side tests -----------------------------------------------------------------------------------------------------------------------------
int testOpen(ID3D11DeviceContext* ctx, const cc::PaneKey& key) noexcept {
    int idx = -1;
    {
        std::lock_guard<std::mutex> lock(g_sideMutex);
        if (!g_testsMade) return -1;
        for (size_t i = 0; i < kTestSlots; ++i)
            if (g_tests[i].state == 0 && g_tests[i].qDrawn) {
                idx = static_cast<int>(i);
                break;
            }
        if (idx < 0) return -1;
        g_tests[idx].state = 1;
        g_tests[idx].frame = g_frameNow.load(std::memory_order_relaxed);
        g_tests[idx].key = key;
    }
    FlatComputeInternalScope internal;
    ctx->Begin(g_tests[idx].qDrawn);
    return idx;
}

// End of the game's draw: the first query closes and the slot waits for the mirror.
void testCloseDrawn(ID3D11DeviceContext* ctx, int idx) noexcept {
    {
        FlatComputeInternalScope internal;
        ctx->End(g_tests[idx].qDrawn);
    }
    std::lock_guard<std::mutex> lock(g_sideMutex);
    g_tests[idx].state = 2;
}

void testOpenMirror(ID3D11DeviceContext* ctx, int idx) noexcept {
    {
        FlatComputeInternalScope internal;
        ctx->Begin(g_tests[idx].qMirror);
    }
    std::lock_guard<std::mutex> lock(g_sideMutex);
    g_tests[idx].state = 3;
}

void testCloseMirror(ID3D11DeviceContext* ctx, int idx) noexcept {
    {
        FlatComputeInternalScope internal;
        ctx->End(g_tests[idx].qMirror);
    }
    std::lock_guard<std::mutex> lock(g_sideMutex);
    g_tests[idx].state = 4;
}

// A test that did not get its mirror draw: the slot is free again (a query that ended and was not read may begin again).
void testAbandon(int idx) noexcept {
    std::lock_guard<std::mutex> lock(g_sideMutex);
    g_tests[idx].state = 0;
}

cc::PaneSide sideOf(const cc::PaneKey& key) noexcept {
    std::lock_guard<std::mutex> lock(g_sideMutex);
    return g_sides.side(key);
}

// The tick: reads the tests that arrived and feeds the table.
void pollTests(ID3D11Device* device, uint32_t frame) noexcept {
    ID3D11DeviceContext* imm = nullptr;
    device->GetImmediateContext(&imm);
    if (!imm) return;
    {
        std::lock_guard<std::mutex> lock(g_sideMutex);
        if (g_testsMade) {
            FlatComputeInternalScope internal;
            for (auto& t : g_tests) {
                if (t.state != 4) {
                    if (t.state != 0 && static_cast<uint32_t>(frame - t.frame) > 600u) t.state = 0;   // stuck open (a draw that never reached its end)
                    continue;
                }
                UINT64 drawn = 0, mirrored = 0;
                if (imm->GetData(t.qDrawn, &drawn, sizeof(drawn), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) continue;
                if (imm->GetData(t.qMirror, &mirrored, sizeof(mirrored), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) continue;
                const cc::PaneKey key = t.key;
                t.state = 0;
                g_testsRead.fetch_add(1, std::memory_order_relaxed);
                const cc::PaneSides::Change c = g_sides.note(key, drawn, mirrored);
                if (c == cc::PaneSides::Change::kNone) continue;
                const bool ext = g_sides.side(key) == cc::PaneSide::kExterior;
                uint32_t& cap = (c == cc::PaneSides::Change::kFirst && ext) ? g_sideLogExterior : g_sideLogOther;
                if (cap >= kSideLogCap) continue;
                ++cap;
                const char* word = c == cc::PaneSides::Change::kChanged ? (ext ? "changed side to exterior" : "changed side to interior") : (ext ? "exterior" : "interior");
                Log::get().note("concourse pane: %s n=%u start=%u base=%d (drawn %llu, mirrored %llu)", word, key.n, key.startIndex, key.baseVertex,
                                static_cast<unsigned long long>(drawn), static_cast<unsigned long long>(mirrored));
            }
        }
    }
    imm->Release();
}

// ---- The mark pass -----------------------------------------------------------------------------------------------------------------------------
bool formatHasStencil(DXGI_FORMAT f) noexcept {
    return f == DXGI_FORMAT_D24_UNORM_S8_UINT || f == DXGI_FORMAT_D32_FLOAT_S8X24_UINT || f == DXGI_FORMAT_R24G8_TYPELESS || f == DXGI_FORMAT_R32G8X24_TYPELESS;
}

bool markRefuse() noexcept {
    g_markRefused.fetch_add(1, std::memory_order_relaxed);
    return false;
}

// Runs at most once per (frame, depth view, viewport): one fullscreen triangle, no colour, at the depth of the beyond distance; where the scene's stored depth is at
// or below it (the scene is farther) stencil bit 0x40 is set. Everything touched is put back before returning, through the game's own entry points (past EDVR's
// thunks: the binding shadow, the shader substitution and the issue epoch do not move) or the context's own methods where no thunk exists (input layout, topology
// and rasteriser state are not hooked), and no draw thunk is re-entered (the draw goes through the real Draw).
bool ensureMarked(ID3D11DeviceContext* ctx, const ConcoursePaneCalls& calls) noexcept {
    if (!g_markReady.load(std::memory_order_acquire) || !calls.draw || !calls.setVs || !calls.setPs || !calls.setBlend || !calls.setDepth) return markRefuse();
    ID3D11VertexShader* vsMark = g_vsMark.load(std::memory_order_acquire);
    ID3D11RasterizerState* rsMark = g_rsMark.load(std::memory_order_acquire);
    ID3D11BlendState* bsMark = g_bsMark.load(std::memory_order_acquire);
    ID3D11DepthStencilState* dsMark = g_dsMark.load(std::memory_order_acquire);
    if (!vsMark || !rsMark || !bsMark || !dsMark) return markRefuse();
    ID3D11DepthStencilView* dsv = nullptr;
    ctx->OMGetRenderTargets(0, nullptr, &dsv);
    if (!dsv) return markRefuse();
    D3D11_VIEWPORT vp{};
    UINT nvp = 1;
    ctx->RSGetViewports(&nvp, &vp);
    const uint32_t frame = g_frameNow.load(std::memory_order_relaxed);
    if (g_markKey.valid && g_markKey.frame == frame && g_markKey.dsv == dsv && g_markKey.vp[0] == vp.TopLeftX && g_markKey.vp[1] == vp.TopLeftY &&
        g_markKey.vp[2] == vp.Width && g_markKey.vp[3] == vp.Height) {
        dsv->Release();
        return true;
    }
    D3D11_DEPTH_STENCIL_VIEW_DESC dd{};
    dsv->GetDesc(&dd);
    DXGI_FORMAT fmt = dd.Format;
    if (fmt == DXGI_FORMAT_UNKNOWN) {   // a view made without a description: the resource's own format
        ID3D11Resource* res = nullptr;
        dsv->GetResource(&res);
        ID3D11Texture2D* tex = nullptr;
        if (res) res->QueryInterface(IID_PPV_ARGS(&tex));
        if (tex) {
            D3D11_TEXTURE2D_DESC td{};
            tex->GetDesc(&td);
            fmt = td.Format;
            tex->Release();
        }
        if (res) res->Release();
    }
    if (!formatHasStencil(fmt) || (dd.Flags & D3D11_DSV_READ_ONLY_STENCIL) != 0 || nvp < 1 || !(vp.Width > 0.0f) || !(vp.Height > 0.0f)) {
        if (!g_markNoStencilLogged) {
            g_markNoStencilLogged = true;
            const char* fn = formatName(fmt);
            char fb[24] = {};
            if (!fn) std::snprintf(fb, sizeof(fb), "format %u", static_cast<unsigned>(fmt));
            Log::get().note("concourse pane: the mirror cannot work: the depth view (%s, flags %u, %u viewports, %.0fx%.0f) has no writable stencil; the game's glass is left alone",
                            fn ? fn : fb, static_cast<unsigned>(dd.Flags), static_cast<unsigned>(nvp), static_cast<double>(vp.Width), static_cast<double>(vp.Height));
        }
        dsv->Release();
        return markRefuse();
    }
    ID3D11GeometryShader* gs = nullptr;
    ID3D11HullShader* hs = nullptr;
    ID3D11DomainShader* dom = nullptr;
    ctx->GSGetShader(&gs, nullptr, nullptr);
    ctx->HSGetShader(&hs, nullptr, nullptr);
    ctx->DSGetShader(&dom, nullptr, nullptr);
    const bool staged = gs || hs || dom;
    if (gs) gs->Release();
    if (hs) hs->Release();
    if (dom) dom->Release();
    if (staged) {   // a geometry or tessellation stage would take the triangle: not run (counted)
        dsv->Release();
        return markRefuse();
    }
    // Save.
    ID3D11InputLayout* prevLayout = nullptr;
    D3D11_PRIMITIVE_TOPOLOGY prevTopo = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    ID3D11VertexShader* prevVs = nullptr;
    ID3D11PixelShader* prevPs = nullptr;
    ID3D11RasterizerState* prevRs = nullptr;
    ID3D11BlendState* prevBs = nullptr;
    FLOAT prevFactor[4] = {};
    UINT prevMask = 0;
    ID3D11DepthStencilState* prevDs = nullptr;
    UINT prevRef = 0;
    ctx->IAGetInputLayout(&prevLayout);
    ctx->IAGetPrimitiveTopology(&prevTopo);
    ctx->VSGetShader(&prevVs, nullptr, nullptr);
    ctx->PSGetShader(&prevPs, nullptr, nullptr);
    ctx->RSGetState(&prevRs);
    ctx->OMGetBlendState(&prevBs, prevFactor, &prevMask);
    ctx->OMGetDepthStencilState(&prevDs, &prevRef);
    // Draw.
    const FLOAT zeros[4] = {};
    ctx->IASetInputLayout(nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    calls.setVs(ctx, vsMark, nullptr, 0);
    calls.setPs(ctx, nullptr, nullptr, 0);
    ctx->RSSetState(rsMark);
    calls.setBlend(ctx, bsMark, zeros, 0xFFFFFFFFu);
    calls.setDepth(ctx, dsMark, kMarkRef);
    calls.draw(ctx, 3, 0);
    // Restore.
    calls.setDepth(ctx, prevDs, prevRef);
    calls.setBlend(ctx, prevBs, prevFactor, prevMask);
    ctx->RSSetState(prevRs);
    calls.setPs(ctx, prevPs, nullptr, 0);
    calls.setVs(ctx, prevVs, nullptr, 0);
    ctx->IASetPrimitiveTopology(prevTopo);
    ctx->IASetInputLayout(prevLayout);
    if (prevDs) prevDs->Release();
    if (prevBs) prevBs->Release();
    if (prevRs) prevRs->Release();
    if (prevPs) prevPs->Release();
    if (prevVs) prevVs->Release();
    if (prevLayout) prevLayout->Release();
    g_markKey.valid = true;
    g_markKey.frame = frame;
    g_markKey.dsv = dsv;   // identity only
    g_markKey.vp[0] = vp.TopLeftX;
    g_markKey.vp[1] = vp.TopLeftY;
    g_markKey.vp[2] = vp.Width;
    g_markKey.vp[3] = vp.Height;
    dsv->Release();
    g_markPasses.fetch_add(1, std::memory_order_relaxed);
    if (!g_markLogged) {
        g_markLogged = true;
        const float beyond = g_vsMarkM;
        const char* fn = formatName(fmt);
        char fb[24] = {};
        if (!fn) std::snprintf(fb, sizeof(fb), "format %u", static_cast<unsigned>(fmt));
        Log::get().note("concourse pane: mark pass: dsv format %s, viewport %ux%u, beyond %.0f m (z %.6g)", fn ? fn : fb, static_cast<unsigned>(vp.Width),
                        static_cast<unsigned>(vp.Height), static_cast<double>(beyond), static_cast<double>(cc::markDepthForDistance(beyond)));
    }
    return true;
}

}  // namespace

// begin() changes no state. It runs the capture path, the withheld check, the family match, the one-shot material read and the share query, then only DECIDES the
// plan (a mirror draw or none, marked or not, opaque or fading) and opens the side test's first query when one is due.
void ConcoursePaneScope::begin(ID3D11DeviceContext* ctx, UINT indexCount, UINT startIndex, INT baseVertex, const ConcoursePaneCalls& calls) noexcept {
    if (!ctx) return;
    if (g_capturing.load(std::memory_order_acquire)) {
        const int q = captureBegin(ctx, indexCount);
        if (q >= 0) {
            ctx_ = ctx;
            capQuery_ = q;
            flags_ = kFlagCapQuery;
        }
        return;
    }
    // Every draw of the frame passes here while the gate holds. On the immediate context (the one the binding shadow follows) the shadow answers with no COM
    // call: 0 is no pixel shader (a depth-only draw), and the draw has none to name. Any other context asks the context for its pixel shader.
    const bool followed = ctx == g_immediate.load(std::memory_order_relaxed);
    const uint64_t ps = followed ? bindingShaderHash(BindSlot::Ps) : pixelShaderHash(ctx);
    const bool cullOn = g_withholdNow.load(std::memory_order_relaxed);
    const bool sealedOn = g_withholdSealedNow.load(std::memory_order_relaxed);
    if (cullOn || sealedOn) {
        // The outside (smoke, trails, ship lasers, the planet) is not issued while the cull is on. By pixel shader on any context; by vertex shader too on the
        // immediate one, because a terrain patch has no pixel shader. Checked before `if (!ps) return`.
        int fam = cc::withheldPsIndex(ps);
        bool sealedToo = fam >= 0 && cc::kWithheldPs[fam].sealedToo;
        if (fam < 0 && followed) {
            const int v = cc::withheldVsIndex(bindingShaderHash(BindSlot::Vs));
            if (v >= 0) {
                fam = static_cast<int>(cc::kWithheldPsCount) + v;
                sealedToo = cc::kWithheldVs[v].sealedToo;
            }
        }
        if (fam >= 0 && (cullOn || sealedToo)) {
            withheld_ = true;
            g_withheld.fetch_add(1, std::memory_order_relaxed);
            g_withheldFam[fam].fetch_add(1, std::memory_order_relaxed);
            return;
        }
        // The glows that may also be the room's: their far pixels are dropped by the mark (concourse_cull_core.h, farLimitedPs). Immediate context only, as the mark is.
        if (!cullOn) return;   // sealed: nothing below is for a sealed hangar
        if (followed && cc::farLimitedPs(ps)) {
            if (ensureMarked(ctx, calls)) {
                UINT ref = 0;
                ctx->OMGetDepthStencilState(&prevDs_, &ref);
                ID3D11DepthStencilState* derived = prevDs_ ? derivedDepth(ctx, prevDs_, DsVariant::kFarOut) : nullptr;
                if (derived) {
                    ctx_ = ctx;
                    calls_ = &calls;
                    prevStencilRef_ = ref;
                    calls.setDepth(ctx, derived, kMarkRef);
                    flags_ |= kFlagDepth;
                    g_farLimited.fetch_add(1, std::memory_order_relaxed);
                } else if (prevDs_) {
                    prevDs_->Release();
                    prevDs_ = nullptr;
                }
            }
            return;
        }
    }
    if (!ps) return;
    const uint32_t listN = std::min<uint32_t>(g_listN.load(std::memory_order_acquire), static_cast<uint32_t>(cc::kPaneListCap));
    bool listed = false;
    for (uint32_t i = 0; i < listN; ++i)
        if (g_list[i].ps == ps) {
            listed = true;
            break;
        }
    if (!listed) return;   // the pixel shader first: the vertex shader's hash is read only for a draw that could be a pane
    const uint64_t vs = vertexShaderHash(ctx);
    if (cc::paneTripleIndex(g_list, listN, vs, ps, indexCount) < 0) return;
    ctx_ = ctx;
    calls_ = &calls;
    family_ = true;
    keyN_ = indexCount;
    keyStart_ = startIndex;
    keyBase_ = baseVertex;
    const bool immediate = ctx->GetType() == D3D11_DEVICE_CONTEXT_IMMEDIATE;
    if (immediate) {
        materialOnce(ctx, indexCount);
        if (g_shareOn.load(std::memory_order_acquire)) {
            query_ = openShareQuery(ctx, indexCount);
            if (query_ >= 0) flags_ |= kFlagQuery;
        }
    }
    const uint32_t mode = g_mode.load(std::memory_order_relaxed);
    if (!immediate || !g_treatNow.load(std::memory_order_acquire) || mode == kModeOff) return;   // measured only, and the game's glass alone
    const bool tint = mode == kModeTint;
    const bool plain = g_withholdNow.load(std::memory_order_relaxed);   // the cull is on and the phase is the plain mirror phase
    bool mark = false;
    bool opaque = true;
    float opacity = 1.0f;
    if (plain) {
        mark = true;   // every pane mirrors where the scene behind it is far; the mark pass says where
        if (g_frameNow.load(std::memory_order_relaxed) - g_plainFrom.load(std::memory_order_relaxed) < kMarkSettleFrames &&
            sideOf(cc::PaneKey{indexCount, startIndex, baseVertex}) == cc::PaneSide::kExterior)
            mark = false;
    } else {
        // The cull is lifted (lifting, fading, clear): only a pane known to be exterior is still mirrored, and over its whole area.
        const cc::PaneKey key{indexCount, startIndex, baseVertex};
        if (sideOf(key) != cc::PaneSide::kExterior) return;
        if (indexCount == cc::kBayIndexCount) {
            opacity = std::min(1.0f, std::max(0.0f, g_bayOpacity.load(std::memory_order_relaxed)));
            if (opacity <= 0.001f) return;   // the bay glass is clear: the game's own glass, alone
            if (opacity < 0.999f) opaque = false;
        }
    }
    if (tint) opaque = true;
    planMirror_ = true;
    planMark_ = mark;
    planOpaque_ = opaque;
    planTint_ = tint;
    planSoft_ = mode == kModeMirrorSoft;
    planSteady_ = steadyOf(mode) > 0.0f;
    planGlow_ = tint ? 0.0f : (planSteady_ ? steadyOf(mode) : glowOf(mode)) * opacity;
    if (tint) {
        const float* c = cc::kPaneColours[tintSlotOf(indexCount) % cc::kPaneColourCount];
        for (int k = 0; k < 3; ++k) planFactor_[k] = c[k] * cc::kPaneTintIntensity;
        planFactor_[3] = 1.0f;
    } else if (!opaque) {
        planFactor_[0] = planFactor_[1] = planFactor_[2] = planFactor_[3] = opacity;
    }
    // The side test (plain phase, one frame in eight per pane): the first query wraps the game's draw and closes in beginMirror().
    if (plain && mark && g_testsOn.load(std::memory_order_acquire) && g_markReady.load(std::memory_order_acquire) &&
        cc::paneTestThisFrame(cc::PaneKey{indexCount, startIndex, baseVertex}, g_frameNow.load(std::memory_order_relaxed))) {
        testSlot_ = testOpen(ctx, cc::PaneKey{indexCount, startIndex, baseVertex});
        if (testSlot_ >= 0) flags_ |= kFlagTestDrawn;
    }
}

// Called right after the game's own draw. Closes the share query (so the bay's coverage is the game's draw alone) and the test's first query, then, if the plan
// says so and the mark pass could run, binds the mirror's state and returns true. Every false return leaves the context as the game had it.
bool ConcoursePaneScope::nextMirrorPassSlow() noexcept {
    if (!mirrorBound_) {
        if (!beginMirrorSlow()) {
            family_ = false;
            return false;
        }
        mirrorBound_ = true;
        return true;
    }
    // The mirror has drawn. Its count for the side test ends here, whether or not a glow follows; the glow is the last pass.
    family_ = false;
    if (testSlot_ >= 0 && (flags_ & kFlagTestMirror) != 0) {
        testCloseMirror(ctx_, testSlot_);
        flags_ &= ~kFlagTestMirror;
        testSlot_ = -1;
    }
    if (!(planGlow_ > 0.0f) || (flags_ & kFlagSrv) == 0 || (flags_ & kFlagBlend) == 0) return false;
    ID3D11BlendState* bs = g_bsGlow.load(std::memory_order_acquire);
    if (!bs) return false;
    if (planSteady_) {
        ID3D11PixelShader* white = g_psWhite.load(std::memory_order_acquire);
        if (!white) return false;
        ctx_->PSGetShader(&prevPs_, nullptr, nullptr);
        calls_->setPs(ctx_, white, nullptr, 0);
        flags_ |= kFlagPs;
        const FLOAT steady[4] = {kSteadyColour[0] * planGlow_, kSteadyColour[1] * planGlow_, kSteadyColour[2] * planGlow_, planGlow_};
        calls_->setBlend(ctx_, bs, steady, 0xFFFFFFFFu);
        return true;
    }
    ID3D11ShaderResourceView* mine[3] = {g_mirrorSrv[0].load(std::memory_order_acquire), g_mirrorSrv[1].load(std::memory_order_acquire),
                                         g_glowT10.load(std::memory_order_acquire)};
    if (!mine[0] || !mine[1] || !mine[2]) return false;
    calls_->setSrv(ctx_, kPaneSrvFirst, kPaneSrvCount, mine);
    const FLOAT factor[4] = {planGlow_, planGlow_, planGlow_, planGlow_};
    calls_->setBlend(ctx_, bs, factor, 0xFFFFFFFFu);
    return true;
}

bool ConcoursePaneScope::beginMirrorSlow() noexcept {
    if ((flags_ & kFlagQuery) != 0) {
        if (query_ >= 0) closeShareQuery(ctx_, query_);
        flags_ &= ~kFlagQuery;
    }
    if ((flags_ & kFlagTestDrawn) != 0) {
        testCloseDrawn(ctx_, testSlot_);
        flags_ &= ~kFlagTestDrawn;
    }
    const auto no = [&]() noexcept {
        if (testSlot_ >= 0) {
            testAbandon(testSlot_);
            testSlot_ = -1;
        }
        return false;
    };
    if (!planMirror_) return no();
    if (planMark_ && !ensureMarked(ctx_, *calls_)) return no();   // runs and restores its own state
    ID3D11BlendState* repl = planTint_ ? g_bsFactor.load(std::memory_order_acquire)
                                       : planOpaque_ ? g_bsMirror.load(std::memory_order_acquire) : g_bsFade.load(std::memory_order_acquire);
    if (!repl) return no();
    ID3D11ShaderResourceView* mine[3] = {};
    if (!planTint_) {
        for (int i = 0; i < 3; ++i) mine[i] = g_mirrorSrv[i].load(std::memory_order_acquire);
        if (!mine[0] || !mine[1] || !mine[2]) return no();
    }
    ID3D11PixelShader* white = planTint_ ? g_psWhite.load(std::memory_order_acquire) : nullptr;
    ID3D11Buffer* ownCb = planSoft_ && !planTint_ ? g_cbMirror.load(std::memory_order_acquire) : nullptr;
    // The depth state first: an opaque pane needs a derived one, and without it nothing is bound.
    ID3D11DepthStencilState* derived = nullptr;
    UINT stencilRef = 0;
    if (planOpaque_) {
        UINT ref = 0;
        ctx_->OMGetDepthStencilState(&prevDs_, &ref);
        derived = prevDs_ ? derivedDepth(ctx_, prevDs_, planMark_ ? DsVariant::kMarked : DsVariant::kPlain) : nullptr;
        if (!derived) {
            if (prevDs_) prevDs_->Release();
            prevDs_ = nullptr;
            return no();
        }
        prevStencilRef_ = ref;
        stencilRef = planMark_ ? kMarkRef : ref;
    }
    // Bind.
    ctx_->OMGetBlendState(&prevBs_, prevFactor_, &prevMask_);
    calls_->setBlend(ctx_, repl, planFactor_, 0xFFFFFFFFu);
    flags_ |= kFlagBlend;
    if (planTint_) {
        if (white) {
            ctx_->PSGetShader(&prevPs_, nullptr, nullptr);
            calls_->setPs(ctx_, white, nullptr, 0);
            flags_ |= kFlagPs;
        }
    } else {
        ctx_->PSGetShaderResources(kPaneSrvFirst, kPaneSrvCount, prevSrv_);
        calls_->setSrv(ctx_, kPaneSrvFirst, kPaneSrvCount, mine);
        flags_ |= kFlagSrv;
    }
    if (derived) {
        calls_->setDepth(ctx_, derived, stencilRef);
        flags_ |= kFlagDepth;
    }
    if (ownCb) {
        ctx_->QueryInterface(IID_PPV_ARGS(&ctx1_));
        if (ctx1_) {
            // The *ConstantBuffers1 methods are not hooked, so this passes no thunk, and the saved offset window is put back exactly.
            ctx1_->PSGetConstantBuffers1(kPaneCbSlot, 1, &prevCb_, &prevCbFirst_, &prevCbNum_);
            ctx1_->PSSetConstantBuffers1(kPaneCbSlot, 1, &ownCb, nullptr, nullptr);
        } else {
            ctx_->PSGetConstantBuffers(kPaneCbSlot, 1, &prevCb_);
            ctx_->PSSetConstantBuffers(kPaneCbSlot, 1, &ownCb);
        }
        flags_ |= kFlagCb;
    }
    if (testSlot_ >= 0) {
        testOpenMirror(ctx_, testSlot_);
        flags_ |= kFlagTestMirror;
    }
    g_treated.fetch_add(1, std::memory_order_relaxed);
    return true;
}

void ConcoursePaneScope::end() noexcept {
    if (testSlot_ >= 0) {
        if ((flags_ & kFlagTestMirror) != 0) {
            testCloseMirror(ctx_, testSlot_);
        } else {
            if ((flags_ & kFlagTestDrawn) != 0) {   // the game's draw was never followed by beginMirror(): close its query and drop the test
                FlatComputeInternalScope internal;
                ctx_->End(g_tests[testSlot_].qDrawn);
            }
            testAbandon(testSlot_);
        }
        testSlot_ = -1;
        flags_ &= ~(kFlagTestMirror | kFlagTestDrawn);
    }
    if ((flags_ & kFlagCb) != 0) {
        if (ctx1_) {
            if (prevCb_) ctx1_->PSSetConstantBuffers1(kPaneCbSlot, 1, &prevCb_, &prevCbFirst_, &prevCbNum_);
            else ctx1_->PSSetConstantBuffers1(kPaneCbSlot, 1, &prevCb_, nullptr, nullptr);
            ctx1_->Release();
            ctx1_ = nullptr;
        } else {
            ctx_->PSSetConstantBuffers(kPaneCbSlot, 1, &prevCb_);
        }
        if (prevCb_) prevCb_->Release();
        prevCb_ = nullptr;
    }
    if ((flags_ & kFlagDepth) != 0) {
        calls_->setDepth(ctx_, prevDs_, prevStencilRef_);
        if (prevDs_) prevDs_->Release();
        prevDs_ = nullptr;
    }
    if ((flags_ & kFlagSrv) != 0) {
        calls_->setSrv(ctx_, kPaneSrvFirst, kPaneSrvCount, prevSrv_);
        for (auto*& v : prevSrv_) {
            if (v) v->Release();
            v = nullptr;
        }
    }
    if ((flags_ & kFlagPs) != 0) {
        calls_->setPs(ctx_, prevPs_, nullptr, 0);
        if (prevPs_) prevPs_->Release();
        prevPs_ = nullptr;
    }
    if ((flags_ & kFlagBlend) != 0) {
        calls_->setBlend(ctx_, prevBs_, prevFactor_, prevMask_);
        if (prevBs_) prevBs_->Release();
        prevBs_ = nullptr;
    }
    if ((flags_ & kFlagQuery) != 0 && query_ >= 0) closeShareQuery(ctx_, query_);
    if ((flags_ & kFlagCapQuery) != 0 && capQuery_ >= 0) {
        FlatComputeInternalScope internal;
        ctx_->End(g_pool[capQuery_]);
    }
    flags_ = 0;
}

namespace {

// ---- The capture's report ---------------------------------------------------------------------------------------------------------------------
struct Group {
    uint64_t vs = 0, ps = 0;
    uint32_t n = 0;
    uint32_t vpW = 0, vpH = 0;
    uint32_t src = 0, dst = 0;
    int depthWrite = 0;
    uint32_t stencil = 0;
    uint32_t draws = 0;
    uint64_t samples = 0;
};

void setMode(uint32_t m, const char* why) noexcept {
    const uint32_t before = g_mode.exchange(m, std::memory_order_relaxed);
    if (before != m) Log::get().note("concourse pane: mode %s (%s)", modeText(m), why);
}

// The ranking only: the capture never changes the list or the mode.
void reportCapture() noexcept {
    static Group groups[kPool];
    size_t gn = 0;
    uint32_t arrived = 0;
    for (size_t i = 0; i < g_capN; ++i) {
        const CapDraw& d = g_cap[i];
        if (!d.arrived) continue;
        ++arrived;
        size_t k = 0;
        while (k < gn && !(groups[k].vs == d.vs && groups[k].ps == d.ps && groups[k].n == d.n)) ++k;
        if (k == gn) {
            groups[gn] = Group{};
            groups[gn].vs = d.vs;
            groups[gn].ps = d.ps;
            groups[gn].n = d.n;
            groups[gn].vpW = d.vpW;
            groups[gn].vpH = d.vpH;
            groups[gn].src = d.src;
            groups[gn].dst = d.dst;
            groups[gn].depthWrite = d.depthWrite;
            groups[gn].stencil = d.stencil;
            ++gn;
        }
        groups[k].samples += d.samples;
        ++groups[k].draws;
    }
    Log::get().note("concourse pane capture: %u blended draws wrapped, %u results arrived, %u pending, %llu qualifying draws over the pool cap of %u; %u triples",
                    static_cast<unsigned>(g_capN), arrived, static_cast<unsigned>(g_capN) - arrived, static_cast<unsigned long long>(g_capDropped),
                    static_cast<unsigned>(kPool), static_cast<unsigned>(gn));
    std::sort(groups, groups + gn, [](const Group& a, const Group& b) { return a.samples > b.samples; });
    const size_t top = gn < 24 ? gn : 24;
    for (size_t i = 0; i < top; ++i) {
        const Group& g = groups[i];
        const double pixels = static_cast<double>(g.vpW) * static_cast<double>(g.vpH);
        Log::get().note("concourse pane capture: #%d samples %llu (%.1f%% of %ux%u) vs=%016llX ps=%016llX n=%u draws=%u blend=%u,%u depth_write=%d stencil=%u",
                        static_cast<int>(i + 1), static_cast<unsigned long long>(g.samples), pixels > 0.0 ? 100.0 * static_cast<double>(g.samples) / pixels : 0.0,
                        g.vpW, g.vpH, static_cast<unsigned long long>(g.vs), static_cast<unsigned long long>(g.ps), g.n, g.draws, g.src, g.dst, g.depthWrite,
                        g.stencil);
    }
    if (gn == 0) Log::get().note("concourse pane capture: no blended draw covered a pixel (or no result arrived)");
}

void pollResults(ID3D11Device* device) noexcept {
    ID3D11DeviceContext* imm = nullptr;
    device->GetImmediateContext(&imm);
    if (!imm) return;
    FlatComputeInternalScope internal;
    for (size_t i = 0; i < g_capN; ++i) {
        CapDraw& d = g_cap[i];
        if (d.arrived) continue;
        UINT64 samples = 0;
        if (imm->GetData(g_pool[i], &samples, sizeof(samples), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK) {
            d.arrived = true;
            d.samples = samples;
        }
    }
    imm->Release();
}

bool allArrived() noexcept {
    for (size_t i = 0; i < g_capN; ++i)
        if (!g_cap[i].arrived) return false;
    return true;
}

// ---- The tick's pieces ------------------------------------------------------------------------------------------------------------------------
void bindKey(Hotkey*& key, std::string& applied, const std::string& text, const char* dotted, const char* what) {
    if (!key) key = new Hotkey();
    if (g_keysBound && text == applied) return;
    const bool first = !g_keysBound;
    applied = text;
    key->setBinding(text.c_str());
    if (key->bound()) Log::get().note("concourse pane: %s bound (%s = \"%s\")", what, dotted, text.c_str());
    else if (!text.empty()) Log::get().note("concourse pane: %s is set (\"%s\") but bound nothing, so it cannot be used this session", dotted, text.c_str());
    else if (!first) Log::get().note("concourse pane: %s cleared", dotted);
}

void pollConfig(uint64_t nowMs) {
    if (nowMs < g_nextPollMs) return;
    g_nextPollMs = nowMs + 1000;
    g_hashEpoch.fetch_add(1, std::memory_order_relaxed);
    bindKey(g_keyCapture, g_keyCaptureText, Config::get().getString("hotkey.concourse_pane_capture", ""), "hotkey.concourse_pane_capture", "capture key");
    bindKey(g_keyMode, g_keyModeText, Config::get().getString("hotkey.concourse_pane_mode", ""), "hotkey.concourse_pane_mode", "mode key");
    g_keysBound = true;
    const std::string modeT = Config::get().getString("diag.concourse_pane_mode", "mirror-steady-100");
    if (!g_modeTextSeen || modeT != g_modeAppliedText) {
        g_modeTextSeen = true;
        g_modeAppliedText = modeT;
        uint32_t m = kModeSteady100;
        if (parseMode(modeT, &m)) setMode(m, "diag.concourse_pane_mode");
        else Log::get().note("concourse pane: diag.concourse_pane_mode = \"%s\" is not mirror-steady-25, -50, -100, -200 or -400, mirror-lit, mirror-bright, mirror-brightest, mirror, mirror-soft, tint or off; the mode is unchanged", modeT.c_str());
    }
    const std::string beyondT = Config::get().getString("diag.concourse_mirror_beyond", "");
    if (!g_beyondTextSeen || beyondT != g_beyondAppliedText) {
        const bool first = !g_beyondTextSeen;
        g_beyondTextSeen = true;
        g_beyondAppliedText = beyondT;
        if (beyondT.empty()) {
            g_beyondM.store(cc::kMirrorBeyondDefaultM, std::memory_order_relaxed);   // unset: the default
        } else {
            char* end = nullptr;
            const double v = std::strtod(beyondT.c_str(), &end);
            while (end && (*end == ' ' || *end == '\t')) ++end;
            if (end && end != beyondT.c_str() && *end == '\0' && v >= static_cast<double>(cc::kMirrorBeyondMinM) && v <= static_cast<double>(cc::kMirrorBeyondMaxM)) {
                g_beyondM.store(static_cast<float>(v), std::memory_order_relaxed);
                if (!first || v != static_cast<double>(cc::kMirrorBeyondDefaultM))
                    Log::get().note("concourse pane: diag.concourse_mirror_beyond is now %.1f m", v);
            } else {
                Log::get().note("concourse pane: diag.concourse_mirror_beyond = \"%s\" is not a number from %.0f to %.0f; the value stays %.1f m", beyondT.c_str(),
                                static_cast<double>(cc::kMirrorBeyondMinM), static_cast<double>(cc::kMirrorBeyondMaxM),
                                static_cast<double>(g_beyondM.load(std::memory_order_relaxed)));
            }
        }
    }
    const std::string panesT = Config::get().getString("diag.concourse_panes", "");
    if (!g_panesTextSeen || panesT != g_panesAppliedText) {
        const bool first = !g_panesTextSeen;
        g_panesTextSeen = true;
        g_panesAppliedText = panesT;
        if (!(first && panesT.empty())) {
            cc::PaneTriple parsed[cc::kPaneListCap];
            size_t bad = 0;
            const size_t n = cc::parsePaneList(panesT.c_str(), parsed, cc::kPaneListCap - 1, &bad);
            g_listN.store(1, std::memory_order_release);   // the built-in family alone while the added entries are written
            size_t total = 1;
            for (size_t i = 0; i < n; ++i)
                if (!cc::paneTripleHas(g_list, total, parsed[i])) g_list[total++] = parsed[i];
            g_listN.store(static_cast<uint32_t>(total), std::memory_order_release);
            Log::get().note("concourse pane: diag.concourse_panes: %u pane(s) added to the built-in family, %u entr%s refused (an entry is VS/PS or VS/PS/N: two 16-digit hex hashes, and an index count)",
                            static_cast<unsigned>(total - 1), static_cast<unsigned>(bad), bad == 1 ? "y" : "ies");
            for (size_t i = 1; i < total; ++i)
                Log::get().note("concourse pane: added vs=%016llX ps=%016llX n=%u (0 is any)", static_cast<unsigned long long>(g_list[i].vs),
                                static_cast<unsigned long long>(g_list[i].ps), g_list[i].n);
        }
    }
}

void updateArmed() noexcept {
    const uint32_t mode = g_mode.load(std::memory_order_relaxed);
    const bool treat = g_feedTreat.load(std::memory_order_relaxed) && mode != kModeOff && g_statesReady.load(std::memory_order_acquire);
    const bool withhold = g_feedWithhold.load(std::memory_order_relaxed);   // whatever the mode: F4 at off shows what the cull leaves
    const bool shares = g_feedMeasure.load(std::memory_order_relaxed) && g_shareOn.load(std::memory_order_relaxed);
    g_treatNow.store(treat, std::memory_order_release);
    const bool sealed = g_feedSealed.load(std::memory_order_relaxed);
    g_withholdNow.store(withhold, std::memory_order_release);
    g_withholdSealedNow.store(sealed, std::memory_order_release);
    detail::g_concoursePaneArmed.store(treat || withhold || sealed || shares || g_capturing.load(std::memory_order_relaxed), std::memory_order_release);
}

}  // namespace

uint64_t concoursePaneTakeTreated() noexcept { return g_treated.exchange(0, std::memory_order_relaxed); }
bool concoursePaneTakeSurveyRequest() noexcept { return g_surveyRequested.exchange(false, std::memory_order_relaxed); }
namespace {
// Every 30 s, while any family was withheld: one line naming the families that were (a family that never matched is absent).
void logWithheld(uint64_t nowMs) noexcept {
    if (nowMs - g_withheldLogMs < 30000) return;
    g_withheldLogMs = nowMs;
    char text[900] = {};
    size_t used = 0;
    bool any = false;
    for (size_t i = 0; i < kWithheldFamilies; ++i) {
        const uint64_t n = g_withheldFam[i].exchange(0, std::memory_order_relaxed);
        if (n == 0) continue;
        const char* name = i < cc::kWithheldPsCount ? cc::kWithheldPs[i].name : cc::kWithheldVs[i - cc::kWithheldPsCount].name;
        const int w = std::snprintf(text + used, sizeof(text) - used, "%s%s %llu", any ? ", " : "", name, static_cast<unsigned long long>(n));
        any = true;
        if (w > 0 && used + static_cast<size_t>(w) < sizeof(text)) used += static_cast<size_t>(w);
    }
    const uint64_t limited = g_farLimited.exchange(0, std::memory_order_relaxed);
    if (limited != 0) {
        const int w = std::snprintf(text + used, sizeof(text) - used, "%sfar glows limited %llu", any ? ", " : "", static_cast<unsigned long long>(limited));
        any = true;
        if (w > 0 && used + static_cast<size_t>(w) < sizeof(text)) used += static_cast<size_t>(w);
    }
    if (any) Log::get().note("concourse withheld 30s: %s", text);
}
}  // namespace

uint64_t concoursePaneTakeWithheld() noexcept { return g_withheld.exchange(0, std::memory_order_relaxed); }
ConcoursePaneMirrorStats concoursePaneTakeMirrorStats() noexcept {
    ConcoursePaneMirrorStats r;
    r.markPasses = g_markPasses.exchange(0, std::memory_order_relaxed);
    r.markRefused = g_markRefused.exchange(0, std::memory_order_relaxed);
    r.tests = g_testsRead.exchange(0, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(g_sideMutex);
    r.known = g_sides.knownCount();
    r.exterior = g_sides.exteriorCount();
    r.dropped = g_sides.dropped();
    return r;
}
const char* concoursePaneModeName() noexcept { return modeText(g_mode.load(std::memory_order_relaxed)); }
uint32_t concoursePaneSlotCount() noexcept { return g_listN.load(std::memory_order_relaxed); }
bool concoursePaneTreating() noexcept { return g_treatNow.load(std::memory_order_relaxed); }

void concoursePaneSetFeed(const ConcoursePaneFeed& feed) noexcept {
    g_feedMeasure.store(feed.measure, std::memory_order_relaxed);
    const bool treatBefore = g_feedTreat.exchange(feed.treat, std::memory_order_relaxed);
    if (!treatBefore && feed.treat) {   // a new visit to a concourse: what was learned about the panes is forgotten
        std::lock_guard<std::mutex> lock(g_sideMutex);
        g_sides.reset();
    }
    if (!g_feedWithhold.exchange(feed.withhold, std::memory_order_relaxed) && feed.withhold)
        g_plainFrom.store(g_frameNow.load(std::memory_order_relaxed), std::memory_order_relaxed);
    g_feedSealed.store(feed.sealed, std::memory_order_relaxed);
    g_bayOpacity.store(feed.bayOpacity, std::memory_order_relaxed);
    updateArmed();
}

ConcoursePaneShares concoursePaneShares() noexcept {
    std::lock_guard<std::mutex> lock(g_shareMutex);
    ConcoursePaneShares r = g_latest;
    if (r.valid && GetTickCount64() - g_latestMs > 250) r = ConcoursePaneShares{};   // no family draw on screen makes no reading: the bay is out of view, soon
    return r;
}

void concoursePaneTick(ID3D11Device* device, uint32_t frame, uint64_t nowMs) noexcept {
    g_lastTickMs = nowMs;
    logWithheld(nowMs);
    if (device != g_device) {
        releaseStates();
        g_poolMade = false;   // the old queries belong to the old device and are left alone
        for (size_t i = 0; i < kPool; ++i) g_pool[i] = nullptr;
        g_device = device;
        ID3D11DeviceContext* imm = nullptr;
        if (device) device->GetImmediateContext(&imm);
        g_immediate.store(imm, std::memory_order_relaxed);
        if (imm) imm->Release();
    }
    const uint32_t prevFrame = g_frameNow.exchange(frame, std::memory_order_relaxed);
    pollConfig(nowMs);
    const JournalRawStatus raw = journalRawStatus();
    const bool known = journalOnFootKnown() || (runtimeFlatProfile() && raw.gameplay);
    g_social = cc::concourseSocialOnFoot(journalStatusFlags2(), known, raw.gameplay);
    if (g_keyMode && g_keyMode->pressed()) {
        const uint32_t cur = g_mode.load(std::memory_order_relaxed);
        // mirror-lit, mirror-bright and mirror-soft are set by the key only: the press steps the five steady levels, then the look to compare them with.
        const uint32_t next = cur == kModeSteady100 ? kModeSteady200 : cur == kModeSteady200 ? kModeSteady400 : cur == kModeSteady400 ? kModeBrightest : cur == kModeBrightest ? kModeMirror
                              : cur == kModeMirror ? kModeTint : cur == kModeTint ? kModeOff : cur == kModeOff ? kModeSteady25 : cur == kModeSteady25 ? kModeSteady50 : kModeSteady100;
        setMode(next, g_social ? "hotkey.concourse_pane_mode" : "hotkey.concourse_pane_mode; it applies on foot in social space");
    }
    if (g_keyCapture && g_keyCapture->pressed()) {
        g_surveyRequested.store(true, std::memory_order_relaxed);
        if (!g_social) Log::get().note("concourse pane capture: refused: not on foot in social space (Status.json Flags2 bits 0 and 14, gameplay seen)");
        else if (!device) Log::get().note("concourse pane capture: refused: no device");
        else if (g_capState != CapState::kIdle) Log::get().note("concourse pane capture: refused: a capture is still open");
        else g_capPressPending = true;
    }
    // The capture arms one full frame: the open frame is the one between this tick and the next.
    if (g_capState == CapState::kIdle && g_capPressPending) {
        g_capPressPending = false;
        if (makePool(device)) {
            {
                std::lock_guard<std::mutex> lock(g_capMutex);
                g_capN = 0;
                g_capDropped = 0;
            }
            g_capState = CapState::kOpen;
            g_capFrame = frame;
            g_capturing.store(true, std::memory_order_release);
            Log::get().note("concourse pane capture: armed at frame %u: the next whole frame's blended draws (RT0 blending, a colour target, viewport at least 1000 wide) are measured", frame);
        }
    } else if (g_capState == CapState::kOpen && frame != g_capFrame) {
        g_capturing.store(false, std::memory_order_release);
        g_capState = CapState::kReading;
        g_capFrame = frame;
    } else if (g_capState == CapState::kReading) {
        pollResults(device);
        const bool done = allArrived();
        if (done || frame - g_capFrame >= 60) {
            if (!done) Log::get().note("concourse pane capture: %u frames passed with results still pending; reporting what arrived", frame - g_capFrame);
            reportCapture();
            g_capState = CapState::kIdle;
        }
    }
    if (device) {
        const bool wantStates = g_feedTreat.load(std::memory_order_relaxed);   // mode off too: the far-limited glows use the mark whatever the mode
        if (wantStates) {
            makeStates(device);
            if (g_statesReady.load(std::memory_order_acquire)) {
                ensureMarkVs(device);   // a change of diag.concourse_mirror_beyond compiles the vertex shader again
                g_markReady.store(g_rsMark.load(std::memory_order_acquire) != nullptr && g_vsMark.load(std::memory_order_acquire) != nullptr,
                                  std::memory_order_release);
                if (makeTests(device)) g_testsOn.store(true, std::memory_order_release);
            }
        }
        pollTests(device, frame);
        const bool measuring = g_feedMeasure.load(std::memory_order_relaxed);
        if (measuring && makeSlots(device)) g_shareOn.store(true, std::memory_order_release);
        if (!measuring) g_shareOn.store(false, std::memory_order_release);
        pollShares(device, prevFrame, frame, measuring, nowMs);
        readMaterials(device, frame);
    }
    updateArmed();
}

}  // namespace edvr
