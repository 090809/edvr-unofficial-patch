#pragma once
// The frame-generation shadow scorer (fix.frame_generation_shadow = off | on, TEMPORARY, hidden, VR only; docs/design-frame-generation-2026-10-26.md,
// Phase 1). It measures how well the previous real frame, warped by the camera and head motion, predicts the next one; nothing in the headset changes.
// The pure half (statistics and the two log lines) is src\common\framegen_shadow_report.h.
//
// This step has the key and the armed line only: temporal_pass.cpp calls frameGenShadowEyeFrame once per treated eye-frame beside the sky ring, and the
// first call after the key goes on says "frame generation (shadow): armed, ...". Nothing is scored yet.
#include <cstdint>

namespace edvr {

// The key. Live: a reload that changes it takes effect at once; turning it off re-arms the line.
void frameGenShadowConfigure(bool on);
bool frameGenShadowOn();

// Once per treated eye-frame, with the eye's render size. A no-op with the key off.
void frameGenShadowEyeFrame(uint32_t eye, uint32_t renderW, uint32_t renderH);

}  // namespace edvr
