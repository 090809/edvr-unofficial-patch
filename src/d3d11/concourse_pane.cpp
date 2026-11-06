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

enum Mode : uint32_t { kModeMirror = 0, kModeMirrorCb = 1, kModeTint = 2, kModeOff = 3 };

const char* modeText(uint32_t m) noexcept {
    switch (m) {
        case kModeMirrorCb: return "mirror-cb";
        case kModeTint: return "tint";
        case kModeOff: return "off";
        default: return "mirror";
    }
}

bool parseMode(const std::string& t, uint32_t* out) noexcept {
    if (t == "mirror") *out = kModeMirror;
    else if (t == "mirror-cb") *out = kModeMirrorCb;
    else if (t == "tint") *out = kModeTint;
    else if (t == "off") *out = kModeOff;
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

// ---- The list, the mode and the feed (the draw threads read them; the tick and the feed write them) --------------------------------------------
std::atomic<uint32_t> g_mode{kModeMirror};
cc::PaneTriple g_list[cc::kPaneListCap] = {{cc::kPaneFamilyVs, cc::kPaneFamilyPs, 0}};   // entry 0 is the built-in glass family; diag.concourse_panes adds
std::atomic<uint32_t> g_listN{1};
std::atomic<bool> g_feedMeasure{false}, g_feedTreat{false}, g_feedWithhold{false};
std::atomic<float> g_bayOpacity{1.0f};
std::atomic<bool> g_treatNow{false};      // treat fed, a mode other than off, and the replacement states made
std::atomic<bool> g_withholdNow{false};   // withhold fed and a mode other than off
std::atomic<bool> g_shareOn{false};       // measure fed and the query pool made
std::atomic<bool> g_capturing{false};     // a capture frame is open
std::atomic<uint64_t> g_treated{0};
std::atomic<uint64_t> g_withheld{0};
std::atomic<uint32_t> g_frameNow{0};
std::atomic<uint32_t> g_hashEpoch{1};

// The replacement states, made by the tick for the game's device.
std::atomic<ID3D11BlendState*> g_bsFactor{nullptr};   // src = BLEND_FACTOR, dest = ZERO (tint)
std::atomic<ID3D11BlendState*> g_bsMirror{nullptr};   // src = ONE, dest = ZERO (a fully opaque pane)
std::atomic<ID3D11BlendState*> g_bsFade{nullptr};     // src = BLEND_FACTOR, dest = INV_BLEND_FACTOR (a pane below full opacity)
std::atomic<ID3D11PixelShader*> g_psWhite{nullptr};   // tint: the blend factor multiplies this, so it is the pane's colour
std::atomic<ID3D11ShaderResourceView*> g_mirrorSrv[3] = {};   // t8, t9, t10
std::atomic<ID3D11Buffer*> g_cbMirror{nullptr};       // mirror-cb: the pixel shader's constant buffer 2
std::atomic<bool> g_statesReady{false};
bool g_psWhiteTried = false;   // the tick's: one compile per device

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

// ---- The derived depth states: the game's, with depth writes on ----------------------------------------------------------------------------------
struct DsEntry {
    ID3D11DepthStencilState* orig = nullptr;   // held, so the pointer cannot be reused for another state
    ID3D11DepthStencilState* derived = nullptr;
};
std::mutex g_dsMutex;
DsEntry g_ds[8];
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
    g_psWhite.store(nullptr, std::memory_order_release);
    for (auto& s : g_mirrorSrv) s.store(nullptr, std::memory_order_release);
    g_cbMirror.store(nullptr, std::memory_order_release);
    g_psWhiteTried = false;
    {
        std::lock_guard<std::mutex> lock(g_dsMutex);
        for (auto& e : g_ds) e = DsEntry{};
        g_dsN = 0;
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
    rt.SrcBlend = D3D11_BLEND_BLEND_FACTOR;
    rt.DestBlend = D3D11_BLEND_ZERO;
    const HRESULT h1 = device->CreateBlendState(&d, &factor);
    rt.SrcBlend = D3D11_BLEND_ONE;
    const HRESULT h2 = device->CreateBlendState(&d, &mirror);
    rt.SrcBlend = D3D11_BLEND_BLEND_FACTOR;
    rt.DestBlend = D3D11_BLEND_INV_BLEND_FACTOR;
    const HRESULT h3 = device->CreateBlendState(&d, &fade);
    if (FAILED(h1) || FAILED(h2) || FAILED(h3) || !factor || !mirror || !fade) {
        Log::get().note("concourse pane: the replacement blend states could not be made (hr %08X, %08X, %08X); no pane is treated", static_cast<unsigned>(h1),
                        static_cast<unsigned>(h2), static_cast<unsigned>(h3));
        if (factor) factor->Release();
        if (mirror) mirror->Release();
        if (fade) fade->Release();
        return false;
    }
    g_bsMirror.store(mirror, std::memory_order_release);
    g_bsFactor.store(factor, std::memory_order_release);
    g_bsFade.store(fade, std::memory_order_release);
    return true;
}

bool makeMirrorTextures(ID3D11Device* device) noexcept {
    static uint8_t t8[cc::kPaneTexSize * cc::kPaneTexSize * 2];
    static uint8_t t9[cc::kPaneTexSize * cc::kPaneTexSize * 2];
    static uint8_t t10[cc::kPaneTexSize * cc::kPaneTexSize * 4];
    cc::fillPaneT8(t8);
    cc::fillPaneT9(t9);
    cc::fillPaneT10(t10);
    struct Spec {
        DXGI_FORMAT format;
        const uint8_t* data;
        UINT bytes;
    };
    const Spec specs[3] = {{DXGI_FORMAT_R8G8_UNORM, t8, 2}, {DXGI_FORMAT_R8G8_UNORM, t9, 2}, {DXGI_FORMAT_R8G8B8A8_UNORM, t10, 4}};
    ID3D11ShaderResourceView* made[3] = {};
    bool ok = true;
    for (int i = 0; i < 3 && ok; ++i) {
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
    for (int i = 0; i < 3; ++i) g_mirrorSrv[i].store(made[i], std::memory_order_release);
    return true;
}

bool makeMirrorCb(ID3D11Device* device) noexcept {
    static const float kRows[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 0};   // row 2: F0 scale 1, roughness scale 1
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = kPaneCbBytes;
    bd.Usage = D3D11_USAGE_IMMUTABLE;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    D3D11_SUBRESOURCE_DATA sd{};
    sd.pSysMem = kRows;
    ID3D11Buffer* b = nullptr;
    const HRESULT h = device->CreateBuffer(&bd, &sd, &b);
    if (FAILED(h) || !b) {
        Log::get().note("concourse pane: the mirror constant buffer could not be made (hr %08X); mirror-cb is not available", static_cast<unsigned>(h));
        return false;
    }
    g_cbMirror.store(b, std::memory_order_release);
    return true;
}

bool makeStates(ID3D11Device* device) noexcept {
    if (!device) return false;
    if (g_statesReady.load(std::memory_order_acquire)) return true;
    makeWhitePs(device);
    if (!g_bsMirror.load(std::memory_order_acquire) && !makeBlendStates(device)) return false;
    if (!g_mirrorSrv[0].load(std::memory_order_acquire) && !makeMirrorTextures(device)) return false;
    if (!g_cbMirror.load(std::memory_order_acquire)) makeMirrorCb(device);   // mirror-cb only: a failure is logged and the plain mirror carries on
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
ID3D11DepthStencilState* derivedDepth(ID3D11DeviceContext* ctx, ID3D11DepthStencilState* orig) noexcept {
    std::lock_guard<std::mutex> lock(g_dsMutex);
    for (size_t i = 0; i < g_dsN; ++i)
        if (g_ds[i].orig == orig) return g_ds[i].derived;
    if (g_dsN >= sizeof(g_ds) / sizeof(g_ds[0])) return nullptr;
    D3D11_DEPTH_STENCIL_DESC d{};
    orig->GetDesc(&d);
    d.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    ID3D11DepthStencilState* derived = nullptr;
    if (dev) {
        if (FAILED(dev->CreateDepthStencilState(&d, &derived))) derived = nullptr;
        dev->Release();
    }
    orig->AddRef();
    g_ds[g_dsN].orig = orig;
    g_ds[g_dsN].derived = derived;
    ++g_dsN;
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

}  // namespace

void ConcoursePaneScope::begin(ID3D11DeviceContext* ctx, UINT indexCount, const ConcoursePaneCalls& calls) noexcept {
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
    const uint64_t ps = pixelShaderHash(ctx);
    if (!ps) return;
    if (g_withholdNow.load(std::memory_order_relaxed) && (ps == cc::kPlumePs || ps == cc::kTwinPs)) {
        withheld_ = true;
        g_withheld.fetch_add(1, std::memory_order_relaxed);
        return;
    }
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
    const bool immediate = ctx->GetType() == D3D11_DEVICE_CONTEXT_IMMEDIATE;
    if (immediate) {
        materialOnce(ctx, indexCount);
        if (g_shareOn.load(std::memory_order_acquire)) {
            query_ = openShareQuery(ctx, indexCount);
            if (query_ >= 0) flags_ |= kFlagQuery;
        }
    }
    const uint32_t mode = g_mode.load(std::memory_order_relaxed);
    if (!g_treatNow.load(std::memory_order_acquire) || mode == kModeOff) return;   // measured only
    const bool tint = mode == kModeTint;
    float opacity = 1.0f;
    if (!tint && indexCount == cc::kBayIndexCount) opacity = std::min(1.0f, std::max(0.0f, g_bayOpacity.load(std::memory_order_relaxed)));
    const bool opaque = opacity >= 0.999f;
    ID3D11BlendState* repl = tint ? g_bsFactor.load(std::memory_order_acquire) : opaque ? g_bsMirror.load(std::memory_order_acquire) : g_bsFade.load(std::memory_order_acquire);
    if (!repl) return;
    FLOAT factor[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    if (tint) {
        const float* c = cc::kPaneColours[tintSlotOf(indexCount) % cc::kPaneColourCount];
        for (int k = 0; k < 3; ++k) factor[k] = c[k] * cc::kPaneTintIntensity;
    } else if (!opaque) {
        factor[0] = factor[1] = factor[2] = factor[3] = opacity;
    }
    ctx->OMGetBlendState(&prevBs_, prevFactor_, &prevMask_);
    calls.setBlend(ctx, repl, factor, 0xFFFFFFFFu);
    flags_ |= kFlagBlend;
    g_treated.fetch_add(1, std::memory_order_relaxed);
    if (tint) {
        ID3D11PixelShader* white = g_psWhite.load(std::memory_order_acquire);
        if (white) {
            ctx->PSGetShader(&prevPs_, nullptr, nullptr);
            calls.setPs(ctx, white, nullptr, 0);
            flags_ |= kFlagPs;
        }
        return;
    }
    ID3D11ShaderResourceView* mine[3] = {g_mirrorSrv[0].load(std::memory_order_acquire), g_mirrorSrv[1].load(std::memory_order_acquire),
                                         g_mirrorSrv[2].load(std::memory_order_acquire)};
    if (mine[0] && mine[1] && mine[2]) {
        ctx->PSGetShaderResources(kPaneSrvFirst, kPaneSrvCount, prevSrv_);
        calls.setSrv(ctx, kPaneSrvFirst, kPaneSrvCount, mine);
        flags_ |= kFlagSrv;
    }
    if (opaque) {
        UINT ref = 0;
        ctx->OMGetDepthStencilState(&prevDs_, &ref);
        ID3D11DepthStencilState* derived = prevDs_ ? derivedDepth(ctx, prevDs_) : nullptr;
        if (derived) {
            prevStencilRef_ = ref;
            calls.setDepth(ctx, derived, ref);
            flags_ |= kFlagDepth;
        } else if (prevDs_) {
            prevDs_->Release();
            prevDs_ = nullptr;
        }
    }
    if (mode == kModeMirrorCb) {
        ID3D11Buffer* own = g_cbMirror.load(std::memory_order_acquire);
        if (own) {
            ctx->QueryInterface(IID_PPV_ARGS(&ctx1_));
            if (ctx1_) {
                // The *ConstantBuffers1 methods are not hooked, so this passes no thunk, and the saved offset window is put back exactly.
                ctx1_->PSGetConstantBuffers1(kPaneCbSlot, 1, &prevCb_, &prevCbFirst_, &prevCbNum_);
                ctx1_->PSSetConstantBuffers1(kPaneCbSlot, 1, &own, nullptr, nullptr);
            } else {
                ctx->PSGetConstantBuffers(kPaneCbSlot, 1, &prevCb_);
                ctx->PSSetConstantBuffers(kPaneCbSlot, 1, &own);
            }
            flags_ |= kFlagCb;
        }
    }
}

void ConcoursePaneScope::end() noexcept {
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
    const std::string modeT = Config::get().getString("diag.concourse_pane_mode", "mirror");
    if (!g_modeTextSeen || modeT != g_modeAppliedText) {
        g_modeTextSeen = true;
        g_modeAppliedText = modeT;
        uint32_t m = kModeMirror;
        if (parseMode(modeT, &m)) setMode(m, "diag.concourse_pane_mode");
        else Log::get().note("concourse pane: diag.concourse_pane_mode = \"%s\" is not mirror, mirror-cb, tint or off; the mode is unchanged", modeT.c_str());
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
    const bool withhold = g_feedWithhold.load(std::memory_order_relaxed) && mode != kModeOff;
    const bool shares = g_feedMeasure.load(std::memory_order_relaxed) && g_shareOn.load(std::memory_order_relaxed);
    g_treatNow.store(treat, std::memory_order_release);
    g_withholdNow.store(withhold, std::memory_order_release);
    detail::g_concoursePaneArmed.store(treat || withhold || shares || g_capturing.load(std::memory_order_relaxed), std::memory_order_release);
}

}  // namespace

uint64_t concoursePaneTakeTreated() noexcept { return g_treated.exchange(0, std::memory_order_relaxed); }
uint64_t concoursePaneTakeWithheld() noexcept { return g_withheld.exchange(0, std::memory_order_relaxed); }
const char* concoursePaneModeName() noexcept { return modeText(g_mode.load(std::memory_order_relaxed)); }
uint32_t concoursePaneSlotCount() noexcept { return g_listN.load(std::memory_order_relaxed); }
bool concoursePaneTreating() noexcept { return g_treatNow.load(std::memory_order_relaxed); }

void concoursePaneSetFeed(const ConcoursePaneFeed& feed) noexcept {
    g_feedMeasure.store(feed.measure, std::memory_order_relaxed);
    g_feedTreat.store(feed.treat, std::memory_order_relaxed);
    g_feedWithhold.store(feed.withhold, std::memory_order_relaxed);
    g_bayOpacity.store(feed.bayOpacity, std::memory_order_relaxed);
    updateArmed();
}

ConcoursePaneShares concoursePaneShares() noexcept {
    std::lock_guard<std::mutex> lock(g_shareMutex);
    ConcoursePaneShares r = g_latest;
    if (r.valid && GetTickCount64() - g_latestMs > 1000) r = ConcoursePaneShares{};
    return r;
}

void concoursePaneTick(ID3D11Device* device, uint32_t frame, uint64_t nowMs) noexcept {
    g_lastTickMs = nowMs;
    if (device != g_device) {
        releaseStates();
        g_poolMade = false;   // the old queries belong to the old device and are left alone
        for (size_t i = 0; i < kPool; ++i) g_pool[i] = nullptr;
        g_device = device;
    }
    const uint32_t prevFrame = g_frameNow.exchange(frame, std::memory_order_relaxed);
    pollConfig(nowMs);
    const JournalRawStatus raw = journalRawStatus();
    const bool known = journalOnFootKnown() || (runtimeFlatProfile() && raw.gameplay);
    g_social = cc::concourseSocialOnFoot(journalStatusFlags2(), known, raw.gameplay);
    if (g_keyMode && g_keyMode->pressed()) {
        const uint32_t cur = g_mode.load(std::memory_order_relaxed);
        const uint32_t next = cur == kModeMirror ? kModeMirrorCb : cur == kModeMirrorCb ? kModeTint : cur == kModeTint ? kModeOff : kModeMirror;
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
            reportCapture();
            g_capState = CapState::kIdle;
        }
    }
    if (device) {
        const bool wantStates = g_feedTreat.load(std::memory_order_relaxed) && g_mode.load(std::memory_order_relaxed) != kModeOff;
        if (wantStates) makeStates(device);
        const bool measuring = g_feedMeasure.load(std::memory_order_relaxed);
        if (measuring && makeSlots(device)) g_shareOn.store(true, std::memory_order_release);
        if (!measuring) g_shareOn.store(false, std::memory_order_release);
        pollShares(device, prevFrame, frame, measuring, nowMs);
        readMaterials(device, frame);
    }
    updateArmed();
}

}  // namespace edvr
