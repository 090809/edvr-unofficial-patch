#pragma once
// The concourse's glass panes (docs\concourse-cull-2026-11-05.md, "The panes"). TEMPORARY. While the concourse cull is wanted (key or override on, on foot in a
// station's social space, the anchor seen), the station's glass family is treated around the game's own draw, and the draws that come after the panes and paint
// over them (the plume and its twin) are withheld while the cull is on. The glass family is named by its vertex shader and pixel shader (concourse_cull_core.h,
// kPaneFamilyVs / kPaneFamilyPs, any index count); diag.concourse_panes adds entries. The modes (diag.concourse_pane_mode, F4):
//   mirror     the game's shader with three stand-in textures (t8, t9, t10) and an opaque colour write (RGB only), writing depth when fully opaque
//   mirror-cb  mirror, and the pixel shader's constant buffer 2 replaced by EDVR's own (F0 and roughness scales of 1)
//   tint       a white pixel shader and a blend-factor colour per index count, opaque, so each window type can be told apart by eye
//   off        the game's own glass, unchanged
// The bay window (36 indices) uses the bay opacity the clearing machine feeds (concourse_cull_core.h, ClearMachine); every other family draw is opaque.
// Family draws are also measured: an occlusion query around the draw gives the share of the viewport it covers, whatever the mode.
//
// The draw thunks of vscreen.cpp (hookedDrawIndexed and hookedDrawIndexedInstanced, the flat branch and the VR path) build one ConcoursePaneScope around the
// real draw call only, and skip the real call when withheld() says so. With nothing armed the scope is one relaxed atomic load.
#include <d3d11_1.h>

#include <atomic>
#include <cstdint>

namespace edvr {

namespace detail {
extern std::atomic<bool> g_concoursePaneArmed;   // a capture frame is open, or panes are treated, measured or withheld (written by the tick and the feed)
}

// The game's own entry points, past EDVR's thunks (so the binding shadow and the issue epoch do not move; the state is put back before anything else looks).
using ConcoursePaneSetBlendFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11BlendState*, const FLOAT[4], UINT);
using ConcoursePaneSetPsFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11PixelShader*, ID3D11ClassInstance* const*, UINT);
using ConcoursePaneSetSrvFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, ID3D11ShaderResourceView* const*);
using ConcoursePaneSetDepthFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11DepthStencilState*, UINT);
struct ConcoursePaneCalls {
    ConcoursePaneSetBlendFn setBlend;
    ConcoursePaneSetPsFn setPs;
    ConcoursePaneSetSrvFn setSrv;
    ConcoursePaneSetDepthFn setDepth;
};

class ConcoursePaneScope {
public:
    ConcoursePaneScope(ID3D11DeviceContext* ctx, UINT indexCount, const ConcoursePaneCalls& calls) noexcept {
        if (detail::g_concoursePaneArmed.load(std::memory_order_relaxed)) begin(ctx, indexCount, calls);
    }
    ~ConcoursePaneScope() {
        if (flags_ != 0) end();
    }
    ConcoursePaneScope(const ConcoursePaneScope&) = delete;
    ConcoursePaneScope& operator=(const ConcoursePaneScope&) = delete;

    // True for a draw that is not issued (the plume and its twin while the cull is on): the site skips only the real draw call.
    bool withheld() const noexcept { return withheld_; }

private:
    void begin(ID3D11DeviceContext* ctx, UINT indexCount, const ConcoursePaneCalls& calls) noexcept;
    void end() noexcept;

    ID3D11DeviceContext* ctx_ = nullptr;
    const ConcoursePaneCalls* calls_ = nullptr;
    uint32_t flags_ = 0;   // which of the things to put back or close are open
    bool withheld_ = false;
    int query_ = -1;
    int capQuery_ = -1;
    ID3D11BlendState* prevBs_ = nullptr;
    FLOAT prevFactor_[4] = {};
    UINT prevMask_ = 0;
    ID3D11PixelShader* prevPs_ = nullptr;
    ID3D11ShaderResourceView* prevSrv_[3] = {};
    ID3D11DepthStencilState* prevDs_ = nullptr;
    UINT prevStencilRef_ = 0;
    ID3D11DeviceContext1* ctx1_ = nullptr;
    ID3D11Buffer* prevCb_ = nullptr;
    UINT prevCbFirst_ = 0, prevCbNum_ = 0;
};

// What the concourse tick tells the panes each frame.
struct ConcoursePaneFeed {
    bool measure = false;       // the gate holds: family draws are measured
    bool treat = false;         // the cull is wanted: family draws are treated by the mode
    bool withhold = false;      // the cull is on and the phase is the plain mirror phase: the plume and its twin are not issued
    float bayOpacity = 1.0f;    // the bay window's opacity, 0 to 1
};
void concoursePaneSetFeed(const ConcoursePaneFeed& feed) noexcept;

// The latest completed frame's shares of the viewport, per index count (family draws only).
struct ConcoursePaneShares {
    bool valid = false;     // a reading from the last second
    uint32_t seq = 0;       // moves with each new frame read
    float bay = -1.0f;      // the largest share of a bay-window (36 indices) draw; 0 when none was drawn; negative when not valid
    uint32_t count = 0;
    uint32_t n[8] = {};
    float share[8] = {};    // 0 to 1
};
ConcoursePaneShares concoursePaneShares() noexcept;

// The frame tick, once a frame at the Present boundary (device_hook.cpp presentFrameBoundary): the keys, the capture's state machine, the mode and the list,
// the measurement's queries and the material read-back, the armed flag. `device` is the game's device; nowMs is GetTickCount64().
void concoursePaneTick(ID3D11Device* device, uint32_t frame, uint64_t nowMs) noexcept;

// Treated draws and withheld draws since the last call (the cull's 5 s summary reports them, so "matched nothing" is visible).
uint64_t concoursePaneTakeTreated() noexcept;
uint64_t concoursePaneTakeWithheld() noexcept;
// The mode ("mirror", "mirror-cb", "tint", "off") and the number of panes in the list, for the summary.
const char* concoursePaneModeName() noexcept;
uint32_t concoursePaneSlotCount() noexcept;
// Panes are being treated: the feed says treat, the mode is not off and the replacement states are made.
bool concoursePaneTreating() noexcept;

}  // namespace edvr
