// The own hull as a private layer, Step 1 v2: a lit copy of the own ship's G-buffer, written into the eye dump only
// (diag.hull_layer, TEMPORARY; docs\research-own-hull-layer-2026-11-08.md "Step 1 v2 build"). Nothing the game draws or
// presents changes: every draw below is issued once more, after the game's own issue, into EDVR's private targets,
// and the game's bindings go back before the next game call.
//
// Per eye, inside the eye pass:
//   1. The seed: the eye's first hull-family G-buffer draw (VS 66DE, 61AE, AACF, DE54) after the cockpit shell (VS
//      BFE5) into the same eye depth. Its instance window (the stride-8 instance stream, record index first) and the
//      pool (VS t33, 336-byte records, position at bytes 16-27) are copied, and one compute dispatch marks every pool
//      record within 120 m of the seed records' centroid in the eye's own mask (one uint a record). The mask's count
//      is read back a few frames later, never with a stall. Needs engine motion's patched shaders (part of fix.temporal_aa):
//      without the slot target at MRT6 on the seed draw the eye-frame does nothing (no-slot-target).
//   2. The private G-buffer: from the seed until the eye's G-buffer ends (its depth copy DEF19B03/CB95394B, or the next
//      eye's shell), every hull-family G-buffer draw is issued again into private RT0-3, a private slot target at RT6
//      (cleared to -1, no slot) and a private depth. Then a full-screen filter pass clears RT0-3 and the depth wherever
//      the private slot names a record outside the own mask (neighbour ships), and a count pass counts those pixels.
//      The hull decals (BBE5/DB3E) are not re-issued.
//   3. The screen-space stages after it: the depth copy, the shadow mask (53211E8C with 7EAC7196, then B403F48C) and
//      every lighting draw (VS 7E38A6AA, 8289669D, 963B52C7, 24DE25E4, B121A79E, 0357BBB2, any PS). Every bound target
//      is given a private counterpart, and every PS resource whose texture has one is swapped for it, so the stage
//      reads the private G-buffer, depth, depth copy and shadow mask. A draw that writes the eye's HDR between the
//      shadow mask and the sky (F8FA801F) with another VS is logged as an unknown lighting pair.
//   4. On the frame an eye dump arms (hullLayerArmDump; the hotkey, or the layer's own auto dump), the private HDR and
//      the game's HDR after the eye's last lighting draw, and the private depth after the filter, go to
//      edvr_logs\eyes\eye_<stamp>_HullHdr.bin, _GameHdrLit.bin and _HullDepth.bin (eye 1: _e1), EDVRTEX1.
//
// Step 2 (diag.hull_layer = separate; "Step 2 build"): everything above, and the own ship is taken out of the game's main
// G-buffer, so the terrain behind it is drawn. THE OWN SHIP IS THEN NOT SHOWN until the composite (Step 3) lands.
//   5. The seed runs before the seed draw itself is issued (hullLayerBeforeMainDraw), so the own mask exists for it.
//   6. From the seed to the end of the eye's G-buffer, every hull-family draw and hull decal (VS BBE5) that engine motion
//      substituted is issued with the own-discard variant of its patched pixel shader (engineVelocityOwnDiscardPs), with
//      the eye's own mask at PS t127: a pixel whose record is own is discarded. The patched shader and the game's t127 go
//      back right after the game's issue (hullLayerAfterMainDraw), before the private re-issue, which draws normally.
//      No seed, no slot target, no variant: the draw is issued as the game made it (the ship stays).
//   7. The own decals are re-issued into the private layer too (filtered like the hull), when they export the slot.
//   8. The dump adds the game's G-buffer RT0 and main depth after the eye's G-buffer (_GameGbuf0, _GameDepth).
//   The own forward draws after the lighting (VS 88DCF116, 025B4B9F, 0B5981F2, F464E27C, 6DB587D2, 2B3F53DD) are not
//   engine-motion families: they still draw into the main pass (an open item of the doc).
//
// Cost with the key off: one bool load per draw (hullLayerWanted). VR owner context only.
#pragma once

#include <d3d11.h>

#include <cstdint>

namespace edvr {

class Config;

namespace detail {
extern bool g_hullLayerOn;
extern bool g_hullLayerSeparate;
}  // namespace detail

// The draw path's one load.
inline bool hullLayerWanted() { return detail::g_hullLayerOn; }
// diag.hull_layer = separate: the draw path's pre-draw load (only read while hullLayerWanted).
inline bool hullLayerSeparateWanted() { return detail::g_hullLayerSeparate; }

// diag.hull_layer = off | dump | separate. Live; a change is said once.
void hullLayerConfigure(Config& cfg);

// Separate mode, before the game's own issue of an owner-context draw (after engine motion's substitution), with the
// draw's instance count and StartInstanceLocation: at the eye's seed draw the own mask is made; inside the eye's G-buffer
// window a hull-family or hull decal draw gets the own-discard variant and the mask at PS t127. True: the caller calls
// hullLayerAfterMainDraw right after the game's issue (and before hullLayerBegin), which puts the shader and t127 back.
bool hullLayerBeforeMainDraw(ID3D11DeviceContext* ctx, UINT instances, UINT startInstance);
void hullLayerAfterMainDraw(ID3D11DeviceContext* ctx);

// Right after the game's own issue of an owner-context draw, with the draw's bindings still bound, and the draw's
// instance count and StartInstanceLocation (1 and 0 for a draw that is not instanced). True: the private targets and
// resources are bound and the caller issues the same draw once more (raw), then calls hullLayerEnd, which puts the
// game's bindings back. False: nothing was bound (the layer may have run its own passes, with the game's state put back).
bool hullLayerBegin(ID3D11DeviceContext* ctx, UINT instances, UINT startInstance);
void hullLayerEnd(ID3D11DeviceContext* ctx);

// From the clear hooks (owner context, the game's own clears): a clear of a target that has a private counterpart is
// made on the counterpart too, and the value an eye depth is cleared to is remembered for the private depth's clear.
void hullLayerNoteClearRtv(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* rtv, const float colour[4]);
void hullLayerNoteClearDsv(ID3D11DeviceContext* ctx, ID3D11DepthStencilView* dsv, UINT flags, float depth, UINT8 stencil);

// The eye dump's arming (temporal_pass.cpp beginEyeRun): the next whole frame is captured. A no-op with the key off.
void hullLayerArmDump(const wchar_t* stamp);

// Once per frame on the owner thread: the per-eye bookkeeping, the readbacks, the dump's write, the auto dump, the 30 s line.
void hullLayerFrameBoundary(ID3D11DeviceContext* ctx);

void hullLayerShutdown();

}  // namespace edvr
