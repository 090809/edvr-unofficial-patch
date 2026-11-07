// The concourse's glass panes: see concourse_pane.h. TEMPORARY (docs\concourse-cull-2026-11-05.md): removed when the arc closes.
#include "concourse_pane.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

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

enum Mode : uint32_t { kModeMirror = 0, kModeMirrorSoft = 1, kModeTint = 2, kModeOff = 3, kModeLit = 4, kModeBright = 5, kModeBrightest = 6, kModeSteady = 7, kModeBackdrop = 8 };

// The lit modes' glow pass, as a factor on the white, rough material: how much of the room's own light the pane gives back on top of its mirror. That pass is the
// glass shader, so it carries the sun and its shadow map, and they move as the station turns.
float glowOf(uint32_t m) noexcept { return m == kModeLit ? 1.0f : m == kModeBright ? 2.0f : m == kModeBrightest ? 4.0f : 0.0f; }
// The steady mode's glow pass: a flat HDR colour added over the mirror, with no light or shadow in it, so it does not change as the station turns. Its level is by
// window type (diag.concourse_glow: sky, bay, other); a pixel shader scales it by how much the pane faces the anchor's up axis.
constexpr float kSteadyColour[3] = {0.95f, 0.97f, 0.92f};   // a pale white, a little to the green of the station's light

const char* modeText(uint32_t m) noexcept {
    switch (m) {
        case kModeMirrorSoft: return "mirror-soft";
        case kModeTint: return "tint";
        case kModeOff: return "off";
        case kModeLit: return "mirror-lit";
        case kModeBright: return "mirror-bright";
        case kModeBrightest: return "mirror-brightest";
        case kModeSteady: return "mirror-steady";
        case kModeBackdrop: return "mirror-backdrop";
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
    else if (t == "mirror-steady") *out = kModeSteady;
    else if (t == "mirror-backdrop") *out = kModeBackdrop;
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
constexpr uint32_t kFlagProbePass = 1u << 11;   // mirror-backdrop: the probe pass's state is bound and its query is open (endProbePass puts it back)
constexpr uint32_t kFlagCapPass = 1u << 10;     // mirror-backdrop: the capture pass's state is bound (endCapturePass puts it back)
constexpr uint32_t kFlagSampler = 1u << 9;      // mirror-backdrop: the sampler slot holds EDVR's sampler (the game's is in prevSampler_)

// ---- The list, the mode and the feed (the draw threads read them; the tick and the feed write them) --------------------------------------------
std::atomic<uint32_t> g_mode{kModeSteady};
cc::PaneTriple g_list[cc::kPaneListCap] = {{cc::kPaneFamilyVs, cc::kPaneFamilyPs, 0}};   // entry 0 is the built-in glass family; diag.concourse_panes adds
std::atomic<uint32_t> g_listN{1};
std::atomic<bool> g_feedMeasure{false}, g_feedTreat{false}, g_feedWithhold{false}, g_feedSealed{false}, g_feedEffects{false};
std::atomic<bool> g_withholdSealedNow{false};   // the hangar is sealed: only the families marked sealedToo are not issued
std::atomic<float> g_bayOpacity{1.0f};
std::atomic<bool> g_treatNow{false};      // treat fed, a mode other than off, and the replacement states made
std::atomic<bool> g_withholdNow{false};   // withhold fed (the cull is on, the plain mirror phase): the mirror is marked; the mirror itself needs a mode other than off
std::atomic<bool> g_effectsNow{false};    // the outside draws (smoke, trails, lasers, the planet) are not issued, whatever the mode: the plain phase, or the backdrop capture's lift (feed.effects)
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
// The steady glow's own pixel shader and its constant buffer (b2: up.xyz, then the sky ratio in the second row's x), the levels, and the anchor's up axis.
std::atomic<ID3D11PixelShader*> g_psSky{nullptr};
std::atomic<ID3D11Buffer*> g_cbGlow{nullptr};      // b2 of the steady pass for every window but the bay: up, (sky, angled, other), colour
std::atomic<ID3D11Buffer*> g_cbGlowBay{nullptr};   // the same for the bay window: no up, its one level
std::atomic<bool> g_steadyFallbackSaid{false};
bool g_psSkyTried = false;
std::atomic<float> g_glowSky{cc::kGlowSkyDefault}, g_glowAngled{cc::kGlowAngledDefault}, g_glowOther{cc::kGlowOtherDefault}, g_glowBay{cc::kGlowBayDefault};   // diag.concourse_glow
std::string g_glowAppliedText;
bool g_glowTextSeen = false;
std::atomic<float> g_upFed[3] = {};
std::atomic<bool> g_upFedValid{false};
float g_glowCbSent[24] = {};     // the tick's: what the buffers hold (rows: up, levels, colour, then the backdrop's distance mode and radius)
float g_glowCbBaySent[24] = {};
bool g_glowCbSentValid = false;
bool g_glowLogged = false;
float g_glowLoggedLv[4] = {};
// The index count treated as the clearing pane (36, the bay window, until an exterior pane fills the view), and the entry share the feed carries.
std::atomic<uint32_t> g_clearN{cc::kBayIndexCount};
std::atomic<float> g_enterShareFed{0.0f};

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

// ---- The backdrop (PROTOTYPE on the concourse arc, docs\concourse-cull-2026-11-05.md "The window plan") ----------------------------------------------------
// Two cubes of view directions from a fixed origin O, filled by capture passes on a key press from frames with the cull off: colour (R16G16B16A16_FLOAT, alpha 1 where
// written, 0 elsewhere) and the distance from O (R16_FLOAT). `back` textures are written by the capture pass (UAVs); `front` textures have a full mip chain, are copied from
// the back ones by the tick after the capture frame and sampled by the display pass, so no resource is bound both ways. O is the camera's row of constant buffer 1 at the
// first capture of a visit, kept in a small buffer. Everything is made on the first capture, per device.
constexpr UINT kBdUavFirst = 6;      // u6 (distance) and u7 (colour) of the capture shader; the bound render target count must not exceed 6
constexpr UINT kBdUavCount = 2;
constexpr UINT kBdSceneSlot = 8;     // t8 t9 t10: the capture shader's scene colour, scene depth and origin; the display shader's colour cube, distance cube and origin
constexpr UINT kBdSrvCount = 3;
constexpr UINT kBdSamplerSlot = 7;   // s7 of the display shader
constexpr UINT kBdCb1Row = 275;      // the camera's row of the glass shaders' constant buffer 1 (INFERRED: the vertex shader subtracts cb1[275], the pixel shader rotates v2 without it)
std::atomic<bool> g_bdArmed{false};                 // a capture frame is open: pane draws copy the scene and issue capture passes
std::atomic<uint32_t> g_bdSceneFrame{0xFFFFFFFFu};  // the frame whose first pane draw tried the scene copy
std::atomic<uint32_t> g_bdSceneOkFrame{0xFFFFFFFFu};   // the frame whose scene, depth and origin copies were all taken
std::atomic<uint32_t> g_bdPaneDraws{0};             // capture passes issued since the arm
std::atomic<bool> g_bdSceneCopied{false};           // a scene copy was taken since the arm
std::atomic<void*> g_bdSceneSource{nullptr};        // the render target resource the copy came from (identity only; a capture pass needs the same one bound)
std::atomic<ID3D11Texture2D*> g_bdScene{nullptr}, g_bdDepth{nullptr};
std::atomic<ID3D11ShaderResourceView*> g_bdSceneSrv{nullptr}, g_bdDepthSrv{nullptr};
std::atomic<ID3D11Texture2D*> g_bdBack{nullptr}, g_bdFront{nullptr}, g_bdDistBack{nullptr}, g_bdDistFront{nullptr};
std::atomic<ID3D11UnorderedAccessView*> g_bdBackUav{nullptr}, g_bdDistBackUav{nullptr};
std::atomic<ID3D11ShaderResourceView*> g_bdFrontSrv{nullptr}, g_bdDistFrontSrv{nullptr};
std::atomic<ID3D11Buffer*> g_bdOrigin{nullptr};     // 16 bytes: the camera at the first capture of this visit (Buffer<float4> originBuf)
std::atomic<ID3D11ShaderResourceView*> g_bdOriginSrv{nullptr};
std::atomic<bool> g_bdOriginSet{false};             // g_bdOrigin holds this visit's origin
std::atomic<bool> g_bdOriginSetNow{false};          // the origin was set during the current capture (the capture line says set or kept)
std::atomic<uint32_t> g_bdCaptures{0};              // captures that went into this cube
std::atomic<bool> g_bdResetPending{false};          // a new visit (treat rose): the tick empties the cube and forgets the origin
std::atomic<uint32_t> g_bdParallax{cc::kParallaxDepth};   // the distance mode (hotkey.concourse_backdrop_parallax)
std::atomic<float> g_bdRadius{cc::kBackdropRadiusDefaultM};   // diag.concourse_backdrop_radius
std::atomic<ID3D11PixelShader*> g_psBdShow{nullptr}, g_psBdCapture{nullptr};
std::atomic<ID3D11SamplerState*> g_bdSampler{nullptr};
std::atomic<bool> g_bdCubeValid{false};             // front holds a capture
std::atomic<bool> g_bdFallbackSaid{false};
std::atomic<bool> g_bdSceneRefusedSaid{false};
std::atomic<bool> g_bdPassRefusedSaid{false};
std::mutex g_bdMutex;                               // the scene and depth textures' (re)making and the status text below
uint32_t g_bdSceneW = 0, g_bdSceneH = 0;
DXGI_FORMAT g_bdSceneFmt = DXGI_FORMAT_UNKNOWN;
uint32_t g_bdDepthW = 0, g_bdDepthH = 0;
DXGI_FORMAT g_bdDepthFmt = DXGI_FORMAT_UNKNOWN;
char g_bdSceneFmtText[28] = {};
char g_bdRefusal[200] = {};
std::atomic<uint8_t> g_bdPhase{0};                  // ConcourseBackdropPhase
std::atomic<bool> g_bdArmRequested{false};
uint32_t g_bdArmFrame = 0;                          // the tick's
uint32_t g_bdFramesWaited = 0;
// The glass look (diag.concourse_glass: r, g, b, haze, blur, fresnel) and its text.
std::atomic<float> g_glass[6] = {{cc::kGlassDefault[0]}, {cc::kGlassDefault[1]}, {cc::kGlassDefault[2]}, {cc::kGlassDefault[3]}, {cc::kGlassDefault[4]}, {cc::kGlassDefault[5]}};
std::string g_glassAppliedText;
bool g_glassTextSeen = false;
// Automatic capture: the probe pass (a colourless pass whose occlusion count is the pixels the cube cannot show), what it measured, and the sticky enable a hotkey sets.
std::atomic<bool> g_bdAutoOn{false};                // a hotkey (F4 to backdrop, or F6) turned automatic capture on; it runs only while the mode is mirror-backdrop
std::atomic<bool> g_bdToast{true};                  // the capture in progress was F6's: its result is a toast (an automatic capture says nothing on screen)
std::atomic<uint64_t> g_probeIssued{0};             // probe passes issued (the draw threads)
std::atomic<uint64_t> g_probeRead{0};               // probe results read (the tick)
std::atomic<ID3D11PixelShader*> g_psBdProbe{nullptr};
struct MissEntry {
    uint32_t frame = 0;
    double missing = 0.0;
};
constexpr size_t kMissEntries = 32;
constexpr uint32_t kMissWindowFrames = 8;
MissEntry g_miss[kMissEntries];                     // the tick's: measured missing samples by the frame they were drawn in
size_t g_missNext = 0;
double g_missPixels = 0.0;                          // the viewport area of the latest measurement
std::atomic<float> g_missShare{0.0f};               // the tick's result: the share of the viewport missing over the last kMissWindowFrames frames
Hotkey* g_keyParallax = nullptr;
std::string g_keyParallaxText;
std::string g_radiusAppliedText;
bool g_radiusTextSeen = false;// The toast, one slot: the newest text wins (hotkey changes of the mode, the capture's result). The tick's thread writes it and menuTick takes it.
std::mutex g_toastMutex;
char g_toastText[64] = {};
bool g_toastDue = false;
void queueToast(const char* text) noexcept {
    std::lock_guard<std::mutex> lock(g_toastMutex);
    std::snprintf(g_toastText, sizeof(g_toastText), "%s", text);
    g_toastDue = true;
}
void releaseBackdrop() noexcept;

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
    ID3D11Query* qProbe = nullptr;   // the backdrop's probe pass (between the game's draw and the display pass); used only on a test frame in mirror-backdrop with a cube
    uint8_t probe = 0;               // 0 no probe in this test, 1 its query is open, 2 closed (read with the other two)
    uint8_t state = 0;   // 0 free, 1 the game's draw open, 2 closed and waiting for the mirror, 3 the mirror open, 4 both closed, not yet read
    uint32_t frame = 0;
    cc::PaneKey key;
    double pixels = 0.0;   // the viewport's area at the draw (the share's denominator)
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
    ID3D11DepthStencilState* probe = nullptr;
    ID3D11DepthStencilState* markedProbe = nullptr;
};
enum class DsVariant { kPlain, kMarked, kFarOut, kProbe, kMarkedProbe };
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
    releaseBackdrop();   // the exception: the cube is about 230 MB
    g_statesReady.store(false, std::memory_order_release);
    g_bsFactor.store(nullptr, std::memory_order_release);
    g_bsMirror.store(nullptr, std::memory_order_release);
    g_bsFade.store(nullptr, std::memory_order_release);
    g_bsGlow.store(nullptr, std::memory_order_release);
    g_glowT10.store(nullptr, std::memory_order_release);
    g_psWhite.store(nullptr, std::memory_order_release);
    g_psSky.store(nullptr, std::memory_order_release);
    g_cbGlow.store(nullptr, std::memory_order_release);
    g_cbGlowBay.store(nullptr, std::memory_order_release);
    g_psSkyTried = false;
    g_glowCbSentValid = false;
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

// The steady pane's pixel shader: it replaces the glass shader for the whole pane (one flat opaque draw, so the glass shader's reflection of a horizon cannot show).
// Its inputs are the first two outputs of the glass vertex shader (matched by name and order). The level is told by how much the pane faces the anchor's up axis:
// overhead (sky), angled, other. A zero `up` gives `other`. A failed compile is logged once and the steady mode falls back to the plain mirror.
// The steady colour of a pane from its lighting normal and b2 (up, the levels, the colour): shared by the steady shader and the backdrop's fallback, so the two are the same text.
static const char kSkyCommon[] =
    "cbuffer GlowCb : register(b2) { float4 up; float4 lv; float4 colour; float4 par; float4 glassA; float4 glassB; };\n"
    "float3 fallbackColour(float3 n) {\n"
    "    float ul = length(up.xyz);\n"
    "    float3 nn = n * rsqrt(max(dot(n, n), 1e-12));\n"
    "    float d = ul > 0.5 ? abs(dot(nn, up.xyz / ul)) : 0.0;\n"
    "    float lvl = lerp(lv.z, lv.y, smoothstep(0.35, 0.6, d));\n"
    "    lvl = lerp(lvl, lv.x, smoothstep(0.92, 0.98, d));\n"
    "    return colour.rgb * lvl;\n"
    "}\n";

void makeSkyPs(ID3D11Device* device) noexcept {
    if (g_psSkyTried) return;
    g_psSkyTried = true;
    ID3D11DeviceContext* imm = nullptr;
    device->GetImmediateContext(&imm);
    if (!imm) return;
    static const std::string kSky = std::string(kSkyCommon) +
        "struct PsIn { nointerpolation uint face : __USER_VERTEX_FACEINVARIANT0; float3 n : __USER_VERTEX_M_LIGHTINGNORMAL0; };\n"
        "float4 main(PsIn i) : SV_Target {\n"
        "    return float4(fallbackColour(i.n), 1.0);\n"
        "}\n";
    ID3D11PixelShader* ps = shaderSwapCompilePs(imm, kSky.c_str(), kSky.size(), "main", "concourse_pane_sky", nullptr, "concourse pane");
    imm->Release();
    g_psSky.store(ps, std::memory_order_release);
    if (!ps) Log::get().note("concourse pane: the steady pixel shader did not compile; mirror-steady draws the plain mirror");
}

bool makeGlowCb(ID3D11Device* device) noexcept {
    float init[24] = {0, 0, 0, 0, 1, 1, 1, 0, kSteadyColour[0], kSteadyColour[1], kSteadyColour[2], 0, static_cast<float>(cc::kParallaxDepth), cc::kBackdropRadiusDefaultM, 0, 0,
                      cc::kGlassDefault[0], cc::kGlassDefault[1], cc::kGlassDefault[2], cc::kGlassDefault[3], cc::kGlassDefault[4], cc::kGlassDefault[5], 0, 0};
    ID3D11Buffer* made[2] = {};
    for (int i = 0; i < 2; ++i) {
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = sizeof(init);
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        D3D11_SUBRESOURCE_DATA sd{};
        sd.pSysMem = init;
        const HRESULT h = device->CreateBuffer(&bd, &sd, &made[i]);
        if (FAILED(h) || !made[i]) {
            Log::get().note("concourse pane: the steady constant buffers could not be made (hr %08X); mirror-steady draws the plain mirror", static_cast<unsigned>(h));
            if (made[0]) made[0]->Release();
            return false;
        }
    }
    std::memcpy(g_glowCbSent, init, sizeof(init));
    std::memcpy(g_glowCbBaySent, init, sizeof(init));
    g_glowCbSentValid = true;
    g_cbGlowBay.store(made[1], std::memory_order_release);
    g_cbGlow.store(made[0], std::memory_order_release);
    return true;
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
    // The probe queries (automatic capture): a failure costs only the probes, said once; the tests go on.
    bool probesOk = true;
    for (size_t i = 0; i < kTestSlots && probesOk; ++i) probesOk = SUCCEEDED(device->CreateQuery(&qd, &g_tests[i].qProbe)) && g_tests[i].qProbe != nullptr;
    if (!probesOk) {
        Log::get().note("concourse backdrop: the probe queries could not be made; automatic capture cannot measure what is missing (F6 still captures)");
        for (auto& s : g_tests) {
            if (s.qProbe) s.qProbe->Release();
            s.qProbe = nullptr;
        }
    }
    g_testsMade = true;
    return true;
}

bool makeStates(ID3D11Device* device) noexcept {
    if (!device) return false;
    if (g_statesReady.load(std::memory_order_acquire)) return true;
    makeWhitePs(device);
    makeSkyPs(device);
    if (!g_cbGlow.load(std::memory_order_acquire)) makeGlowCb(device);
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
                if (best->n[k] == g_clearN.load(std::memory_order_relaxed) && best->share[k] > r.bay) r.bay = best->share[k];
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
// kProbe is the backdrop capture pass's: the game's depth test, no depth write, no stencil. kPlain and kMarked are the mirror's: depth write on, without and with the mark's test. kFarOut is the far-limited glows': the game's depth state as it is, and only
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
    ID3D11DepthStencilState*& slot = variant == DsVariant::kFarOut ? g_ds[at].farOut : variant == DsVariant::kMarked ? g_ds[at].marked
                                   : variant == DsVariant::kProbe ? g_ds[at].probe : variant == DsVariant::kMarkedProbe ? g_ds[at].markedProbe : g_ds[at].plain;
    if (slot) return slot;
    D3D11_DEPTH_STENCIL_DESC d{};
    orig->GetDesc(&d);
    if (variant == DsVariant::kProbe || variant == DsVariant::kMarkedProbe) d.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;   // (kMarkedProbe: the display's mark test, nothing written)   // the backdrop's capture pass: the game's depth test, nothing written
    else if (variant != DsVariant::kFarOut) d.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    if (variant != DsVariant::kPlain && variant != DsVariant::kProbe) {
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
    D3D11_VIEWPORT vp{};
    UINT nvp = 1;
    ctx->RSGetViewports(&nvp, &vp);
    const double pixels = nvp >= 1 ? static_cast<double>(vp.Width) * static_cast<double>(vp.Height) : 0.0;
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
        g_tests[idx].probe = 0;
        g_tests[idx].frame = g_frameNow.load(std::memory_order_relaxed);
        g_tests[idx].key = key;
        g_tests[idx].pixels = pixels;
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
    g_tests[idx].probe = 0;
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
    // The clearing pane: in the plain phase, an exterior mesh whose glass fills the view as much as the entry share is a candidate (the largest wins).
    uint32_t candN = 0;
    float candShare = 0.0f;
    const bool plainNow = g_withholdNow.load(std::memory_order_relaxed);
    const float enterShare = g_enterShareFed.load(std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(g_sideMutex);
        if (g_testsMade) {
            FlatComputeInternalScope internal;
            for (auto& t : g_tests) {
                if (t.state != 4) {
                    if (t.state != 0 && static_cast<uint32_t>(frame - t.frame) > 600u) {
                        t.state = 0;   // stuck open (a draw that never reached its end)
                        t.probe = 0;
                    }
                    continue;
                }
                UINT64 drawn = 0, mirrored = 0, probed = 0;
                const bool hadProbe = t.probe == 2;
                const bool arrived = imm->GetData(t.qDrawn, &drawn, sizeof(drawn), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK &&
                                     imm->GetData(t.qMirror, &mirrored, sizeof(mirrored), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK &&
                                     (!hadProbe || imm->GetData(t.qProbe, &probed, sizeof(probed), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK);
                if (!arrived) {
                    if (static_cast<uint32_t>(frame - t.frame) > 600u) {   // a query that never returns: the slot is freed (the probe counters then show issued without read)
                        t.state = 0;
                        t.probe = 0;
                    }
                    continue;
                }
                const cc::PaneKey key = t.key;
                const double pixels = t.pixels;
                const uint32_t drawFrame = t.frame;
                t.state = 0;
                t.probe = 0;
                g_testsRead.fetch_add(1, std::memory_order_relaxed);
                if (hadProbe) g_probeRead.fetch_add(1, std::memory_order_relaxed);
                const cc::PaneSides::Change c = g_sides.note(key, drawn, mirrored);
                // The backdrop's missing share: what a probe counted (pixels the cube cannot show), or, with no cube yet, the whole of what the pane would draw (the mirror count).
                if (g_mode.load(std::memory_order_relaxed) == kModeBackdrop && pixels > 0.0 && g_sides.side(key) == cc::PaneSide::kExterior) {
                    double miss = -1.0;
                    if (hadProbe) miss = static_cast<double>(probed);
                    else if (!g_bdCubeValid.load(std::memory_order_relaxed)) miss = static_cast<double>(mirrored);
                    if (miss >= 0.0) {
                        g_miss[g_missNext].frame = drawFrame;
                        g_miss[g_missNext].missing = miss;
                        g_missNext = (g_missNext + 1) % kMissEntries;
                        g_missPixels = pixels;
                    }
                }
                if (plainNow && enterShare > 0.0f && pixels > 0.0 && g_sides.side(key) == cc::PaneSide::kExterior) {
                    const float share = static_cast<float>(static_cast<double>(drawn) / pixels);
                    if (share >= enterShare * cc::clearEntryScale(key.n) && share > candShare && key.n != g_clearN.load(std::memory_order_relaxed)) {
                        candN = key.n;
                        candShare = share;
                    }
                }
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
    if (candN != 0) {
        // Against the latest share of the pane that is the clearing pane now (0 when it is not on screen). The shares' lock is taken after the table's is let go.
        float cur = 0.0f;
        {
            std::lock_guard<std::mutex> lock(g_shareMutex);
            const uint32_t now = g_clearN.load(std::memory_order_relaxed);
            for (uint32_t k = 0; g_latest.valid && k < g_latest.count; ++k)
                if (g_latest.n[k] == now) cur = g_latest.share[k];
        }
        if (candShare > cur && g_clearN.exchange(candN, std::memory_order_relaxed) != candN)
            Log::get().note("concourse pane: the clearing pane is now n=%u (share %.1f%%)", candN, 100.0 * static_cast<double>(candShare));
    }
}

// ---- The mark pass -----------------------------------------------------------------------------------------------------------------------------
// ---- The backdrop's resources and the scene copy (PROTOTYPE, docs\concourse-cull-2026-11-05.md) ------------------------------------------------------------
template <class T>
void releaseAtomic(std::atomic<T*>& a) noexcept {
    T* p = a.exchange(nullptr, std::memory_order_acq_rel);   // the draw threads may hold the pointer for one more draw
    if (p) p->Release();
}

void releaseBackdrop() noexcept {
    g_bdArmed.store(false, std::memory_order_release);
    g_bdCubeValid.store(false, std::memory_order_release);
    g_bdOriginSet.store(false, std::memory_order_release);
    g_bdCaptures.store(0, std::memory_order_release);
    g_bdSceneSource.store(nullptr, std::memory_order_release);
    g_bdSceneFrame.store(0xFFFFFFFFu, std::memory_order_relaxed);
    g_bdSceneOkFrame.store(0xFFFFFFFFu, std::memory_order_relaxed);
    releaseAtomic(g_bdSceneSrv);
    releaseAtomic(g_bdScene);
    releaseAtomic(g_bdDepthSrv);
    releaseAtomic(g_bdDepth);
    releaseAtomic(g_bdOriginSrv);
    releaseAtomic(g_bdOrigin);
    releaseAtomic(g_bdBackUav);
    releaseAtomic(g_bdDistBackUav);
    releaseAtomic(g_bdFrontSrv);
    releaseAtomic(g_bdDistFrontSrv);
    releaseAtomic(g_bdBack);
    releaseAtomic(g_bdFront);
    releaseAtomic(g_bdDistBack);
    releaseAtomic(g_bdDistFront);
    releaseAtomic(g_psBdShow);
    releaseAtomic(g_psBdCapture);
    releaseAtomic(g_psBdProbe);
    releaseAtomic(g_bdSampler);
    std::lock_guard<std::mutex> lock(g_bdMutex);
    g_bdSceneW = g_bdSceneH = 0;
    g_bdSceneFmt = DXGI_FORMAT_UNKNOWN;
    g_bdDepthW = g_bdDepthH = 0;
    g_bdDepthFmt = DXGI_FORMAT_UNKNOWN;
    g_bdSceneFmtText[0] = '\0';
}
const char* bdFormatName(DXGI_FORMAT f, char* fb, size_t cap) noexcept {
    switch (f) {
        case DXGI_FORMAT_R16G16B16A16_FLOAT: return "R16G16B16A16_FLOAT";
        case DXGI_FORMAT_R32G32B32A32_FLOAT: return "R32G32B32A32_FLOAT";
        case DXGI_FORMAT_R11G11B10_FLOAT: return "R11G11B10_FLOAT";
        case DXGI_FORMAT_R10G10B10A2_UNORM: return "R10G10B10A2_UNORM";
        case DXGI_FORMAT_R8G8B8A8_UNORM: return "R8G8B8A8_UNORM";
        case DXGI_FORMAT_B8G8R8A8_UNORM: return "B8G8R8A8_UNORM";
        case DXGI_FORMAT_R16G16B16A16_TYPELESS: return "R16G16B16A16_TYPELESS";
        default: break;
    }
    const char* n = formatName(f);
    if (n) return n;
    std::snprintf(fb, cap, "format %u", static_cast<unsigned>(f));
    return fb;
}

bool formatIsTypeless(DXGI_FORMAT f) noexcept {
    switch (f) {
        case DXGI_FORMAT_R32G32B32A32_TYPELESS: case DXGI_FORMAT_R32G32B32_TYPELESS: case DXGI_FORMAT_R16G16B16A16_TYPELESS: case DXGI_FORMAT_R32G32_TYPELESS:
        case DXGI_FORMAT_R32G8X24_TYPELESS: case DXGI_FORMAT_R10G10B10A2_TYPELESS: case DXGI_FORMAT_R8G8B8A8_TYPELESS: case DXGI_FORMAT_R16G16_TYPELESS:
        case DXGI_FORMAT_R32_TYPELESS: case DXGI_FORMAT_R24G8_TYPELESS: case DXGI_FORMAT_R8G8_TYPELESS: case DXGI_FORMAT_R16_TYPELESS: case DXGI_FORMAT_R8_TYPELESS:
        case DXGI_FORMAT_B8G8R8A8_TYPELESS: case DXGI_FORMAT_B8G8R8X8_TYPELESS:
            return true;
        default: return false;
    }
}

bool bdRefuse(const char* why) noexcept {
    {
        std::lock_guard<std::mutex> lock(g_bdMutex);
        std::snprintf(g_bdRefusal, sizeof(g_bdRefusal), "%s", why);
    }
    Log::get().note("concourse backdrop: capture refused: %s", why);
    return false;
}

UINT bdMipCount() noexcept {
    UINT n = 1;
    for (UINT s = cc::kBackdropCubeSize; s > 1; s >>= 1) ++n;
    return n;
}

// The back textures' mip 0 goes into the front ones slice by slice (their mip counts differ, so not CopyResource), then the front mips are built. Unwritten texels are zero
// in every channel and written ones have alpha 1, so each mip is premultiplied by coverage (its alpha), in the colour cube and in the distance cube alike.
void backdropPublishFront(ID3D11DeviceContext* imm) noexcept {
    ID3D11Texture2D* back = g_bdBack.load(std::memory_order_acquire);
    ID3D11Texture2D* front = g_bdFront.load(std::memory_order_acquire);
    ID3D11Texture2D* dback = g_bdDistBack.load(std::memory_order_acquire);
    ID3D11Texture2D* dfront = g_bdDistFront.load(std::memory_order_acquire);
    ID3D11ShaderResourceView* srv = g_bdFrontSrv.load(std::memory_order_acquire);
    ID3D11ShaderResourceView* dsrv = g_bdDistFrontSrv.load(std::memory_order_acquire);
    if (!imm || !back || !front || !dback || !dfront || !srv || !dsrv) return;
    FlatComputeInternalScope internal;
    const UINT mips = bdMipCount();
    for (UINT slice = 0; slice < 6; ++slice) {
        imm->CopySubresourceRegion(front, D3D11CalcSubresource(0, slice, mips), 0, 0, 0, back, D3D11CalcSubresource(0, slice, 1), nullptr);
        imm->CopySubresourceRegion(dfront, D3D11CalcSubresource(0, slice, mips), 0, 0, 0, dback, D3D11CalcSubresource(0, slice, 1), nullptr);
    }
    imm->GenerateMips(srv);
    imm->GenerateMips(dsrv);
}

// A new visit (treat rose): the back cubes are emptied, the front ones follow, the origin is forgotten.
void backdropReset(ID3D11Device* device) noexcept {
    const bool had = g_bdCubeValid.load(std::memory_order_relaxed) || g_bdOriginSet.load(std::memory_order_relaxed);
    ID3D11UnorderedAccessView* u1 = g_bdBackUav.load(std::memory_order_acquire);
    ID3D11UnorderedAccessView* u2 = g_bdDistBackUav.load(std::memory_order_acquire);
    if (had && device && u1 && u2) {
        ID3D11DeviceContext* imm = nullptr;
        device->GetImmediateContext(&imm);
        if (imm) {
            {
                FlatComputeInternalScope internal;
                const FLOAT zero[4] = {};
                imm->ClearUnorderedAccessViewFloat(u1, zero);
                imm->ClearUnorderedAccessViewFloat(u2, zero);
            }
            backdropPublishFront(imm);
            imm->Release();
        }
    }
    g_bdCubeValid.store(false, std::memory_order_release);
    g_bdOriginSet.store(false, std::memory_order_release);
    const uint32_t n = g_bdCaptures.exchange(0, std::memory_order_acq_rel);
    if (had) Log::get().note("concourse backdrop: the cube is emptied and the origin forgotten (a new visit; it held %u capture%s)", n, n == 1 ? "" : "s");
}

static std::string bdFloatText(double v) {
    char b[40];
    std::snprintf(b, sizeof(b), "%.9g", v);
    return b;
}

// The capture shader (an exterior pane's pixel) and the display shader. Both declare the glass vertex shader's outputs in order (v0 face, v1 lighting normal, v2 lighting
// position = the world position relative to the camera, INFERRED); the capture shader also declares what lies before SV_POSITION in the game's own pixel shader (v3 tangent,
// v4 texcoord). Both read cb1[275].xyz as the camera in the game's world frame (the glass constant buffer, bound at b1 by the game and left alone) and an origin O from a
// 16-byte buffer (t10).
//   Capture: dir = normalize(v2); the scene's depth z gives the view depth 0.025 / z (reversed infinite Z), along the ray t = depth / |dot(cb1[279].xyz, dir)| (cb1[279] is the
//   view Z axis, INFERRED); the hit H = C + dir * t; rel = H - O. The colour goes to the colour cube's texel of rel (alpha 1) and |rel| to the distance cube's. A hit nearer than
//   5 m is not backdrop and is not written.
//   Display, per pixel: 1 constant-buffer row read and 1 buffer load; None: 6 cube samples (lods 0 1 2 3 5 7). Sphere: the same, with the direction moved. Depth: 4 marching steps
//   of 2 samples each (the colour cube's alpha at lod 1, and the distance cube at lod 1) and the final 6, so 14 cube samples. Mips fill the holes the splat leaves: the lookup
//   composites lod 0 over the premultiplied coarser lods front to back.
bool makeBackdropShaders(ID3D11Device* device) noexcept {
    ID3D11DeviceContext* imm = nullptr;
    device->GetImmediateContext(&imm);
    if (!imm) return false;
    const std::string cb1 = "cbuffer Cb1 : register(b1) { float4 cb1[327]; };\n";
    // The display and the probe share everything up to main: the inputs, the resources, the look-up and the direction `s` (what the pane looks up).
    const std::string head = std::string(kSkyCommon) + cb1 +
        "struct PsIn { nointerpolation uint face : __USER_VERTEX_FACEINVARIANT0; float3 n : __USER_VERTEX_M_LIGHTINGNORMAL0; float3 wp : __USER_VERTEX_M_LIGHTINGPOSITION0; };\n"
        "TextureCube<float4> backdrop : register(t8);\n"
        "TextureCube<float> distMap : register(t9);\n"
        "Buffer<float4> originBuf : register(t10);\n"
        "SamplerState backdropSmp : register(s7);\n"
        // The composite of lods 0 1 2 3 5 7 (up to lastLod), each plus the glass's blur, front to back over premultiplied colour.
        "float4 lookup(float3 s, float blur, float lastLod) {\n"
        "    float4 acc = float4(0, 0, 0, 0);\n"
        "    static const float L[6] = {0.0, 1.0, 2.0, 3.0, 5.0, 7.0};\n"
        "    [unroll] for (int k = 0; k < 6; ++k) {\n"
        "        if (L[k] <= lastLod) {\n"
        "            float4 c = backdrop.SampleLevel(backdropSmp, s, L[k] + blur);\n"
        "            acc.rgb += (1.0 - acc.a) * c.rgb;\n"
        "            acc.a += (1.0 - acc.a) * c.a;\n"
        "        }\n"
        "    }\n"
        "    return acc;\n"
        "}\n"
        "float3 backdropS(PsIn i, out float3 dir) {\n"
        "    dir = i.wp * rsqrt(max(dot(i.wp, i.wp), 1e-12));\n"
        "    float3 off = cb1[275].xyz - originBuf[0].xyz;\n"
        "    float3 s = dir;\n"
        "    float mode = par.x;\n"
        "    float R = max(par.y, 1.0);\n"
        "    if (mode > 0.5) {\n"
        "        float b = dot(off, dir);\n"
        "        float cq = dot(off, off) - R * R;\n"
        "        float t = -b + sqrt(max(b * b - cq, 0.0));\n"
        "        if (mode > 1.5) {\n"
        "            t = R;\n"
        "            [unroll] for (int k = 0; k < 4; ++k) {\n"
        "                float3 rel = off + dir * t;\n"
        "                float cov = backdrop.SampleLevel(backdropSmp, rel, 1).a;\n"
        "                if (cov > 0.01) {\n"
        "                    float r = distMap.SampleLevel(backdropSmp, rel, 1) / max(cov, 1e-3);\n"
        "                    float3 hit = normalize(rel) * r;\n"
        "                    t = max(dot(hit - off, dir), 1.0);\n"
        "                }\n"
        "            }\n"
        "        }\n"
        "        s = off + dir * t;\n"
        "    }\n"
        "    return s;\n"
        "}\n";
    // Display: glass after the composite. Tint on the captured colour (premultiplied, so only where captured), the level colour under what is not captured (the fallback,
    // unchanged), then the glass's own sheen: haze plus a Fresnel term at grazing angles, mixing toward the level colour. Samples: the look-up's 6 (blur is a bias, not
    // extra samples) plus, in the depth mode, 8 for the march: 14 cube samples at most, 1 buffer load, 1 constant-buffer row.
    const std::string show = head +
        "float4 main(PsIn i) : SV_Target {\n"
        "    float3 fb = fallbackColour(i.n);\n"
        "    float3 dir;\n"
        "    float3 s = backdropS(i, dir);\n"
        "    float4 acc = lookup(s, glassB.x, 7.0);\n"
        "    float3 base = acc.rgb * glassA.rgb + (1.0 - acc.a) * fb;\n"
        "    float f = glassB.y * pow(saturate(1.0 - abs(dot(i.n * rsqrt(max(dot(i.n, i.n), 1e-12)), dir))), 5.0);\n"
        "    return float4(lerp(base, fb, saturate(glassA.w + f)), 1.0);\n"
        "}\n";
    // Probe: the same direction and the same blur; a pixel whose composite coverage through lod 3 is under one half survives, and the occlusion count is those pixels.
    const std::string probe = head +
        "float4 main(PsIn i) : SV_Target {\n"
        "    float3 dir;\n"
        "    float3 s = backdropS(i, dir);\n"
        "    float4 acc = lookup(s, glassB.x, 3.0);\n"
        "    clip(0.5 - acc.a);\n"
        "    return float4(0, 0, 0, 0);\n"
        "}\n";    const std::string capture = std::string(cc::cubeTexelHlsl()) + cb1 +
        "struct PsIn { nointerpolation uint face : __USER_VERTEX_FACEINVARIANT0; float3 n : __USER_VERTEX_M_LIGHTINGNORMAL0; float3 wp : __USER_VERTEX_M_LIGHTINGPOSITION0;\n"
        "              float3 tg : __USER_VERTEX_M_LIGHTINGTANGENT0; float2 uv : __USER_VERTEX_M_TEXCOORD0; float4 p : SV_POSITION; };\n"
        "Texture2D<float4> scene : register(t8);\n"
        "Texture2D<float> sceneDepth : register(t9);\n"
        "Buffer<float4> originBuf : register(t10);\n"
        "RWTexture2DArray<float> distCube : register(u6);\n"
        "RWTexture2DArray<float4> cube : register(u7);\n"
        "[earlydepthstencil]\n"
        "void main(PsIn i) {\n"
        "    float3 keep = (i.face == 0xDEADBEEFu) ? float3(i.tg.x, i.n.y + i.uv.x, i.n.z) : float3(0, 0, 0);\n"   // never true: keeps v3 and v4 in the signature
        "    float3 dir = i.wp + keep;\n"
        "    float dl = dot(dir, dir);\n"
        "    if (dl < 1e-12) return;\n"
        "    dir *= rsqrt(dl);\n"
        "    float z = sceneDepth.Load(int3(i.p.xy, 0));\n"
        "    float vd = z > 0.0 ? " + bdFloatText(cc::kBackdropNearZ) + " / z : " + bdFloatText(cc::kBackdropFarM) + ";\n"
        "    float t = min(vd / max(abs(dot(cb1[279].xyz, dir)), 1e-3), " + bdFloatText(cc::kBackdropFarM) + ");\n"
        "    if (t < " + bdFloatText(cc::kBackdropMinDistM) + ") return;\n"
        "    float3 rel = cb1[275].xyz + dir * t - originBuf[0].xyz;\n"
        "    float rl = length(rel);\n"
        "    if (!(rl > 1e-3)) return;\n"
        "    uint3 tx = cubeTexel(rel, " + std::to_string(cc::kBackdropCubeSize) + "u);\n"
        "    float3 c = clamp(scene.Load(int3(i.p.xy, 0)).rgb, 0.0, 60000.0);\n"
        "    cube[tx] = float4(c, 1.0);\n"
        "    distCube[tx] = clamp(rl, 1.0, " + bdFloatText(cc::kBackdropFarM) + ");\n"
        "}\n";
    ID3D11PixelShader* psShow = shaderSwapCompilePs(imm, show.c_str(), show.size(), "main", "concourse_backdrop_show", nullptr, "concourse backdrop");
    ID3D11PixelShader* psCap = shaderSwapCompilePs(imm, capture.c_str(), capture.size(), "main", "concourse_backdrop_capture", nullptr, "concourse backdrop");
    ID3D11PixelShader* psProbe = shaderSwapCompilePs(imm, probe.c_str(), probe.size(), "main", "concourse_backdrop_probe", nullptr, "concourse backdrop");
    imm->Release();
    if (!psShow || !psCap || !psProbe) {
        if (psShow) psShow->Release();
        if (psCap) psCap->Release();
        if (psProbe) psProbe->Release();
        return bdRefuse(!psShow ? "the display pixel shader did not compile" : !psCap ? "the capture pixel shader did not compile" : "the probe pixel shader did not compile");
    }
    g_psBdShow.store(psShow, std::memory_order_release);
    g_psBdCapture.store(psCap, std::memory_order_release);
    g_psBdProbe.store(psProbe, std::memory_order_release);
    return true;
}

bool bdFormatOk(ID3D11Device* device, DXGI_FORMAT fmt, const char* name) noexcept {
    UINT sup = 0;
    const UINT need = D3D11_FORMAT_SUPPORT_TEXTURE2D | D3D11_FORMAT_SUPPORT_TEXTURECUBE | D3D11_FORMAT_SUPPORT_SHADER_SAMPLE | D3D11_FORMAT_SUPPORT_TYPED_UNORDERED_ACCESS_VIEW |
                      D3D11_FORMAT_SUPPORT_RENDER_TARGET | D3D11_FORMAT_SUPPORT_MIP_AUTOGEN;
    char why[160];
    if (FAILED(device->CheckFormatSupport(fmt, &sup)) || (sup & need) != need) {
        std::snprintf(why, sizeof(why), "%s cannot be a sampled, mip-generated cube and a typed UAV on this device", name);
        return bdRefuse(why);
    }
    D3D11_FEATURE_DATA_FORMAT_SUPPORT2 f2{};
    f2.InFormat = fmt;
    if (FAILED(device->CheckFeatureSupport(D3D11_FEATURE_FORMAT_SUPPORT2, &f2, sizeof(f2))) || (f2.OutFormatSupport2 & D3D11_FORMAT_SUPPORT2_UAV_TYPED_STORE) == 0) {
        std::snprintf(why, sizeof(why), "%s has no typed UAV store on this device", name);
        return bdRefuse(why);
    }
    return true;
}

// The cube pairs, the views, the origin buffer, the sampler and both shaders; made once per device (releaseBackdrop undoes it). A refusal is one log line and leaves nothing
// half-made.
bool makeBackdropResources(ID3D11Device* device) noexcept {
    if (g_bdBack.load() && g_bdFront.load() && g_bdDistBack.load() && g_bdDistFront.load() && g_bdBackUav.load() && g_bdDistBackUav.load() && g_bdFrontSrv.load() &&
        g_bdDistFrontSrv.load() && g_bdOrigin.load() && g_bdOriginSrv.load() && g_psBdCapture.load() && g_psBdShow.load() && g_psBdProbe.load() && g_bdSampler.load())
        return true;
    releaseBackdrop();
    const DXGI_FORMAT cfmt = DXGI_FORMAT_R16G16B16A16_FLOAT, dfmt = DXGI_FORMAT_R16_FLOAT;
    if (!bdFormatOk(device, cfmt, "R16G16B16A16_FLOAT") || !bdFormatOk(device, dfmt, "R16_FLOAT")) return false;
    const UINT size = cc::kBackdropCubeSize;
    const UINT mips = bdMipCount();
    std::vector<uint8_t> zero(static_cast<size_t>(size) * size * 8u, 0);   // zero everywhere (alpha 0: nothing captured); the distance cube uses its first two bytes a texel
    D3D11_SUBRESOURCE_DATA cinit[6], dinit[6];
    for (int k = 0; k < 6; ++k) {
        cinit[k].pSysMem = zero.data();
        cinit[k].SysMemPitch = size * 8u;
        cinit[k].SysMemSlicePitch = size * 8u * size;
        dinit[k].pSysMem = zero.data();
        dinit[k].SysMemPitch = size * 2u;
        dinit[k].SysMemSlicePitch = size * 2u * size;
    }
    ID3D11Texture2D* tex[4] = {};   // colour back, colour front, distance back, distance front
    HRESULT h[4] = {};
    for (int k = 0; k < 4; ++k) {
        const bool isBack = (k % 2) == 0;
        D3D11_TEXTURE2D_DESC td{};
        td.Width = td.Height = size;
        td.MipLevels = isBack ? 1u : mips;
        td.ArraySize = 6;
        td.Format = k < 2 ? cfmt : dfmt;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = isBack ? D3D11_BIND_UNORDERED_ACCESS : (D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
        td.MiscFlags = D3D11_RESOURCE_MISC_TEXTURECUBE | (isBack ? 0u : D3D11_RESOURCE_MISC_GENERATE_MIPS);
        h[k] = device->CreateTexture2D(&td, isBack ? (k < 2 ? cinit : dinit) : nullptr, &tex[k]);
    }
    ID3D11UnorderedAccessView* uav[2] = {};
    ID3D11ShaderResourceView* srv[2] = {};
    HRESULT hv[4] = {E_FAIL, E_FAIL, E_FAIL, E_FAIL};
    if (tex[0] && tex[1] && tex[2] && tex[3]) {
        for (int k = 0; k < 2; ++k) {
            D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
            ud.Format = k == 0 ? cfmt : dfmt;
            ud.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2DARRAY;
            ud.Texture2DArray.MipSlice = 0;
            ud.Texture2DArray.FirstArraySlice = 0;
            ud.Texture2DArray.ArraySize = 6;
            hv[k] = device->CreateUnorderedAccessView(tex[k * 2], &ud, &uav[k]);
            D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
            sd.Format = k == 0 ? cfmt : dfmt;
            sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBE;
            sd.TextureCube.MostDetailedMip = 0;
            sd.TextureCube.MipLevels = static_cast<UINT>(-1);
            hv[2 + k] = device->CreateShaderResourceView(tex[k * 2 + 1], &sd, &srv[k]);
        }
    }
    // The origin: 16 bytes, a copy destination for the camera's row of the game's constant buffer.
    const float zero4[4] = {};
    D3D11_BUFFER_DESC ob{};
    ob.ByteWidth = 16;
    ob.Usage = D3D11_USAGE_DEFAULT;
    ob.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA oi{};
    oi.pSysMem = zero4;
    ID3D11Buffer* origin = nullptr;
    const HRESULT ho = device->CreateBuffer(&ob, &oi, &origin);
    ID3D11ShaderResourceView* originSrv = nullptr;
    HRESULT hos = E_FAIL;
    if (origin) {
        D3D11_SHADER_RESOURCE_VIEW_DESC od{};
        od.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        od.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
        od.Buffer.FirstElement = 0;
        od.Buffer.NumElements = 1;
        hos = device->CreateShaderResourceView(origin, &od, &originSrv);
    }
    D3D11_SAMPLER_DESC smd{};
    smd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    smd.AddressU = smd.AddressV = smd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    smd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    smd.MaxLOD = D3D11_FLOAT32_MAX;
    ID3D11SamplerState* smp = nullptr;
    const HRESULT h5 = device->CreateSamplerState(&smd, &smp);
    bool good = smp && origin && originSrv && uav[0] && uav[1] && srv[0] && srv[1];
    for (int k = 0; k < 4; ++k) good = good && tex[k] != nullptr && SUCCEEDED(h[k]);
    if (!good) {
        char why[220];
        std::snprintf(why, sizeof(why), "the cubes could not be made (textures hr %08X %08X %08X %08X, views %08X %08X %08X %08X, origin %08X %08X, sampler %08X)", static_cast<unsigned>(h[0]),
                      static_cast<unsigned>(h[1]), static_cast<unsigned>(h[2]), static_cast<unsigned>(h[3]), static_cast<unsigned>(hv[0]), static_cast<unsigned>(hv[1]),
                      static_cast<unsigned>(hv[2]), static_cast<unsigned>(hv[3]), static_cast<unsigned>(ho), static_cast<unsigned>(hos), static_cast<unsigned>(h5));
        for (auto* p : tex) if (p) p->Release();
        for (auto* p : uav) if (p) p->Release();
        for (auto* p : srv) if (p) p->Release();
        if (origin) origin->Release();
        if (originSrv) originSrv->Release();
        if (smp) smp->Release();
        return bdRefuse(why);
    }
    g_bdBack.store(tex[0], std::memory_order_release);
    g_bdFront.store(tex[1], std::memory_order_release);
    g_bdDistBack.store(tex[2], std::memory_order_release);
    g_bdDistFront.store(tex[3], std::memory_order_release);
    g_bdBackUav.store(uav[0], std::memory_order_release);
    g_bdDistBackUav.store(uav[1], std::memory_order_release);
    g_bdFrontSrv.store(srv[0], std::memory_order_release);
    g_bdDistFrontSrv.store(srv[1], std::memory_order_release);
    g_bdOrigin.store(origin, std::memory_order_release);
    g_bdOriginSrv.store(originSrv, std::memory_order_release);
    g_bdSampler.store(smp, std::memory_order_release);
    if (!makeBackdropShaders(device)) {
        releaseBackdrop();
        return false;
    }
    ID3D11DeviceContext* imm = nullptr;
    device->GetImmediateContext(&imm);
    backdropPublishFront(imm);   // the front mips start as zero too
    if (imm) imm->Release();
    uint64_t texels = static_cast<uint64_t>(size) * size * 6u;
    const uint64_t mb = (texels * (8u + 2u) + texels * (8u + 2u) * 4u / 3u) >> 20;
    Log::get().note("concourse backdrop: cubes made: %u square x 6, colour R16G16B16A16_FLOAT and distance R16_FLOAT, %u mips on the front pair, about %u MB", static_cast<unsigned>(size),
                    static_cast<unsigned>(mips), static_cast<unsigned>(mb));
    return true;
}

// The depth view's texture can be read through a view of the same texture only in a typeless-compatible format.
DXGI_FORMAT bdDepthReadFormat(DXGI_FORMAT f) noexcept {
    switch (f) {
        case DXGI_FORMAT_R32G8X24_TYPELESS: case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
        case DXGI_FORMAT_R24G8_TYPELESS: case DXGI_FORMAT_D24_UNORM_S8_UINT: return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
        case DXGI_FORMAT_R32_TYPELESS: case DXGI_FORMAT_D32_FLOAT: return DXGI_FORMAT_R32_FLOAT;
        case DXGI_FORMAT_R16_TYPELESS: case DXGI_FORMAT_D16_UNORM: return DXGI_FORMAT_R16_UNORM;
        default: return DXGI_FORMAT_UNKNOWN;
    }
}

// The first pane draw of an armed frame, before the game's glass draws (begin() runs before the real draw): the bound render target and the bound depth are copied to
// textures of the same size and format (a typeless colour format resolves through the view's own), which the capture shader reads, and, at the first capture of a visit, the
// camera's row of constant buffer 1 is copied into the origin buffer. Remade when the render target's or depth's size or format changes. On any refusal (each has its own
// line, once per capture) nothing is captured this frame: the capture passes need g_bdSceneOkFrame to be this frame, which is set only when all three copies were taken.
void backdropSceneCopy(ID3D11DeviceContext* ctx) noexcept {
    const uint32_t frame = g_frameNow.load(std::memory_order_relaxed);
    if (g_bdSceneFrame.exchange(frame, std::memory_order_acq_rel) == frame) return;   // once a frame
    const auto refuse = [&](const char* what, const char* why) noexcept {
        if (!g_bdSceneRefusedSaid.exchange(true, std::memory_order_relaxed)) Log::get().note("concourse backdrop: %s refused: %s", what, why);
    };
    ID3D11RenderTargetView* rtv = nullptr;
    ID3D11DepthStencilView* dsv = nullptr;
    ctx->OMGetRenderTargets(1, &rtv, &dsv);
    ID3D11Resource* res = nullptr;
    ID3D11Resource* dres = nullptr;
    D3D11_RENDER_TARGET_VIEW_DESC rd{};
    if (rtv) {
        rtv->GetDesc(&rd);
        rtv->GetResource(&res);
        rtv->Release();
    }
    if (dsv) {
        dsv->GetResource(&dres);
        dsv->Release();
    }
    ID3D11Texture2D* src = nullptr;
    ID3D11Texture2D* dsrc = nullptr;
    if (res) res->QueryInterface(IID_PPV_ARGS(&src));
    if (dres) dres->QueryInterface(IID_PPV_ARGS(&dsrc));
    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    bool ok = false;
    do {
        if (!rtv) { refuse("scene copy", "no render target is bound at the first pane draw"); break; }
        if (!src) { refuse("scene copy", "the render target is not a 2D texture"); break; }
        D3D11_TEXTURE2D_DESC sd{};
        src->GetDesc(&sd);
        const DXGI_FORMAT fmt = rd.Format != DXGI_FORMAT_UNKNOWN ? rd.Format : sd.Format;
        if (formatIsTypeless(fmt) || sd.SampleDesc.Count > 1 || sd.ArraySize != 1 || rd.ViewDimension != D3D11_RTV_DIMENSION_TEXTURE2D || rd.Texture2D.MipSlice != 0) {
            char why[160];
            std::snprintf(why, sizeof(why), "the render target is not a single, single-sample, whole 2D texture of a typed format (format %u, %ux%u, %u samples, array %u)", static_cast<unsigned>(fmt),
                          sd.Width, sd.Height, sd.SampleDesc.Count, sd.ArraySize);
            refuse("scene copy", why);
            break;
        }
        if (!dsrc) { refuse("depth copy", "no depth view is bound at the first pane draw"); break; }
        D3D11_TEXTURE2D_DESC dd{};
        dsrc->GetDesc(&dd);
        const DXGI_FORMAT dread = bdDepthReadFormat(dd.Format);
        if (dread == DXGI_FORMAT_UNKNOWN || dd.SampleDesc.Count > 1 || dd.ArraySize != 1 || dd.MipLevels != 1) {
            char why[160];
            std::snprintf(why, sizeof(why), "the depth texture cannot be read as one (format %u, %ux%u, %u samples, array %u, %u mips)", static_cast<unsigned>(dd.Format), dd.Width, dd.Height,
                          dd.SampleDesc.Count, dd.ArraySize, dd.MipLevels);
            refuse("depth copy", why);
            break;
        }
        if (!dev) { refuse("scene copy", "no device"); break; }
        std::lock_guard<std::mutex> lock(g_bdMutex);
        ID3D11Texture2D* scene = g_bdScene.load(std::memory_order_acquire);
        if (!scene || g_bdSceneW != sd.Width || g_bdSceneH != sd.Height || g_bdSceneFmt != fmt) {
            ID3D11Texture2D* made = nullptr;
            ID3D11ShaderResourceView* madeSrv = nullptr;
            D3D11_TEXTURE2D_DESC td{};
            td.Width = sd.Width;
            td.Height = sd.Height;
            td.MipLevels = 1;
            td.ArraySize = 1;
            td.Format = fmt;
            td.SampleDesc.Count = 1;
            td.Usage = D3D11_USAGE_DEFAULT;
            td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            if (SUCCEEDED(dev->CreateTexture2D(&td, nullptr, &made)) && made) {
                D3D11_SHADER_RESOURCE_VIEW_DESC vd{};
                vd.Format = fmt;
                vd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
                vd.Texture2D.MipLevels = 1;
                if (FAILED(dev->CreateShaderResourceView(made, &vd, &madeSrv)) || !madeSrv) {
                    made->Release();
                    made = nullptr;
                }
            }
            if (!made) { refuse("scene copy", "the scene texture could not be made"); break; }
            ID3D11Texture2D* oldTex = g_bdScene.exchange(made, std::memory_order_acq_rel);
            ID3D11ShaderResourceView* oldSrv = g_bdSceneSrv.exchange(madeSrv, std::memory_order_acq_rel);
            if (oldSrv) oldSrv->Release();
            if (oldTex) oldTex->Release();
            scene = made;
            g_bdSceneW = sd.Width;
            g_bdSceneH = sd.Height;
            g_bdSceneFmt = fmt;
            char fb[24] = {};
            std::snprintf(g_bdSceneFmtText, sizeof(g_bdSceneFmtText), "%s", bdFormatName(fmt, fb, sizeof(fb)));
        }
        ID3D11Texture2D* depth = g_bdDepth.load(std::memory_order_acquire);
        if (!depth || g_bdDepthW != dd.Width || g_bdDepthH != dd.Height || g_bdDepthFmt != dd.Format) {
            ID3D11Texture2D* made = nullptr;
            ID3D11ShaderResourceView* madeSrv = nullptr;
            D3D11_TEXTURE2D_DESC td{};
            td.Width = dd.Width;
            td.Height = dd.Height;
            td.MipLevels = 1;
            td.ArraySize = 1;
            td.Format = dd.Format;
            td.SampleDesc.Count = 1;
            td.Usage = D3D11_USAGE_DEFAULT;
            td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            if (SUCCEEDED(dev->CreateTexture2D(&td, nullptr, &made)) && made) {
                D3D11_SHADER_RESOURCE_VIEW_DESC vd{};
                vd.Format = dread;
                vd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
                vd.Texture2D.MipLevels = 1;
                if (FAILED(dev->CreateShaderResourceView(made, &vd, &madeSrv)) || !madeSrv) {
                    made->Release();
                    made = nullptr;
                }
            }
            if (!made) { refuse("depth copy", "the depth copy texture or its view could not be made"); break; }
            ID3D11Texture2D* oldTex = g_bdDepth.exchange(made, std::memory_order_acq_rel);
            ID3D11ShaderResourceView* oldSrv = g_bdDepthSrv.exchange(madeSrv, std::memory_order_acq_rel);
            if (oldSrv) oldSrv->Release();
            if (oldTex) oldTex->Release();
            depth = made;
            g_bdDepthW = dd.Width;
            g_bdDepthH = dd.Height;
            g_bdDepthFmt = dd.Format;
        }
        // The origin, once per visit: the camera's row of the glass's constant buffer 1, copied on the GPU (no read of the game's memory).
        bool originNow = false;
        if (!g_bdOriginSet.load(std::memory_order_acquire)) {
            ID3D11DeviceContext1* c1 = nullptr;
            ctx->QueryInterface(IID_PPV_ARGS(&c1));
            ID3D11Buffer* cb = nullptr;
            UINT first = 0, num = 0;
            if (c1) c1->PSGetConstantBuffers1(1, 1, &cb, &first, &num);
            char why[200] = {};
            ID3D11Buffer* origin = g_bdOrigin.load(std::memory_order_acquire);
            if (!c1) std::snprintf(why, sizeof(why), "the context has no ID3D11DeviceContext1, so the b1 window cannot be read");
            else if (!cb) std::snprintf(why, sizeof(why), "no constant buffer is bound at b1");
            else if (!origin) std::snprintf(why, sizeof(why), "the origin buffer is missing");
            else {
                D3D11_BUFFER_DESC bd{};
                cb->GetDesc(&bd);
                const UINT byteOff = (first + static_cast<UINT>(kBdCb1Row)) * 16u;
                if ((num != 0 && kBdCb1Row >= num) || byteOff + 16u > bd.ByteWidth)
                    std::snprintf(why, sizeof(why), "constant buffer 1 (%u bytes, window start %u, %u constants) has no row %u", bd.ByteWidth, first, num, static_cast<unsigned>(kBdCb1Row));
                else {
                    D3D11_BOX box{byteOff, 0, 0, byteOff + 16u, 1, 1};
                    FlatComputeInternalScope internal;
                    ctx->CopySubresourceRegion(origin, 0, 0, 0, 0, cb, 0, &box);
                    originNow = true;
                }
            }
            if (cb) cb->Release();
            if (c1) c1->Release();
            if (!originNow) { refuse("origin copy", why); break; }
        }
        {
            FlatComputeInternalScope internal;
            ctx->CopySubresourceRegion(scene, 0, 0, 0, 0, src, 0, nullptr);
            ctx->CopyResource(depth, dsrc);
        }
        if (originNow) {
            g_bdOriginSet.store(true, std::memory_order_release);
            g_bdOriginSetNow.store(true, std::memory_order_relaxed);
        }
        g_bdSceneSource.store(res, std::memory_order_release);   // identity only
        g_bdSceneOkFrame.store(frame, std::memory_order_release);
        g_bdSceneCopied.store(true, std::memory_order_release);
        ok = true;
    } while (false);
    (void)ok;
    if (dev) dev->Release();
    if (src) src->Release();
    if (dsrc) dsrc->Release();
    if (res) res->Release();
    if (dres) dres->Release();
}
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
    const bool cullOn = g_effectsNow.load(std::memory_order_relaxed);   // the outside is not issued: the plain phase, or the backdrop capture's lift
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
        if (followed && g_withholdNow.load(std::memory_order_relaxed) && cc::farLimitedPs(ps)) {   // the plain phase only: the capture's lift draws the room's far glows
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
    // PROTOTYPE (backdrop): in an armed frame the first pane draw copies the render target now, before the game's glass or any pass of ours has drawn into it, and
    // an exterior pane is planned a capture pass (its context's own: a deferred context records it).
    if (g_bdArmed.load(std::memory_order_acquire)) {
        backdropSceneCopy(ctx);
        if (sideOf(cc::PaneKey{indexCount, startIndex, baseVertex}) == cc::PaneSide::kExterior) planCapture_ = true;
    }
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
        if (indexCount == g_clearN.load(std::memory_order_relaxed)) {
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
    planSteady_ = mode == kModeSteady || mode == kModeBackdrop;
    planBackdrop_ = mode == kModeBackdrop;
    if (tint) {
        planGlow_ = 0.0f;
    } else if (planSteady_) {
        planGlow_ = 0.0f;   // the steady mode is one flat pass (beginMirrorSlow), with no glow pass after it
    } else {
        planGlow_ = glowOf(mode) * opacity;
    }
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
        if (testSlot_ >= 0) {
            flags_ |= kFlagTestDrawn;
            if (planBackdrop_ && !g_bdArmed.load(std::memory_order_acquire)) planProbe_ = true;   // never on a capture frame
        }
    }
}

// Called right after the game's own draw. Closes the share query (so the bay's coverage is the game's draw alone) and the test's first query, then, if the plan
// says so and the mark pass could run, binds the mirror's state and returns true. Every false return leaves the context as the game had it.
bool ConcoursePaneScope::nextMirrorPassSlow() noexcept {
    if (capBound_) endCapturePass();   // the capture pass has drawn: its state goes back before anything else is bound
    if (probeBound_) endProbePass();   // so has the probe pass: its query ends and its state goes back (the constant buffer stays, for the display pass)
    if (planCapture_ && !capDone_) {
        capDone_ = true;
        closeGameQueries();   // the capture draw must not count in the game's draw's occlusion queries
        if (beginCapturePass()) return true;
    }
    if (planProbe_ && !probeDone_) {
        probeDone_ = true;
        closeGameQueries();   // the probe must not count in the game's draw's query
        if (beginProbePass()) return true;
    }
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
    ID3D11ShaderResourceView* mine[3] = {g_mirrorSrv[0].load(std::memory_order_acquire), g_mirrorSrv[1].load(std::memory_order_acquire),
                                         g_glowT10.load(std::memory_order_acquire)};
    if (!mine[0] || !mine[1] || !mine[2]) return false;
    calls_->setSrv(ctx_, kPaneSrvFirst, kPaneSrvCount, mine);
    const FLOAT factor[4] = {planGlow_, planGlow_, planGlow_, planGlow_};
    calls_->setBlend(ctx_, bs, factor, 0xFFFFFFFFu);
    return true;
}

// Constant buffer 2 of the pixel stage becomes `own`; the game's is kept for end() (once: a second call only changes the buffer).
void ConcoursePaneScope::bindPsCb(ID3D11Buffer* own) noexcept {
    if ((flags_ & kFlagCb) != 0) {
        if (ctx1_) ctx1_->PSSetConstantBuffers1(kPaneCbSlot, 1, &own, nullptr, nullptr);
        else ctx_->PSSetConstantBuffers(kPaneCbSlot, 1, &own);
        return;
    }
    ctx_->QueryInterface(IID_PPV_ARGS(&ctx1_));
    if (ctx1_) {
        // The *ConstantBuffers1 methods are not hooked, so this passes no thunk, and the saved offset window is put back exactly.
        ctx1_->PSGetConstantBuffers1(kPaneCbSlot, 1, &prevCb_, &prevCbFirst_, &prevCbNum_);
        ctx1_->PSSetConstantBuffers1(kPaneCbSlot, 1, &own, nullptr, nullptr);
    } else {
        ctx_->PSGetConstantBuffers(kPaneCbSlot, 1, &prevCb_);
        ctx_->PSSetConstantBuffers(kPaneCbSlot, 1, &own);
    }
    flags_ |= kFlagCb;
}

void ConcoursePaneScope::closeGameQueries() noexcept {
    if ((flags_ & kFlagQuery) != 0) {
        if (query_ >= 0) closeShareQuery(ctx_, query_);
        flags_ &= ~kFlagQuery;
    }
    if ((flags_ & kFlagTestDrawn) != 0) {
        testCloseDrawn(ctx_, testSlot_);
        flags_ &= ~kFlagTestDrawn;
    }
}

// PROTOTYPE (backdrop). The capture pass: the game's glass geometry again, no colour written, the capture pixel shader, the scene copy at t8, the game's depth test with no
// write, and the cube's back texture at u7. Refused (nothing bound, one line once) when the scene was not copied this frame, the bound render target is not the one copied, the
// render target count does not leave slot 7 free, or there is no depth view. The saved state comes back in endCapturePass (called by the next nextMirrorPass, or end()).
bool ConcoursePaneScope::beginCapturePass() noexcept {
    const auto refuse = [&](const char* why) noexcept {
        if (!g_bdPassRefusedSaid.exchange(true, std::memory_order_relaxed)) Log::get().note("concourse backdrop: a capture pass was refused: %s", why);
        return false;
    };
    ID3D11PixelShader* ps = g_psBdCapture.load(std::memory_order_acquire);
    ID3D11ShaderResourceView* sceneSrv = g_bdSceneSrv.load(std::memory_order_acquire);
    ID3D11ShaderResourceView* depthSrv = g_bdDepthSrv.load(std::memory_order_acquire);
    ID3D11ShaderResourceView* originSrv = g_bdOriginSrv.load(std::memory_order_acquire);
    ID3D11UnorderedAccessView* uavs[kBdUavCount] = {g_bdDistBackUav.load(std::memory_order_acquire), g_bdBackUav.load(std::memory_order_acquire)};   // u6 distance, u7 colour
    ID3D11BlendState* noColour = g_bsMark.load(std::memory_order_acquire);
    if (!ps || !sceneSrv || !depthSrv || !originSrv || !uavs[0] || !uavs[1] || !noColour || !calls_ || !calls_->setBlend || !calls_->setPs || !calls_->setSrv || !calls_->setDepth) return refuse("a shader, view or state is missing");
    if (g_bdSceneOkFrame.load(std::memory_order_acquire) != g_frameNow.load(std::memory_order_relaxed)) return refuse("the scene was not copied in this frame");
    ID3D11RenderTargetView* rtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
    ID3D11DepthStencilView* dsv = nullptr;
    ctx_->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs, &dsv);
    int bound = 0;   // the render target count the runtime sees: the highest bound slot, plus one
    for (int i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i)
        if (rtvs[i]) bound = i + 1;
    bool sameTarget = false;
    if (rtvs[0]) {
        ID3D11Resource* r = nullptr;
        rtvs[0]->GetResource(&r);
        sameTarget = r != nullptr && r == g_bdSceneSource.load(std::memory_order_acquire);
        if (r) r->Release();
    }
    for (auto*& v : rtvs)
        if (v) {
            v->Release();
            v = nullptr;
        }
    const bool haveDsv = dsv != nullptr;
    if (dsv) dsv->Release();
    if (!sameTarget) return refuse("the render target is not the one the scene was copied from");
    if (bound > static_cast<int>(kBdUavFirst)) return refuse("a render target is bound at slot 6 or above, so u6 and u7 are not free");
    if (!haveDsv) return refuse("no depth view is bound");
    UINT ref = 0;
    ctx_->OMGetDepthStencilState(&capPrevDs_, &ref);
    ID3D11DepthStencilState* derived = capPrevDs_ ? derivedDepth(ctx_, capPrevDs_, DsVariant::kProbe) : nullptr;
    if (!derived) {
        if (capPrevDs_) capPrevDs_->Release();
        capPrevDs_ = nullptr;
        return refuse("the depth state could not be derived");
    }
    capPrevRef_ = ref;
    ctx_->OMGetBlendState(&capPrevBs_, capPrevFactor_, &capPrevMask_);
    ctx_->PSGetShader(&capPrevPs_, nullptr, nullptr);
    ctx_->PSGetShaderResources(kBdSceneSlot, kBdSrvCount, capPrevSrv_);
    const FLOAT zeros[4] = {};
    calls_->setBlend(ctx_, noColour, zeros, 0xFFFFFFFFu);
    calls_->setPs(ctx_, ps, nullptr, 0);
    ID3D11ShaderResourceView* mine[kBdSrvCount] = {sceneSrv, depthSrv, originSrv};
    calls_->setSrv(ctx_, kBdSceneSlot, kBdSrvCount, mine);
    calls_->setDepth(ctx_, derived, ref);
    {
        // OMSetRenderTargetsAndUnorderedAccessViews is not hooked by EDVR (nothing in src hooks it): this goes to the context directly.
        FlatComputeInternalScope internal;
        ctx_->OMSetRenderTargetsAndUnorderedAccessViews(D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL, nullptr, nullptr, kBdUavFirst, kBdUavCount, uavs, nullptr);
    }
    capBound_ = true;
    flags_ |= kFlagCapPass;
    g_bdPaneDraws.fetch_add(1, std::memory_order_relaxed);
    return true;
}

// PROTOTYPE (automatic capture). The probe pass: the game's glass geometry again, no colour, the probe pixel shader (the display shader's direction and look-up, `clip` where
// the composite covers at least a half), the cube bound as the display pass binds it, the game's depth test with the display's mark stencil and no depth write, and its own
// occlusion query, whose count is the pixels the cube cannot show. Only on a test frame, only with a cube, never on a capture frame.
bool ConcoursePaneScope::beginProbePass() noexcept {
    if (testSlot_ < 0 || !planMark_ || !calls_ || !calls_->setBlend || !calls_->setPs || !calls_->setSrv || !calls_->setDepth) return false;
    if (g_bdArmed.load(std::memory_order_acquire) || !g_bdCubeValid.load(std::memory_order_acquire)) return false;
    ID3D11Query* q = g_tests[testSlot_].qProbe;
    ID3D11PixelShader* ps = g_psBdProbe.load(std::memory_order_acquire);
    ID3D11ShaderResourceView* mine[kBdSrvCount] = {g_bdFrontSrv.load(std::memory_order_acquire), g_bdDistFrontSrv.load(std::memory_order_acquire), g_bdOriginSrv.load(std::memory_order_acquire)};
    ID3D11SamplerState* smp = g_bdSampler.load(std::memory_order_acquire);
    ID3D11BlendState* noColour = g_bsMark.load(std::memory_order_acquire);
    ID3D11Buffer* cb = (keyN_ == cc::kBayIndexCount ? g_cbGlowBay : g_cbGlow).load(std::memory_order_acquire);
    if (!q || !ps || !mine[0] || !mine[1] || !mine[2] || !smp || !noColour || !cb) return false;
    if (!ensureMarked(ctx_, *calls_)) return false;   // runs and restores its own state
    UINT ref = 0;
    ctx_->OMGetDepthStencilState(&capPrevDs_, &ref);
    ID3D11DepthStencilState* derived = capPrevDs_ ? derivedDepth(ctx_, capPrevDs_, DsVariant::kMarkedProbe) : nullptr;
    if (!derived) {
        if (capPrevDs_) capPrevDs_->Release();
        capPrevDs_ = nullptr;
        return false;
    }
    capPrevRef_ = ref;
    ctx_->OMGetBlendState(&capPrevBs_, capPrevFactor_, &capPrevMask_);
    ctx_->PSGetShader(&capPrevPs_, nullptr, nullptr);
    ctx_->PSGetShaderResources(kBdSceneSlot, kBdSrvCount, capPrevSrv_);
    ctx_->PSGetSamplers(kBdSamplerSlot, 1, &probePrevSampler_);
    const FLOAT zeros[4] = {};
    calls_->setBlend(ctx_, noColour, zeros, 0xFFFFFFFFu);
    calls_->setPs(ctx_, ps, nullptr, 0);
    calls_->setSrv(ctx_, kBdSceneSlot, kBdSrvCount, mine);
    ctx_->PSSetSamplers(kBdSamplerSlot, 1, &smp);
    calls_->setDepth(ctx_, derived, kMarkRef);
    bindPsCb(cb);   // the game's constant buffer 2 is saved once (kFlagCb); the display pass changes the buffer again and end() puts the game's back
    {
        FlatComputeInternalScope internal;
        ctx_->Begin(q);
    }
    {
        std::lock_guard<std::mutex> lock(g_sideMutex);
        g_tests[testSlot_].probe = 1;
    }
    probeBound_ = true;
    flags_ |= kFlagProbePass;
    g_probeIssued.fetch_add(1, std::memory_order_relaxed);
    return true;
}

void ConcoursePaneScope::endProbePass() noexcept {
    if (!probeBound_) return;
    probeBound_ = false;
    flags_ &= ~kFlagProbePass;
    if (testSlot_ >= 0) {
        {
            FlatComputeInternalScope internal;
            ctx_->End(g_tests[testSlot_].qProbe);
        }
        std::lock_guard<std::mutex> lock(g_sideMutex);
        g_tests[testSlot_].probe = 2;
    }
    ctx_->PSSetSamplers(kBdSamplerSlot, 1, &probePrevSampler_);
    if (probePrevSampler_) probePrevSampler_->Release();
    probePrevSampler_ = nullptr;
    calls_->setDepth(ctx_, capPrevDs_, capPrevRef_);
    if (capPrevDs_) capPrevDs_->Release();
    capPrevDs_ = nullptr;
    calls_->setSrv(ctx_, kBdSceneSlot, kBdSrvCount, capPrevSrv_);
    for (auto*& v : capPrevSrv_) {
        if (v) v->Release();
        v = nullptr;
    }
    calls_->setPs(ctx_, capPrevPs_, nullptr, 0);
    if (capPrevPs_) capPrevPs_->Release();
    capPrevPs_ = nullptr;
    calls_->setBlend(ctx_, capPrevBs_, capPrevFactor_, capPrevMask_);
    if (capPrevBs_) capPrevBs_->Release();
    capPrevBs_ = nullptr;
}

void ConcoursePaneScope::endCapturePass() noexcept {
    if (!capBound_) return;
    capBound_ = false;
    flags_ &= ~kFlagCapPass;
    {
        FlatComputeInternalScope internal;
        ID3D11UnorderedAccessView* none[kBdUavCount] = {};
        ctx_->OMSetRenderTargetsAndUnorderedAccessViews(D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL, nullptr, nullptr, kBdUavFirst, kBdUavCount, none, nullptr);
    }
    calls_->setDepth(ctx_, capPrevDs_, capPrevRef_);
    if (capPrevDs_) capPrevDs_->Release();
    capPrevDs_ = nullptr;
    calls_->setSrv(ctx_, kBdSceneSlot, kBdSrvCount, capPrevSrv_);
    for (auto*& v : capPrevSrv_) {
        if (v) v->Release();
        v = nullptr;
    }
    calls_->setPs(ctx_, capPrevPs_, nullptr, 0);
    if (capPrevPs_) capPrevPs_->Release();
    capPrevPs_ = nullptr;
    calls_->setBlend(ctx_, capPrevBs_, capPrevFactor_, capPrevMask_);
    if (capPrevBs_) capPrevBs_->Release();
    capPrevBs_ = nullptr;
}

bool ConcoursePaneScope::beginMirrorSlow() noexcept {
    closeGameQueries();
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
    // The steady mode is one flat pass with EDVR's pixel shader and no textures. If that shader or the pane's buffer is missing, the pane is the plain mirror
    // instead (the textures), said once: a pane is never left transparent with the outside culled.
    ID3D11PixelShader* skyPs = nullptr;
    ID3D11Buffer* steadyCb = nullptr;
    ID3D11ShaderResourceView* cubeSrvs[kBdSrvCount] = {};   // mirror-backdrop: the colour cube, the distance cube and the origin, with the sampler (null: not this pass)
    ID3D11SamplerState* cubeSmp = nullptr;
    if (planSteady_ && !planTint_) {
        skyPs = g_psSky.load(std::memory_order_acquire);
        steadyCb = (keyN_ == cc::kBayIndexCount ? g_cbGlowBay : g_cbGlow).load(std::memory_order_acquire);
        if (planBackdrop_) {
            ID3D11PixelShader* show = g_psBdShow.load(std::memory_order_acquire);
            ID3D11ShaderResourceView* srv = g_bdFrontSrv.load(std::memory_order_acquire);
            ID3D11ShaderResourceView* dsrv = g_bdDistFrontSrv.load(std::memory_order_acquire);
            ID3D11ShaderResourceView* osrv = g_bdOriginSrv.load(std::memory_order_acquire);
            ID3D11SamplerState* smp = g_bdSampler.load(std::memory_order_acquire);
            if (show && srv && dsrv && osrv && smp) {
                skyPs = show;
                cubeSrvs[0] = srv;
                cubeSrvs[1] = dsrv;
                cubeSrvs[2] = osrv;
                cubeSmp = smp;
            } else if (!g_bdFallbackSaid.exchange(true, std::memory_order_relaxed)) {
                Log::get().note("concourse backdrop: mirror-backdrop has no %s yet (the cube is made by the first capture, F6); the panes are drawn as mirror-steady",
                                !show ? "display shader" : !srv || !dsrv || !osrv ? "cube" : "sampler");
            }
        }
        if (!skyPs || !steadyCb) {
            skyPs = nullptr;
            steadyCb = nullptr;
            if (!g_steadyFallbackSaid.exchange(true, std::memory_order_relaxed))
                Log::get().note("concourse pane: mirror-steady has no pixel shader or buffer (see the line before); the panes are drawn as the plain mirror");
        }
    }
    const bool steadyNow = skyPs != nullptr;
    ID3D11ShaderResourceView* mine[3] = {};
    if (!planTint_ && !steadyNow) {
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
    } else if (steadyNow) {
        ctx_->PSGetShader(&prevPs_, nullptr, nullptr);
        calls_->setPs(ctx_, skyPs, nullptr, 0);
        flags_ |= kFlagPs;   // put back by end(), with the buffer below (kFlagCb); no textures are swapped (the backdrop: t8 and s7 below)
        if (cubeSrvs[0]) {
            ctx_->PSGetShaderResources(kPaneSrvFirst, kPaneSrvCount, prevSrv_);
            calls_->setSrv(ctx_, kBdSceneSlot, kBdSrvCount, cubeSrvs);
            flags_ |= kFlagSrv;
            ctx_->PSGetSamplers(kBdSamplerSlot, 1, &prevSampler_);
            ctx_->PSSetSamplers(kBdSamplerSlot, 1, &cubeSmp);
            flags_ |= kFlagSampler;
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
    if (steadyNow) bindPsCb(steadyCb);
    else if (ownCb) bindPsCb(ownCb);
    if (testSlot_ >= 0) {
        testOpenMirror(ctx_, testSlot_);
        flags_ |= kFlagTestMirror;
    }
    g_treated.fetch_add(1, std::memory_order_relaxed);
    return true;
}

void ConcoursePaneScope::end() noexcept {
    if ((flags_ & kFlagProbePass) != 0) endProbePass();   // before the test slot is touched below: it closes the probe's query
    if ((flags_ & kFlagCapPass) != 0) endCapturePass();   // a capture pass the site never asked past (the loop always does; this is the way out of any other exit)
    if ((flags_ & kFlagSampler) != 0) {
        ctx_->PSSetSamplers(kBdSamplerSlot, 1, &prevSampler_);
        if (prevSampler_) prevSampler_->Release();
        prevSampler_ = nullptr;
    }
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

// The toast names the mode in plain words (F4 and F6 only; the ini's own setting at start-up says nothing).
void modeToast(uint32_t m) noexcept {
    switch (m) {
        case kModeBackdrop: queueToast(g_bdCubeValid.load(std::memory_order_relaxed) ? "Windows: captured view" : "Windows: captured view, none yet"); break;
        case kModeSteady: queueToast("Windows: flat light"); break;
        case kModeLit: case kModeBright: case kModeBrightest: queueToast("Windows: bright mirror"); break;
        case kModeMirror: case kModeMirrorSoft: queueToast("Windows: mirror"); break;
        case kModeTint: queueToast("Windows: tint"); break;
        default: queueToast("Windows: plain glass"); break;
    }
}

void setMode(uint32_t m, const char* why, bool toast = false) noexcept {
    const uint32_t before = g_mode.exchange(m, std::memory_order_relaxed);
    if (before != m) Log::get().note("concourse pane: mode %s (%s)", modeText(m), why);
    if (toast && m == kModeBackdrop) g_bdAutoOn.store(true, std::memory_order_relaxed);   // a hotkey chose the backdrop: automatic capture is on (the ini's own setting does not turn it on)
    if (toast && before != m) modeToast(m);
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
    bindKey(g_keyParallax, g_keyParallaxText, Config::get().getString("hotkey.concourse_backdrop_parallax", ""), "hotkey.concourse_backdrop_parallax", "backdrop distance key");
    g_keysBound = true;
    const std::string modeT = Config::get().getString("diag.concourse_pane_mode", "mirror-steady");
    if (!g_modeTextSeen || modeT != g_modeAppliedText) {
        g_modeTextSeen = true;
        g_modeAppliedText = modeT;
        uint32_t m = kModeSteady;
        if (parseMode(modeT, &m)) setMode(m, "diag.concourse_pane_mode");
        else Log::get().note("concourse pane: diag.concourse_pane_mode = \"%s\" is not mirror-backdrop, mirror-steady, mirror-brightest, mirror-lit, mirror-bright, mirror-soft, mirror, tint or off; the mode is unchanged", modeT.c_str());
    }
    const std::string glowT = Config::get().getString("diag.concourse_glow", "");
    if (!g_glowTextSeen || glowT != g_glowAppliedText) {
        g_glowTextSeen = true;
        g_glowAppliedText = glowT;
        float lv[4] = {};
        if (glowT.empty()) {
            g_glowSky.store(cc::kGlowSkyDefault, std::memory_order_relaxed);   // unset: the defaults
            g_glowAngled.store(cc::kGlowAngledDefault, std::memory_order_relaxed);
            g_glowOther.store(cc::kGlowOtherDefault, std::memory_order_relaxed);
            g_glowBay.store(cc::kGlowBayDefault, std::memory_order_relaxed);
        } else if (cc::parseGlowLevels(glowT.c_str(), lv)) {
            g_glowSky.store(lv[0], std::memory_order_relaxed);
            g_glowAngled.store(lv[1], std::memory_order_relaxed);
            g_glowOther.store(lv[2], std::memory_order_relaxed);
            g_glowBay.store(lv[3], std::memory_order_relaxed);
        } else {
            Log::get().note("concourse pane: diag.concourse_glow = \"%s\" is not four numbers from 0 to %.0f (sky,angled,other,bay); the levels stay sky %g angled %g other %g bay %g",
                            glowT.c_str(), static_cast<double>(cc::kGlowMax), static_cast<double>(g_glowSky.load(std::memory_order_relaxed)),
                            static_cast<double>(g_glowAngled.load(std::memory_order_relaxed)), static_cast<double>(g_glowOther.load(std::memory_order_relaxed)),
                            static_cast<double>(g_glowBay.load(std::memory_order_relaxed)));
        }
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
    const std::string radiusT = Config::get().getString("diag.concourse_backdrop_radius", "");
    if (!g_radiusTextSeen || radiusT != g_radiusAppliedText) {
        const bool first = !g_radiusTextSeen;
        g_radiusTextSeen = true;
        g_radiusAppliedText = radiusT;
        float want = cc::kBackdropRadiusDefaultM;
        bool bad = false;
        if (!radiusT.empty()) {
            char* end = nullptr;
            const double v = std::strtod(radiusT.c_str(), &end);
            while (end && (*end == ' ' || *end == '\t')) ++end;
            if (end && end != radiusT.c_str() && *end == '\0' && v >= static_cast<double>(cc::kBackdropRadiusMinM) && v <= static_cast<double>(cc::kBackdropRadiusMaxM)) want = static_cast<float>(v);
            else bad = true;
        }
        if (bad) {
            Log::get().note("concourse backdrop: diag.concourse_backdrop_radius = \"%s\" is not a number from %.0f to %.0f; the radius stays %.0f m", radiusT.c_str(),
                            static_cast<double>(cc::kBackdropRadiusMinM), static_cast<double>(cc::kBackdropRadiusMaxM), static_cast<double>(g_bdRadius.load(std::memory_order_relaxed)));
        } else {
            g_bdRadius.store(want, std::memory_order_relaxed);
            if (!first || want != cc::kBackdropRadiusDefaultM)
                Log::get().note("concourse backdrop: distance mode %s, radius %.0f m", cc::parallaxName(g_bdParallax.load(std::memory_order_relaxed)), static_cast<double>(want));
        }
    }
    const std::string glassT = Config::get().getString("diag.concourse_glass", "");
    if (!g_glassTextSeen || glassT != g_glassAppliedText) {
        const bool first = !g_glassTextSeen;
        g_glassTextSeen = true;
        g_glassAppliedText = glassT;
        float v[6] = {};
        if (glassT.empty()) {
            for (int k = 0; k < 6; ++k) g_glass[k].store(cc::kGlassDefault[k], std::memory_order_relaxed);   // unset: the defaults
            if (!first) Log::get().note("concourse backdrop: glass back to the defaults: tint %g %g %g haze %g blur %g fresnel %g", static_cast<double>(cc::kGlassDefault[0]),
                                        static_cast<double>(cc::kGlassDefault[1]), static_cast<double>(cc::kGlassDefault[2]), static_cast<double>(cc::kGlassDefault[3]),
                                        static_cast<double>(cc::kGlassDefault[4]), static_cast<double>(cc::kGlassDefault[5]));
        } else if (cc::parseGlass(glassT.c_str(), v)) {
            for (int k = 0; k < 6; ++k) g_glass[k].store(v[k], std::memory_order_relaxed);
            Log::get().note("concourse backdrop: glass tint %g %g %g haze %g blur %g fresnel %g", static_cast<double>(v[0]), static_cast<double>(v[1]), static_cast<double>(v[2]),
                            static_cast<double>(v[3]), static_cast<double>(v[4]), static_cast<double>(v[5]));
        } else {
            Log::get().note("concourse backdrop: diag.concourse_glass = \"%s\" is not six numbers (r,g,b 0 to 2, haze 0 to 1, blur 0 to 8, fresnel 0 to 2); the glass stays tint %g %g %g haze %g blur %g fresnel %g",
                            glassT.c_str(), static_cast<double>(g_glass[0].load()), static_cast<double>(g_glass[1].load()), static_cast<double>(g_glass[2].load()),
                            static_cast<double>(g_glass[3].load()), static_cast<double>(g_glass[4].load()), static_cast<double>(g_glass[5].load()));
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
    const bool effects = withhold || g_feedEffects.load(std::memory_order_relaxed);   // the outside is not issued: the plain phase, or the backdrop capture's lift
    const bool shares = g_feedMeasure.load(std::memory_order_relaxed) && g_shareOn.load(std::memory_order_relaxed);
    g_treatNow.store(treat, std::memory_order_release);
    const bool sealed = g_feedSealed.load(std::memory_order_relaxed);
    g_withholdNow.store(withhold, std::memory_order_release);
    g_effectsNow.store(effects, std::memory_order_release);
    g_withholdSealedNow.store(sealed, std::memory_order_release);
    detail::g_concoursePaneArmed.store(treat || effects || sealed || shares || g_capturing.load(std::memory_order_relaxed) || g_bdArmed.load(std::memory_order_relaxed), std::memory_order_release);
}

}  // namespace

uint64_t concoursePaneTakeTreated() noexcept { return g_treated.exchange(0, std::memory_order_relaxed); }
bool concoursePaneTakeSurveyRequest() noexcept { return g_surveyRequested.exchange(false, std::memory_order_relaxed); }
namespace {
// The glow constant buffer follows the levels and the anchor's up axis: written at the tick, only when a value changed. The one-shot line says the levels at first use
// (the steady mode treating panes) and again when they change.
void updateGlowCb(ID3D11Device* device) noexcept {
    ID3D11Buffer* cb = g_cbGlow.load(std::memory_order_acquire);
    ID3D11Buffer* cbBay = g_cbGlowBay.load(std::memory_order_acquire);
    if (!cb || !cbBay) return;
    const float sky = g_glowSky.load(std::memory_order_relaxed), angled = g_glowAngled.load(std::memory_order_relaxed), other = g_glowOther.load(std::memory_order_relaxed),
                bay = g_glowBay.load(std::memory_order_relaxed);
    float data[24] = {};
    const bool upOk = g_upFedValid.load(std::memory_order_relaxed);
    for (int k = 0; k < 3; ++k) data[k] = upOk ? g_upFed[k].load(std::memory_order_relaxed) : 0.0f;
    data[4] = sky;
    data[5] = angled;
    data[6] = other;
    for (int k = 0; k < 3; ++k) data[8 + k] = kSteadyColour[k];
    float dataBay[24] = {};
    dataBay[4] = dataBay[5] = dataBay[6] = bay;
    for (int k = 0; k < 3; ++k) dataBay[8 + k] = kSteadyColour[k];
    // The backdrop's row 3: the distance mode (cc::kParallax*) and the sphere's radius. Both buffers carry it (the bay window is backdrop too).
    data[12] = dataBay[12] = static_cast<float>(g_bdParallax.load(std::memory_order_relaxed));
    data[13] = dataBay[13] = g_bdRadius.load(std::memory_order_relaxed);
    // Rows 4 and 5: the glass look (r, g, b, haze; blur, fresnel).
    for (int k = 0; k < 6; ++k) data[16 + k] = dataBay[16 + k] = g_glass[k].load(std::memory_order_relaxed);
    const bool changed = !g_glowCbSentValid || std::memcmp(data, g_glowCbSent, sizeof(data)) != 0;
    const bool changedBay = !g_glowCbSentValid || std::memcmp(dataBay, g_glowCbBaySent, sizeof(dataBay)) != 0;
    if (changed || changedBay) {
        ID3D11DeviceContext* imm = nullptr;
        device->GetImmediateContext(&imm);
        if (imm) {
            FlatComputeInternalScope internal;
            if (changed) imm->UpdateSubresource(cb, 0, nullptr, data, 0, 0);
            if (changedBay) imm->UpdateSubresource(cbBay, 0, nullptr, dataBay, 0, 0);
            imm->Release();
            std::memcpy(g_glowCbSent, data, sizeof(data));
            std::memcpy(g_glowCbBaySent, dataBay, sizeof(dataBay));
            g_glowCbSentValid = true;
        }
    }
    if ((g_mode.load(std::memory_order_relaxed) == kModeSteady || g_mode.load(std::memory_order_relaxed) == kModeBackdrop) && g_treatNow.load(std::memory_order_relaxed) &&
        (!g_glowLogged || g_glowLoggedLv[0] != sky || g_glowLoggedLv[1] != angled || g_glowLoggedLv[2] != other || g_glowLoggedLv[3] != bay)) {
        g_glowLogged = true;
        g_glowLoggedLv[0] = sky;
        g_glowLoggedLv[1] = angled;
        g_glowLoggedLv[2] = other;
        g_glowLoggedLv[3] = bay;
        Log::get().note("concourse pane: glow levels sky %g angled %g other %g bay %g, up %.3f %.3f %.3f%s", static_cast<double>(sky), static_cast<double>(angled),
                        static_cast<double>(other), static_cast<double>(bay), static_cast<double>(data[0]), static_cast<double>(data[1]), static_cast<double>(data[2]),
                        upOk ? "" : " (not read: every window takes the other level)");
    }
}

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
uint32_t concoursePaneClearingN() noexcept { return g_clearN.load(std::memory_order_relaxed); }
const char* concoursePaneModeName() noexcept { return modeText(g_mode.load(std::memory_order_relaxed)); }
uint32_t concoursePaneSlotCount() noexcept { return g_listN.load(std::memory_order_relaxed); }
bool concoursePaneTreating() noexcept { return g_treatNow.load(std::memory_order_relaxed); }

void concoursePaneSetFeed(const ConcoursePaneFeed& feed) noexcept {
    g_feedMeasure.store(feed.measure, std::memory_order_relaxed);
    const bool treatBefore = g_feedTreat.exchange(feed.treat, std::memory_order_relaxed);
    if (!treatBefore && feed.treat) {   // a new visit to a concourse: what was learned about the panes is forgotten
        std::lock_guard<std::mutex> lock(g_sideMutex);
        g_sides.reset();
        g_bdResetPending.store(true, std::memory_order_release);   // and the backdrop cube and its origin (the tick does it)
    }
    if (!g_feedWithhold.exchange(feed.withhold, std::memory_order_relaxed) && feed.withhold) {
        g_plainFrom.store(g_frameNow.load(std::memory_order_relaxed), std::memory_order_relaxed);
        g_clearN.store(cc::kBayIndexCount, std::memory_order_relaxed);   // back in the plain mirror phase: the next approach starts from the bay window
    }
    g_feedEffects.store(feed.effects, std::memory_order_relaxed);
    g_feedSealed.store(feed.sealed, std::memory_order_relaxed);
    g_bayOpacity.store(feed.bayOpacity, std::memory_order_relaxed);
    g_enterShareFed.store(feed.enterShare, std::memory_order_relaxed);
    for (int k = 0; k < 3; ++k) g_upFed[k].store(feed.upValid ? feed.up[k] : 0.0f, std::memory_order_relaxed);
    g_upFedValid.store(feed.upValid, std::memory_order_relaxed);
    updateArmed();
}

ConcoursePaneShares concoursePaneShares() noexcept {
    std::lock_guard<std::mutex> lock(g_shareMutex);
    ConcoursePaneShares r = g_latest;
    if (r.valid && GetTickCount64() - g_latestMs > 250) r = ConcoursePaneShares{};   // no family draw on screen makes no reading: the bay is out of view, soon
    return r;
}

namespace {
// The state machine, once a tick (the cull's tick runs first in the same Present: it arms, and reads the result a tick later). Idle -> (arm requested) -> Armed, whose
// pane draws, in the frames after this tick, copy the scene and capture; at the tick after a frame in which at least one pane was captured, the back texture is copied to
// the front one and the phase is Done. With no pane captured for kBackdropWaitFrames frames it is Empty. A resource refusal is Refused (its line is logged by the maker).
void backdropTick(ID3D11Device* device, uint32_t frame) noexcept {
    const auto phase = static_cast<ConcourseBackdropPhase>(g_bdPhase.load(std::memory_order_relaxed));
    // A new visit (treat rose): the cube is emptied and the origin forgotten, unless a capture is open (then at the next tick without one).
    if (g_bdResetPending.load(std::memory_order_acquire) && !g_bdArmed.load(std::memory_order_acquire) && !g_bdArmRequested.load(std::memory_order_acquire) &&
        phase != ConcourseBackdropPhase::kArmed) {
        g_bdResetPending.store(false, std::memory_order_release);
        backdropReset(device);
    }
    if (phase == ConcourseBackdropPhase::kIdle && g_bdArmRequested.exchange(false, std::memory_order_acq_rel)) {
        if (!device) {
            bdRefuse("no device");
            g_bdPhase.store(static_cast<uint8_t>(ConcourseBackdropPhase::kRefused), std::memory_order_relaxed);
            if (g_bdToast.load(std::memory_order_relaxed)) queueToast("Windows: capture refused");
        } else if (!makeBackdropResources(device) || !g_statesReady.load(std::memory_order_acquire)) {
            if (g_statesReady.load(std::memory_order_acquire) == false && g_bdBack.load() != nullptr) bdRefuse("the pane states are not made (the cull is not treating panes)");
            g_bdPhase.store(static_cast<uint8_t>(ConcourseBackdropPhase::kRefused), std::memory_order_relaxed);
            if (g_bdToast.load(std::memory_order_relaxed)) queueToast("Windows: capture refused");
        } else {
            g_bdPaneDraws.store(0, std::memory_order_relaxed);
            g_bdSceneCopied.store(false, std::memory_order_relaxed);
            g_bdOriginSetNow.store(false, std::memory_order_relaxed);
            g_bdSceneFrame.store(0xFFFFFFFFu, std::memory_order_relaxed);
            g_bdSceneOkFrame.store(0xFFFFFFFFu, std::memory_order_relaxed);
            g_bdSceneRefusedSaid.store(false, std::memory_order_relaxed);
            g_bdPassRefusedSaid.store(false, std::memory_order_relaxed);
            g_bdFramesWaited = 0;
            g_bdArmFrame = frame;
            g_bdPhase.store(static_cast<uint8_t>(ConcourseBackdropPhase::kArmed), std::memory_order_relaxed);
            g_bdArmed.store(true, std::memory_order_release);   // the draws after this tick are the capture frame
        }
        return;
    }
    if (phase != ConcourseBackdropPhase::kArmed || frame == g_bdArmFrame) return;
    g_bdFramesWaited = frame - g_bdArmFrame;
    if (g_bdPaneDraws.load(std::memory_order_relaxed) > 0) {
        g_bdArmed.store(false, std::memory_order_release);
        ID3D11DeviceContext* imm = nullptr;
        if (device) device->GetImmediateContext(&imm);
        if (g_bdBack.load(std::memory_order_acquire) && g_bdFront.load(std::memory_order_acquire) && imm) {
            backdropPublishFront(imm);   // back to front, mip 0 slice by slice, then the mips of both cubes
            g_bdCaptures.fetch_add(1, std::memory_order_relaxed);
            g_bdCubeValid.store(true, std::memory_order_release);
            g_bdPhase.store(static_cast<uint8_t>(ConcourseBackdropPhase::kDone), std::memory_order_relaxed);
            if (g_bdToast.load(std::memory_order_relaxed)) queueToast("Windows: view captured");
        } else {
            bdRefuse("the cube could not be copied (no immediate context or texture)");
            g_bdPhase.store(static_cast<uint8_t>(ConcourseBackdropPhase::kRefused), std::memory_order_relaxed);
            if (g_bdToast.load(std::memory_order_relaxed)) queueToast("Windows: capture refused");
        }
        if (imm) imm->Release();
    } else if (g_bdFramesWaited >= cc::kBackdropWaitFrames) {
        g_bdArmed.store(false, std::memory_order_release);
        g_bdPhase.store(static_cast<uint8_t>(ConcourseBackdropPhase::kEmpty), std::memory_order_relaxed);
        if (g_bdToast.load(std::memory_order_relaxed)) queueToast("Windows: nothing captured");
    }
}
}  // namespace

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
    if (device) updateGlowCb(device);   // after the device check: the buffer belongs to g_device
    const uint32_t prevFrame = g_frameNow.exchange(frame, std::memory_order_relaxed);
    pollConfig(nowMs);
    const JournalRawStatus raw = journalRawStatus();
    const bool known = journalOnFootKnown() || (runtimeFlatProfile() && raw.gameplay);
    g_social = cc::concourseSocialOnFoot(journalStatusFlags2(), known, raw.gameplay);
    if (g_keyMode && g_keyMode->pressed()) {
        const uint32_t cur = g_mode.load(std::memory_order_relaxed);
        // mirror-lit, mirror-bright and mirror-soft are set by the ini only: the press steps mirror-backdrop, mirror-steady, mirror-brightest, mirror, tint, off.
        const uint32_t next = cur == kModeBackdrop ? kModeSteady : cur == kModeSteady ? kModeBrightest : cur == kModeBrightest ? kModeMirror : cur == kModeMirror ? kModeTint
                              : cur == kModeTint ? kModeOff : kModeBackdrop;
        setMode(next, g_social ? "hotkey.concourse_pane_mode" : "hotkey.concourse_pane_mode; it applies on foot in social space", true);
    }
    if (g_keyParallax && g_keyParallax->pressed()) {
        const uint32_t next = cc::parallaxNext(g_bdParallax.load(std::memory_order_relaxed));
        g_bdParallax.store(next, std::memory_order_relaxed);
        Log::get().note("concourse backdrop: distance mode %s, radius %.0f m (hotkey.concourse_backdrop_parallax)", cc::parallaxName(next), static_cast<double>(g_bdRadius.load(std::memory_order_relaxed)));
        queueToast(next == cc::kParallaxNone ? "Windows: distance off" : next == cc::kParallaxSphere ? "Windows: distance by sphere" : "Windows: distance by depth");
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
        {   // the missing share over the last kMissWindowFrames frames (the tick's thread owns the entries)
            double sum = 0.0;
            if (g_mode.load(std::memory_order_relaxed) == kModeBackdrop && g_missPixels > 0.0)
                for (const MissEntry& e : g_miss)
                    if (e.missing > 0.0 && frame - e.frame < kMissWindowFrames) sum += e.missing;
            g_missShare.store(g_missPixels > 0.0 ? static_cast<float>(sum / g_missPixels) : 0.0f, std::memory_order_relaxed);
        }
        const bool measuring = g_feedMeasure.load(std::memory_order_relaxed);
        if (measuring && makeSlots(device)) g_shareOn.store(true, std::memory_order_release);
        if (!measuring) g_shareOn.store(false, std::memory_order_release);
        pollShares(device, prevFrame, frame, measuring, nowMs);
        readMaterials(device, frame);
    }
    backdropTick(device, frame);
    updateArmed();
}

// ---- The backdrop capture's tick and its API (PROTOTYPE) -------------------------------------------------------------------------------------------------
void concoursePaneSetBackdropMode(const char* why) noexcept { setMode(kModeBackdrop, why, true); }

void concoursePaneArmCapture(bool toast) noexcept {
    g_bdToast.store(toast, std::memory_order_relaxed);
    g_bdPhase.store(static_cast<uint8_t>(ConcourseBackdropPhase::kIdle), std::memory_order_relaxed);
    g_bdArmRequested.store(true, std::memory_order_release);
}

ConcourseBackdropStatus concoursePaneBackdropStatus() noexcept {
    ConcourseBackdropStatus s;
    s.phase = static_cast<ConcourseBackdropPhase>(g_bdPhase.load(std::memory_order_relaxed));
    s.sceneCopied = g_bdSceneCopied.load(std::memory_order_relaxed);
    s.paneDraws = g_bdPaneDraws.load(std::memory_order_relaxed);
    s.originSet = g_bdOriginSetNow.load(std::memory_order_relaxed);
    s.captures = g_bdCaptures.load(std::memory_order_relaxed);
    s.framesWaited = g_bdFramesWaited;
    std::lock_guard<std::mutex> lock(g_bdMutex);
    s.sceneW = g_bdSceneW;
    s.sceneH = g_bdSceneH;
    std::snprintf(s.sceneFormat, sizeof(s.sceneFormat), "%s", g_bdSceneFmtText);
    std::snprintf(s.refusal, sizeof(s.refusal), "%s", g_bdRefusal);
    return s;
}

void concoursePaneBackdropEnd() noexcept {
    g_bdArmRequested.store(false, std::memory_order_relaxed);
    g_bdArmed.store(false, std::memory_order_release);
    g_bdPhase.store(static_cast<uint8_t>(ConcourseBackdropPhase::kIdle), std::memory_order_relaxed);
    updateArmed();
}

bool concoursePaneBackdropAuto() noexcept { return g_bdAutoOn.load(std::memory_order_relaxed) && g_mode.load(std::memory_order_relaxed) == kModeBackdrop; }
bool concoursePaneIsBackdropMode() noexcept { return g_mode.load(std::memory_order_relaxed) == kModeBackdrop; }
ConcourseBackdropMissing concoursePaneBackdropMissing() noexcept {
    ConcourseBackdropMissing m;
    m.share = g_missShare.load(std::memory_order_relaxed);
    m.probesIssued = g_probeIssued.load(std::memory_order_relaxed);
    m.probesRead = g_probeRead.load(std::memory_order_relaxed);
    m.cubeValid = g_bdCubeValid.load(std::memory_order_relaxed);
    return m;
}

void concoursePaneQueueToast(const char* text) noexcept {
    if (text) queueToast(text);
}

bool concoursePaneTakeToast(char* out, size_t size) noexcept {
    if (!out || size == 0) return false;
    std::lock_guard<std::mutex> lock(g_toastMutex);
    if (!g_toastDue) return false;
    g_toastDue = false;
    std::snprintf(out, size, "%s", g_toastText);
    return true;
}

}  // namespace edvr
