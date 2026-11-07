#pragma once
// The frame-generation synthesizer (fix.frame_generation_shadow = on, TEMPORARY, hidden, VR only; docs/design-frame-generation-2026-10-26.md, Phase 2.1, "The synthesis pass").
// Per eye and real frame B it builds S, the frame one display period later, at output size, from B's upscaled output, depth and DLSS vectors and the head's own step; nothing
// is shown in the headset and the frame loop does not change. It runs under the shadow scorer's key (no key of its own) and is called beside it. The pure halves are
// src\common\framegen_synth_math.h (the geometry and the constant block) and framegen_synth_report.h (the statistics and the lines); the shaders are framegen_synth_shader.h.
//
// Every other real frame of an eye builds the f = 0.5 S (a later phase shows it: Elite's step is two display periods); the frames between build the f = 1 S, which lands on the
// next real frame C's time. That one is reduced to the scorer's quarter-grid luma, kept with B's luma and class, and scored against C on C's frame: the error per class of B, with
// the no-warp comparison (B against C), read back without waiting. Key off: nothing is created and nothing is dispatched.
// frameGenSynthFrameBoundary (once a frame, from temporalPassFrameBoundary) prints the one-second report.
#include <d3d11.h>

#include <cstdint>

#include "../common/framegen_synth_math.h"

namespace edvr {

// The key (the shadow scorer's, passed on by the caller). Live: turning it off re-arms the line and the next boundary releases the resources.
void frameGenSynthConfigure(bool on);
bool frameGenSynthOn();

// What the temporal pass knew about one eye-frame. Pointers are the pass's own and are not kept.
struct FrameGenSynthFrame {
    uint32_t renderW = 0, renderH = 0, outW = 0, outH = 0;
    ID3D11ShaderResourceView* outSrv = nullptr;     // the upscaler's output, before the interface (dlOutSrv)
    ID3D11ShaderResourceView* mvSrv = nullptr;      // the DLSS motion vectors (dlMvSrv): R16G16_FLOAT, render size, pixels, this frame -> previous, jitter-free
    ID3D11ShaderResourceView* depthSrv = nullptr;   // this frame's depth copy (dlDepthSrv): Elite's reversed-Z, depth = A + B / metres
    const float* tanNow = nullptr;                  // this frame's tangents, jitter excluded (left, right, bottom, top)
    float depthA = 0.0f, depthB = 0.0f;             // p.knobs[0], p.knobs[2]
    float splitMetres = 0.0f;                       // p.split[0]
    FrameGenDeltas deltas;                          // only the head delta (Hd, th) is read
    bool depthWritten = false;                      // this frame wrote a depth copy (zcWritten)
    bool cameraJump = false;                        // the camera gate dropped or carried this frame's rows, or the origin jumped: built, not scored
    bool wantShown = false;                         // the eye dump is open: build the f = 0.5 S on this frame whatever the alternation says
};

// Once per treated eye-frame (usedDlaa, key on). `frame` is the pass's frame counter: the f = 1 S is scored only against the frame right after it.
void frameGenSynthEyeFrame(ID3D11Device* dev, ID3D11DeviceContext* ctx, uint32_t eye, uint32_t frame, const FrameGenSynthFrame& f);

// The output-size S texture (the upscaler's format) the call for `frame` just built at f = 0.5, else null: valid until the next call for the eye.
ID3D11Texture2D* frameGenSynthShown(uint32_t eye, uint32_t frame);

// Once a frame: the one-second report when the key is on, the release of the resources when it went off.
void frameGenSynthFrameBoundary();

// For the WARP rig (tools/framegen_synth_test): whether any shader, buffer or texture exists, and a copy of the open one-second window.
bool frameGenSynthResident();
FrameGenSynthWindow frameGenSynthOpenWindow();

}  // namespace edvr
