// The concourse's glass panes: see concourse_pane.h. TEMPORARY (docs\concourse-cull-2026-11-05.md): removed when the arc closes.
#include "concourse_pane.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

#include "../common/config.h"
#include "../common/hotkey.h"
#include "../common/log.h"
#include "../common/runtime_profile.h"
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

enum Mode : uint32_t { kModeOff = 0, kModeTint = 1, kModeMirror = 2, kModeGrey = 3 };

const char* modeText(uint32_t m) noexcept {
    switch (m) {
        case kModeTint: return "tint";
        case kModeMirror: return "mirror";
        case kModeGrey: return "grey";
        default: return "off";
    }
}

bool parseMode(const std::string& t, uint32_t* out) noexcept {
    if (t == "off") *out = kModeOff;
    else if (t == "tint") *out = kModeTint;
    else if (t == "mirror") *out = kModeMirror;
    else if (t == "grey") *out = kModeGrey;
    else return false;
    return true;
}

// ---- The list and the mode (the draw threads read them; the tick writes them) -----------------------------------------------------------------
std::atomic<uint32_t> g_mode{kModeOff};
cc::PaneTriple g_list[cc::kPaneListCap];
std::atomic<uint32_t> g_listN{0};
std::atomic<bool> g_treat{false};       // on foot in social space, a mode other than off, a list, and the replacement states made
std::atomic<bool> g_capturing{false};   // a capture frame is open
std::atomic<uint64_t> g_treated{0};
std::atomic<ID3D11BlendState*> g_bsFactor{nullptr};   // src = BLEND_FACTOR, dest = ZERO (tint and grey)
std::atomic<ID3D11BlendState*> g_bsMirror{nullptr};   // src = ONE, dest = ZERO
std::atomic<ID3D11PixelShader*> g_psWhite{nullptr};   // tint and grey: the blend factor multiplies this, so it is the pane's colour
bool g_psWhiteTried = false;                          // the tick's: one compile per device
std::atomic<float> g_grey{0.1f};

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
float g_greyLogged = -1.0f;
uint64_t g_lastTickMs = 0;

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
    g_bsFactor.store(nullptr, std::memory_order_release);
    g_bsMirror.store(nullptr, std::memory_order_release);
    g_psWhite.store(nullptr, std::memory_order_release);
    g_psWhiteTried = false;
}

// A failed compile leaves tint and grey multiplying the game's own colour, and the log says so.
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
    if (!ps) Log::get().note("concourse pane: the white pixel shader did not compile; tint and grey multiply the game's own colour, so a dark pane stays dark");
}

bool makeStates(ID3D11Device* device) noexcept {
    if (!device) return false;
    makeWhitePs(device);
    if (g_bsFactor.load(std::memory_order_acquire) && g_bsMirror.load(std::memory_order_acquire)) return true;
    D3D11_BLEND_DESC d{};
    d.AlphaToCoverageEnable = FALSE;
    d.IndependentBlendEnable = FALSE;
    D3D11_RENDER_TARGET_BLEND_DESC& rt = d.RenderTarget[0];
    rt.BlendEnable = TRUE;
    rt.DestBlend = D3D11_BLEND_ZERO;
    rt.BlendOp = D3D11_BLEND_OP_ADD;
    rt.SrcBlendAlpha = D3D11_BLEND_ONE;
    rt.DestBlendAlpha = D3D11_BLEND_ZERO;
    rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
    rt.RenderTargetWriteMask = 0x7;   // red, green and blue: the alpha channel is left alone
    ID3D11BlendState* factor = nullptr;
    ID3D11BlendState* mirror = nullptr;
    rt.SrcBlend = D3D11_BLEND_BLEND_FACTOR;
    const HRESULT h1 = device->CreateBlendState(&d, &factor);
    rt.SrcBlend = D3D11_BLEND_ONE;
    const HRESULT h2 = device->CreateBlendState(&d, &mirror);
    if (FAILED(h1) || FAILED(h2) || !factor || !mirror) {
        Log::get().note("concourse pane: the replacement blend states could not be made (hr %08X, %08X); no pane is treated", static_cast<unsigned>(h1),
                        static_cast<unsigned>(h2));
        if (factor) factor->Release();
        if (mirror) mirror->Release();
        return false;
    }
    g_bsMirror.store(mirror, std::memory_order_release);
    g_bsFactor.store(factor, std::memory_order_release);
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

// ---- The draw hooks ---------------------------------------------------------------------------------------------------------------------------
// The capture's qualifying test: RT0 blending on, a colour target bound, a viewport at least 1000 wide. Opens a query around the draw.
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

}  // namespace

void ConcoursePaneScope::begin(ID3D11DeviceContext* ctx, UINT indexCount, ConcoursePaneSetBlendFn setBlend, ConcoursePaneSetPsFn setPs) noexcept {
    if (!ctx) return;
    if (g_capturing.load(std::memory_order_acquire)) {
        const int q = captureBegin(ctx, indexCount);
        if (q >= 0) {
            ctx_ = ctx;
            query_ = q;
            state_ = 1;
        }
        return;
    }
    if (!g_treat.load(std::memory_order_acquire) || !setBlend) return;
    const uint32_t listN = g_listN.load(std::memory_order_acquire);
    bool sizeMatches = false;
    for (uint32_t i = 0; i < listN && i < cc::kPaneListCap; ++i) {
        if (g_list[i].n == indexCount) {
            sizeMatches = true;
            break;
        }
    }
    if (!sizeMatches) return;   // the index count first: the shader hashes are fetched only for a draw of a listed size
    uint64_t vs = 0, ps = 0;
    shaderHashes(ctx, &vs, &ps);
    const int slot = cc::paneTripleIndex(g_list, listN < cc::kPaneListCap ? listN : cc::kPaneListCap, vs, ps, indexCount);
    if (slot < 0) return;
    const uint32_t mode = g_mode.load(std::memory_order_relaxed);
    ID3D11BlendState* repl = mode == kModeMirror ? g_bsMirror.load(std::memory_order_acquire) : g_bsFactor.load(std::memory_order_acquire);
    if (!repl) return;
    FLOAT factor[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    if (mode == kModeTint) {
        const float* c = cc::kPaneColours[static_cast<size_t>(slot) % cc::kPaneColourCount];
        for (int k = 0; k < 3; ++k) factor[k] = c[k] * cc::kPaneTintIntensity;
    } else if (mode == kModeGrey) {
        const float g = g_grey.load(std::memory_order_relaxed);
        factor[0] = factor[1] = factor[2] = g;
    }
    ctx->OMGetBlendState(&prev_, prevFactor_, &prevMask_);
    setBlend(ctx, repl, factor, 0xFFFFFFFFu);
    if (mode != kModeMirror && setPs) {
        ID3D11PixelShader* white = g_psWhite.load(std::memory_order_acquire);
        if (white) {
            ctx->PSGetShader(&prevPs_, nullptr, nullptr);
            setPs(ctx, white, nullptr, 0);
            setPs_ = setPs;
        }
    }
    ctx_ = ctx;
    setBlend_ = setBlend;
    state_ = 2;
    g_treated.fetch_add(1, std::memory_order_relaxed);
}

void ConcoursePaneScope::end() noexcept {
    if (state_ == 2) {
        setBlend_(ctx_, prev_, prevFactor_, prevMask_);
        if (prev_) prev_->Release();
        prev_ = nullptr;
        if (setPs_) {
            setPs_(ctx_, prevPs_, nullptr, 0);
            if (prevPs_) prevPs_->Release();
            prevPs_ = nullptr;
            setPs_ = nullptr;
        }
    } else if (state_ == 1 && query_ >= 0) {
        FlatComputeInternalScope internal;
        ctx_->End(g_pool[query_]);
    }
    state_ = 0;
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

// A capture replaces the list, so one screenshot's colours name one capture's draws.
void replaceList(const cc::PaneTriple* picks, uint32_t count) noexcept {
    g_listN.store(0, std::memory_order_release);
    char seed[512] = {};
    size_t used = 0;
    for (uint32_t i = 0; i < count; ++i) {
        g_list[i] = picks[i];
        Log::get().note("concourse pane: slot %u colour %s vs=%016llX ps=%016llX n=%u", i, cc::kPaneColourNames[i % cc::kPaneColourCount],
                        static_cast<unsigned long long>(picks[i].vs), static_cast<unsigned long long>(picks[i].ps), picks[i].n);
        const int w = std::snprintf(seed + used, sizeof(seed) - used, "%s%016llX/%016llX/%u", i ? "," : "", static_cast<unsigned long long>(picks[i].vs),
                                    static_cast<unsigned long long>(picks[i].ps), picks[i].n);
        if (w > 0 && used + static_cast<size_t>(w) < sizeof(seed)) used += static_cast<size_t>(w);
    }
    g_listN.store(count, std::memory_order_release);
    Log::get().note("concourse pane: this capture as diag.concourse_panes = %s", seed);
}

void reportCapture(ID3D11Device* device) noexcept {
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
    // The top eight candidates, one colour each. A pane darkens or tints what is behind it, so additive draws are skipped: they are the light volumes, the sky
    // and the light cards, they cover the most pixels, and they would fill the list. So is the green fog, by name. A pane may be a plain quad (n = 6), so a
    // quad is skipped only at 90% of the viewport or more, which is a full-screen pass. All of them stay in the ranking above.
    constexpr uint64_t kFogVs = 0xB12F7A618E1BDE98ull;
    constexpr uint32_t kPicks = 8;
    cc::PaneTriple picks[kPicks];
    uint32_t taken = 0;
    for (size_t i = 0; i < gn && taken < kPicks; ++i) {
        const Group& g = groups[i];
        const double pixels = static_cast<double>(g.vpW) * static_cast<double>(g.vpH);
        if (!(pixels > 0.0) || g.n < 6 || g.dst == D3D11_BLEND_ONE || g.vs == kFogVs) continue;
        const double share = static_cast<double>(g.samples) / pixels;
        if (share < 0.002 || (g.n == 6 && share >= 0.9)) continue;
        picks[taken].vs = g.vs;
        picks[taken].ps = g.ps;
        picks[taken].n = g.n;
        ++taken;
    }
    if (gn == 0) Log::get().note("concourse pane capture: no blended draw covered a pixel (or no result arrived); the list is unchanged");
    else if (taken == 0) Log::get().note("concourse pane capture: no candidate (not additive, at least 6 indices, at least 0.2%% of its viewport); the list is unchanged");
    else {
        replaceList(picks, taken);
        setMode(kModeTint, "capture");
    }
    (void)device;
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
    bindKey(g_keyCapture, g_keyCaptureText, Config::get().getString("hotkey.concourse_pane_capture", ""), "hotkey.concourse_pane_capture", "capture key");
    bindKey(g_keyMode, g_keyModeText, Config::get().getString("hotkey.concourse_pane_mode", ""), "hotkey.concourse_pane_mode", "mode key");
    g_keysBound = true;
    const std::string modeT = Config::get().getString("diag.concourse_pane_mode", "off");
    if (!g_modeTextSeen || modeT != g_modeAppliedText) {
        g_modeTextSeen = true;
        g_modeAppliedText = modeT;
        uint32_t m = kModeOff;
        if (parseMode(modeT, &m)) setMode(m, "diag.concourse_pane_mode");
        else Log::get().note("concourse pane: diag.concourse_pane_mode = \"%s\" is not off, tint, mirror or grey; the mode is unchanged", modeT.c_str());
    }
    const std::string panesT = Config::get().getString("diag.concourse_panes", "");
    if (!g_panesTextSeen || panesT != g_panesAppliedText) {
        const bool first = !g_panesTextSeen;
        g_panesTextSeen = true;
        g_panesAppliedText = panesT;
        if (!(first && panesT.empty())) {
            cc::PaneTriple parsed[cc::kPaneListCap];
            size_t bad = 0;
            const size_t n = cc::parsePaneList(panesT.c_str(), parsed, cc::kPaneListCap, &bad);
            g_listN.store(0, std::memory_order_release);
            for (size_t i = 0; i < n; ++i) g_list[i] = parsed[i];
            g_listN.store(static_cast<uint32_t>(n), std::memory_order_release);
            Log::get().note("concourse pane: diag.concourse_panes: %u pane(s) set, %u entr%s refused (an entry is VS/PS/N: two 16-digit hex hashes and an index count)",
                            static_cast<unsigned>(n), static_cast<unsigned>(bad), bad == 1 ? "y" : "ies");
            for (size_t i = 0; i < n; ++i)
                Log::get().note("concourse pane: slot %u colour %s vs=%016llX ps=%016llX n=%u", static_cast<unsigned>(i),
                                cc::kPaneColourNames[i % cc::kPaneColourCount], static_cast<unsigned long long>(parsed[i].vs),
                                static_cast<unsigned long long>(parsed[i].ps), parsed[i].n);
        }
    }
    float grey = Config::get().getFloat("diag.concourse_pane_grey", 0.1f);
    if (!(grey >= 0.0f)) grey = 0.0f;
    if (grey > 4.0f) grey = 4.0f;
    g_grey.store(grey, std::memory_order_relaxed);
    if (grey != g_greyLogged) {
        if (g_greyLogged >= 0.0f) Log::get().note("concourse pane: grey level %.3f (diag.concourse_pane_grey)", static_cast<double>(grey));
        g_greyLogged = grey;
    }
}

}  // namespace

uint64_t concoursePaneTakeTreated() noexcept { return g_treated.exchange(0, std::memory_order_relaxed); }
const char* concoursePaneModeName() noexcept { return modeText(g_mode.load(std::memory_order_relaxed)); }
uint32_t concoursePaneSlotCount() noexcept { return g_listN.load(std::memory_order_relaxed); }
bool concoursePaneTreating() noexcept { return g_treat.load(std::memory_order_relaxed); }

void concoursePaneTick(ID3D11Device* device, uint32_t frame, uint64_t nowMs) noexcept {
    g_lastTickMs = nowMs;
    if (device != g_device) {
        releaseStates();
        g_poolMade = false;   // the old queries belong to the old device and are left alone
        for (size_t i = 0; i < kPool; ++i) g_pool[i] = nullptr;
        g_device = device;
    }
    pollConfig(nowMs);
    const JournalRawStatus raw = journalRawStatus();
    const bool known = journalOnFootKnown() || (runtimeFlatProfile() && raw.gameplay);
    g_social = cc::concourseSocialOnFoot(journalStatusFlags2(), known, raw.gameplay);
    if (g_keyMode && g_keyMode->pressed()) {
        const uint32_t cur = g_mode.load(std::memory_order_relaxed);
        const uint32_t next = cur == kModeOff ? kModeTint : cur == kModeTint ? kModeMirror : cur == kModeMirror ? kModeGrey : kModeOff;
        setMode(next, g_social ? "hotkey.concourse_pane_mode" : "hotkey.concourse_pane_mode; it applies on foot in social space");
    }
    if (g_keyCapture && g_keyCapture->pressed()) {
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
            reportCapture(device);
            g_capState = CapState::kIdle;
        }
    }
    const bool wantStates = g_mode.load(std::memory_order_relaxed) != kModeOff && g_listN.load(std::memory_order_relaxed) > 0 && g_social;
    const bool states = wantStates ? makeStates(device) : (g_bsFactor.load(std::memory_order_acquire) != nullptr);
    g_treat.store(wantStates && states, std::memory_order_release);
    detail::g_concoursePaneArmed.store((wantStates && states) || g_capturing.load(std::memory_order_relaxed), std::memory_order_release);
}

}  // namespace edvr
