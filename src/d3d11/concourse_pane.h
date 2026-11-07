#pragma once
// The concourse's glass panes (docs\concourse-cull-2026-11-05.md, "The panes"). TEMPORARY. While the concourse cull is wanted (key or override on, on foot, in a
// station's social space, the anchor seen), the station's glass family is treated, and the outside is not drawn while the cull is on: the plume and its twin, which
// paint over the panes, and the smoke, ship lasers, engine trails, atmosphere and planet (concourse_cull_core.h, kWithheldPs and kWithheldVs), whatever the mode. The glass family is named by its vertex shader and pixel shader (concourse_cull_core.h, kPaneFamilyVs,
// kPaneFamilyPs, any index count); diag.concourse_panes adds entries. The game's own glass draw is always issued, unchanged; the treatment is a SECOND draw of
// the same arguments, right after it, that paints the pane over. The modes (diag.concourse_pane_mode, F4):
//   mirror       the second draw is the game's shader with three stand-in textures (t8, t9, t10), opaque (RGB only), writing depth
//   mirror-soft  mirror, and the pixel shader's constant buffer 2 replaced by EDVR's own (roughness scaled to 0.54)
//   tint         the second draw is a white pixel shader and a blend-factor colour per index count, opaque, so each window type can be told apart by eye
//   off          the game's own glass, unchanged
//   mirror-backdrop  mirror-steady's one opaque pass, with a pixel shader that samples a cube of view directions captured on a key press (PROTOTYPE, the end of this header)
// While the cull is on (the plain phase), the second draw is limited to the pixels whose scene behind the pane is farther than diag.concourse_mirror_beyond
// metres: a mark pass (one fullscreen triangle at that depth, writing stencil bit 0x40 where the scene is farther) runs once a frame before the first mirror
// draw, and the mirror draw tests that bit. Glass with a wall behind it (a terminal screen) is left as the game drew it. Each pane's side, learned by two
// occlusion queries (the game's glass samples, the mirror's samples), decides which panes still mirror once the cull is lifted (the bay window clearing).
// Family draws are also measured: an occlusion query around the game's draw gives the share of the viewport it covers, whatever the mode.
//
// The draw thunks of vscreen.cpp (hookedDraw and hookedDrawInstanced for what is withheld; hookedDrawIndexed and hookedDrawIndexedInstanced for that and the
// mirror; the flat branch and the VR path of each) build one ConcoursePaneScope around the
// real draw call, skip the real call when withheld() says so, and issue the second draw when nextMirrorPass() says so. With nothing armed the scope is one
// relaxed atomic load, and nextMirrorPass() is a check of one member that is false for every draw that is not the family's.
#include <d3d11_1.h>

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace edvr {

namespace detail {
extern std::atomic<bool> g_concoursePaneArmed;   // a capture frame is open, or panes are treated, measured or withheld (written by the tick and the feed)
}

// The game's own entry points, past EDVR's thunks (so the binding shadow and the issue epoch do not move; the state is put back before anything else looks).
// The input layout, topology and rasteriser state setters are not hooked at all, so the context's own methods are used for those.
using ConcoursePaneSetBlendFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11BlendState*, const FLOAT[4], UINT);
using ConcoursePaneSetPsFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11PixelShader*, ID3D11ClassInstance* const*, UINT);
using ConcoursePaneSetVsFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11VertexShader*, ID3D11ClassInstance* const*, UINT);
using ConcoursePaneSetSrvFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, ID3D11ShaderResourceView* const*);
using ConcoursePaneSetDepthFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11DepthStencilState*, UINT);
using ConcoursePaneDrawFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT);
struct ConcoursePaneCalls {
    ConcoursePaneSetBlendFn setBlend;
    ConcoursePaneSetPsFn setPs;
    ConcoursePaneSetSrvFn setSrv;
    ConcoursePaneSetDepthFn setDepth;
    ConcoursePaneSetVsFn setVs;
    ConcoursePaneDrawFn draw;
};

class ConcoursePaneScope {
public:
    ConcoursePaneScope(ID3D11DeviceContext* ctx, UINT indexCount, UINT startIndex, INT baseVertex, const ConcoursePaneCalls& calls) noexcept {
        if (detail::g_concoursePaneArmed.load(std::memory_order_relaxed)) begin(ctx, indexCount, startIndex, baseVertex, calls);
    }
    ~ConcoursePaneScope() {
        if (flags_ != 0) end();
    }
    ConcoursePaneScope(const ConcoursePaneScope&) = delete;
    ConcoursePaneScope& operator=(const ConcoursePaneScope&) = delete;

    // True for a draw that is not issued (an outside draw while the cull is on): the site skips only the real draw call.
    bool withheld() const noexcept { return withheld_; }
    // Called right after the game's own draw was issued: true when the site is to issue the same draw again (the mirror), with the replacement state bound. The
    // state is put back when the scope ends. False for every draw that is not a family draw.
    // Asked after the game's own draw and after each pass: true means issue the same draw again. First the mirror, then, in the lit modes, its glow.
    bool nextMirrorPass() noexcept { return family_ && nextMirrorPassSlow(); }

private:
    void begin(ID3D11DeviceContext* ctx, UINT indexCount, UINT startIndex, INT baseVertex, const ConcoursePaneCalls& calls) noexcept;
    bool nextMirrorPassSlow() noexcept;
    void bindPsCb(ID3D11Buffer* own) noexcept;   // constant buffer 2 of the pixel stage, the previous one kept for end()
    bool beginMirrorSlow() noexcept;
    void closeGameQueries() noexcept;   // the share query and the side test's first query end with the game's own draw, before any pass of ours
    bool beginCapturePass() noexcept;   // PROTOTYPE (backdrop): binds the capture pass; false leaves the context as it was
    void endCapturePass() noexcept;     // puts the capture pass's state back (the UAV, the pixel shader, t8, the blend and depth states)
    bool beginProbePass() noexcept;     // PROTOTYPE (automatic capture): the colourless probe pass with its own occlusion query; false leaves the context as it was
    void endProbePass() noexcept;       // ends the probe's query and puts its state back (all but the constant buffer, which the display pass keeps and end() restores)
    void end() noexcept;

    ID3D11DeviceContext* ctx_ = nullptr;
    const ConcoursePaneCalls* calls_ = nullptr;
    uint32_t flags_ = 0;   // which of the things to put back or close are open
    bool family_ = false;   // the draw is a pane of the list
    bool withheld_ = false;
    // The plan begin() decides and beginMirrorSlow() carries out.
    bool planMirror_ = false;
    bool planMark_ = false;     // the mirror draw is limited to the marked pixels
    bool planOpaque_ = true;    // false: the fade (blend factor opacity)
    bool planTint_ = false;
    bool planSoft_ = false;     // mirror-soft: EDVR's constant buffer 2
    float planGlow_ = 0.0f;     // the lit modes: the glow pass's blend factor (0: no glow pass)
    bool planSteady_ = false;   // the steady modes: the glow pass is a flat colour, with no light or shadow in it
    bool mirrorBound_ = false;  // the mirror pass has been bound
    bool planBackdrop_ = false; // mirror-backdrop: the steady pass with the cube-sampling pixel shader
    bool planCapture_ = false;  // an armed backdrop capture: this exterior pane gets a capture pass before its display pass
    bool capDone_ = false;      // the capture pass has been asked for
    bool capBound_ = false;     // the capture pass's state is bound (endCapturePass puts it back)
    bool planProbe_ = false;    // a test frame in mirror-backdrop with the mark: a probe pass runs before the display pass
    bool probeDone_ = false;    // the probe pass has been asked for
    bool probeBound_ = false;   // the probe pass's state is bound and its query is open (endProbePass closes and restores; the capture pass's save fields are used, the two never share a scope)
    ID3D11SamplerState* probePrevSampler_ = nullptr;
    ID3D11BlendState* capPrevBs_ = nullptr;
    FLOAT capPrevFactor_[4] = {};
    UINT capPrevMask_ = 0;
    ID3D11PixelShader* capPrevPs_ = nullptr;
    ID3D11ShaderResourceView* capPrevSrv_[3] = {};
    ID3D11DepthStencilState* capPrevDs_ = nullptr;
    UINT capPrevRef_ = 0;
    ID3D11SamplerState* prevSampler_ = nullptr;   // the display pass's sampler slot, the game's kept for end()
    FLOAT planFactor_[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    uint32_t keyN_ = 0, keyStart_ = 0;
    int32_t keyBase_ = 0;
    int query_ = -1;
    int capQuery_ = -1;
    int testSlot_ = -1;
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
    bool withhold = false;      // the cull is on and the phase is the plain mirror phase: the outside draws are not issued, the mirror is marked
    bool effects = false;       // the outside effects are not issued although the phase is not the plain one (the backdrop capture's lift); withhold implies it
    bool sealed = false;        // the hangar is sealed: the families that can only be outside are not issued (concourse_cull_core.h, sealedToo)
    float bayOpacity = 1.0f;    // the clearing pane's opacity, 0 to 1
    float enterShare = 0.0f;    // the share of the view at which the clearing starts (diag.concourse_clear_share); 0 never: a pane read as exterior that fills this much becomes the clearing pane
    float up[3] = {};           // the anchor's local up axis in world (the steady glow's sky test); zero when not valid
    bool upValid = false;
};
void concoursePaneSetFeed(const ConcoursePaneFeed& feed) noexcept;

// The latest completed frame's shares of the viewport, per index count (family draws only).
struct ConcoursePaneShares {
    bool valid = false;     // a reading from the last 250 ms
    uint32_t seq = 0;       // moves with each new frame read
    float bay = -1.0f;      // the largest share of a clearing-pane draw (n == the clearing pane's index count: 36, the bay window, until another exterior pane is approached); 0 when none was drawn; negative when not valid
    uint32_t count = 0;
    uint32_t n[8] = {};
    float share[8] = {};    // 0 to 1
};
ConcoursePaneShares concoursePaneShares() noexcept;

// The frame tick, once a frame at the Present boundary (device_hook.cpp presentFrameBoundary): the keys, the capture's state machine, the mode and the list,
// the measurement's queries and the material read-back, the armed flag. `device` is the game's device; nowMs is GetTickCount64().
void concoursePaneTick(ID3D11Device* device, uint32_t frame, uint64_t nowMs) noexcept;

// Treated draws (mirror draws issued) and withheld draws since the last call (the cull's 5 s summary reports them, so "matched nothing" is visible).
uint64_t concoursePaneTakeTreated() noexcept;
uint64_t concoursePaneTakeWithheld() noexcept;
// An F3 press since the last call: the cull runs its names survey too, so the log names what is left in view.
bool concoursePaneTakeSurveyRequest() noexcept;
// The mirror's own counters: mark passes run and refused (no depth view, no stencil, a geometry stage bound, states missing) and side tests read since the last
// call; the side table as it stands now. All zero when the mirror never ran.
struct ConcoursePaneMirrorStats {
    uint64_t markPasses = 0, markRefused = 0, tests = 0;
    uint32_t known = 0, exterior = 0, dropped = 0;
};
ConcoursePaneMirrorStats concoursePaneTakeMirrorStats() noexcept;
// The mode ("mirror-backdrop", "mirror-steady", "mirror", "mirror-soft", "tint", "off") and the number of panes in the list, for the summary.
const char* concoursePaneModeName() noexcept;
// The index count treated as the clearing pane now (36 until an exterior pane fills the view).
uint32_t concoursePaneClearingN() noexcept;
uint32_t concoursePaneSlotCount() noexcept;
// Panes are being treated: the feed says treat, the mode is not off and the replacement states are made.
bool concoursePaneTreating() noexcept;

// ---- The backdrop capture (PROTOTYPE on the concourse arc, docs\concourse-cull-2026-11-05.md "The window plan") ----------------------------------------------
// The cull's tick lifts the cull for kBackdropLiftMs, then calls concoursePaneArmCapture(). In the next frame (up to kBackdropWaitFrames) the first pane draw copies the
// bound render target, and each exterior pane gets a capture pass that writes the scene behind it into the cube's back texture; the tick after copies it to the front
// texture the display pass samples. The status says what happened, so the cull's tick can log it once and end the lift.
enum class ConcourseBackdropPhase : uint8_t { kIdle, kArmed, kDone, kRefused, kEmpty };
struct ConcourseBackdropStatus {
    ConcourseBackdropPhase phase = ConcourseBackdropPhase::kIdle;
    bool sceneCopied = false;        // a frame's first pane draw copied the render target
    uint32_t sceneW = 0, sceneH = 0;
    char sceneFormat[28] = {};
    bool originSet = false;          // this capture set the cube's origin (false: the visit's earlier origin was kept)
    uint32_t captures = 0;           // captures that have gone into this cube, this one included once done
    uint32_t paneDraws = 0;          // capture passes issued
    uint32_t framesWaited = 0;
    char refusal[200] = {};          // kRefused: why
};
void concoursePaneSetBackdropMode(const char* why) noexcept;   // F6: the mode becomes mirror-backdrop (a toast says so)
void concoursePaneArmCapture(bool toast) noexcept;               // the capture is armed from the next tick; the status follows; `toast`: its result is said on screen (F6's, not an automatic one's)
ConcourseBackdropStatus concoursePaneBackdropStatus() noexcept;
// Automatic capture: the pane mode is mirror-backdrop and a hotkey (F4 to it, or F6) turned automatic capture on.
bool concoursePaneBackdropAuto() noexcept;
bool concoursePaneIsBackdropMode() noexcept;
// What the probes measured: the share of the viewport the cube cannot show through the exterior panes, over the last 8 frames (0 when nothing was measured), and the probe
// counters since the process began (issued by the draw threads, read by the tick), so a probe that never ran, or never returned, can be told from a share that is just low.
struct ConcourseBackdropMissing {
    float share = 0.0f;
    uint64_t probesIssued = 0, probesRead = 0;
    bool cubeValid = false;
};
ConcourseBackdropMissing concoursePaneBackdropMissing() noexcept;
void concoursePaneBackdropEnd() noexcept;                      // the cull's tick has read the result (or gives up): back to idle, and an open capture is closed
// A toast text, once per hotkey change of the mode and per capture result ("Windows: ..."); false when none is waiting. The tick hands it to menuShowNotice.
bool concoursePaneTakeToast(char* out, size_t size) noexcept;
void concoursePaneQueueToast(const char* text) noexcept;   // TEMPORARY: the camera test's key (optimized_station.cpp) shows its mode through the same toast

// ---- The bake (TEMPORARY, docs\concourse-cull-2026-11-05.md "Flight 24 results"): the cube filled at arrival from the room's centre, in the frames the camera is moved for ------
// Arm when a ride up is armed: the cubes and the shader are made and cleared in the tick, while the cabin is shut, and the back buffer is kept each Present. Begin when the anchor is first
// seen: the kept image is shown from then on, and each frame the resolve sees at the centre goes into the cube. Finish when the frames are taken: the cube is published and the pane
// mode becomes the captured view; the hold ends when the resolve sees the camera back. Cancel ends it with nothing published.
bool concoursePaneBakeDone() noexcept;                                      // the windows have a bake to show
bool concoursePaneBakeWouldBeSharper(uint64_t nowMs) noexcept;              // the render size has risen past the bake's and held steady
void concoursePaneBakeRelease() noexcept;                                   // leaving the station: the cube goes at the next tick, and the next arrival bakes afresh
void concoursePaneBakeArm(bool on) noexcept;
bool concoursePaneBakeBegin(const float centre[3], float nearM) noexcept;   // false when the cube was not ready: the frames still move, nothing is captured or held
void concoursePaneBakeExpectLast(const float forward[3]) noexcept;          // where the last direction looks: its arrival at the resolve is the end of the bake
bool concoursePaneBakeLastSeen() noexcept;
void concoursePaneBakeFinish() noexcept;
void concoursePaneBakeCancel() noexcept;
// From the flat runtime, with what the resolve is about to read: the scene's colour and depth before the visor and the HUD, and b1[270..275]. One relaxed load when no bake runs.
void concoursePaneBakeFrame(ID3D11DeviceContext* ctx, ID3D11ShaderResourceView* color, ID3D11ShaderResourceView* depth, const float camera[6][4], uint32_t width, uint32_t height) noexcept;
// From Present, just before the real one: keeps the back buffer, or puts the kept image back. One relaxed load when no bake runs.
void concoursePaneBakePresent(IDXGISwapChain* swap) noexcept;

}  // namespace edvr
