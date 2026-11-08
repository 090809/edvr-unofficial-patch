// The own hull as a private layer, Step 1: a lit copy of the own ship's G-buffer, written into the eye dump only
// (diag.hull_layer, TEMPORARY; docs\research-own-hull-layer-2026-11-08.md "Step 1 build"). Nothing the game draws or
// presents changes: every draw below is issued once more, after the game's own issue, into EDVR's private targets,
// and the game's bindings go back before the next game call.
//
// Per eye, inside the eye pass:
//   1. The own-ship block: the run of hull-family G-buffer draws (VS 66DE, 61AE, AACF, DE54) that starts at the first
//      hull draw after the cockpit shell (VS BFE5) into that eye's depth. It continues across at most kMaxGap
//      non-hull pool draws (VS t33 bound) and ends at the first non-pool draw, the terrain (72BDD292 / 6C320453...) or a
//      longer gap. Each block draw is re-issued into private RT0-3 and a private depth, cleared at the block's first
//      draw (colour 0; depth and stencil to the values the game last cleared that eye's depth to). The hull decals
//      (BBE5/DB3E) are not re-issued in Step 1.
//   2. Three screen-space stages, each once per eye after the block: the depth copy (DEF19B03/CB95394B), the shadow
//      mask (53211E8C with 7EAC7196, then B403F48C) and the sun and ambient light (7E38A6AA/7CECABDE). Every bound
//      target is given a private counterpart, and every PS resource whose texture has one is swapped for it (by the
//      texture's identity), so the stage reads the private G-buffer, depth, depth copy and shadow mask.
//   3. On the frame an eye dump arms (hullLayerArmDump), the private HDR after the private sun draw, the game's HDR
//      right after the game's own sun draw, and the private depth go to edvr_logs\eyes\eye_<stamp>_HullHdr.bin,
//      _GameHdrSun.bin and _HullDepth.bin (eye 1: the same names with _e1), EDVRTEX1 (tools\eye_inputs.py).
//
// Cost with the key off: one bool load per draw (hullLayerWanted). VR owner context only.
#pragma once

#include <d3d11.h>

#include <cstdint>

namespace edvr {

class Config;

namespace detail {
extern bool g_hullLayerOn;
}  // namespace detail

// The draw path's one load.
inline bool hullLayerWanted() { return detail::g_hullLayerOn; }

// diag.hull_layer = off | dump. Live; a change is said once.
void hullLayerConfigure(Config& cfg);

// Right after the game's own issue of an owner-context draw, with the draw's bindings still bound. True: the private
// targets and resources are bound and the caller issues the same draw once more (raw), then calls hullLayerEnd, which
// puts the game's bindings back. False: nothing was bound.
bool hullLayerBegin(ID3D11DeviceContext* ctx);
void hullLayerEnd(ID3D11DeviceContext* ctx);

// From the clear hooks (owner context, the game's own clears): a clear of a target that has a private counterpart is
// made on the counterpart too, and the value an eye depth is cleared to is remembered for the block's clear.
void hullLayerNoteClearRtv(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* rtv, const float colour[4]);
void hullLayerNoteClearDsv(ID3D11DeviceContext* ctx, ID3D11DepthStencilView* dsv, UINT flags, float depth, UINT8 stencil);

// The eye dump's arming (temporal_pass.cpp beginEyeRun): the next whole frame is captured. A no-op with the key off.
void hullLayerArmDump(const wchar_t* stamp);

// Once per frame on the owner thread: the per-eye bookkeeping, the dump's write, the 30 s line.
void hullLayerFrameBoundary(ID3D11DeviceContext* ctx);

void hullLayerShutdown();

}  // namespace edvr
