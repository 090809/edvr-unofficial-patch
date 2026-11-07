// TEMPORARY (docs\concourse-cull-2026-11-05.md): the concourse camera test, version 3. Flight 20 refuted a write to RenderView+0x310 (only the cull context's copy), and Flight 21 showed
// the VR eye path does not run in flat. This version moves flat's main camera where it is made: after 0x2874290, on 0x281D190's stack, before any copy is taken (concourse_camera_test.cpp
// labels each offset, build 334925). 0x1EA9C0 is read only, for view 0. Flat only (the caller refuses the VR profile), installed only when hotkey.concourse_camera_test is bound; the relay
// gate is shut while the mode is 0, so unbound costs nothing.
// Remove with the concourse arc.
#pragma once

#include <cstdint>

namespace edvr {

struct ConcourseCameraTestSnap {
    static constexpr int kCalls = 2;   // 0x1EA9C0, 0x2874290
    uint32_t hookMask = 0;             // bit i: hook i is installed (the order above)
    uint64_t calls[kCalls] = {};
    uint64_t written = 0, refused = 0, otherSite = 0, noView0 = 0, multiView = 0, faults = 0;
    uint32_t rvCount = 0, mainCount = 0, flags = 0, slotFlags = 0, slotIndex = 0;
    bool slotKnown = false;
    float rvPos[3] = {}, slot[3] = {}, camBefore[3] = {}, camAfter[3] = {}, fwdBefore[3] = {}, fwdAfter[3] = {};
};

bool concourseCameraTestInstall(const char** why) noexcept;             // true when at least one hook is in; else false, and why (one sentence)
void concourseCameraTestControl(uint32_t mode, bool acting) noexcept;   // mode 0 off, 1 log only, 2 lift, 3 turn; acting: the camera may be written now
bool concourseCameraTestHooked() noexcept;
void concourseCameraTestSnapshot(ConcourseCameraTestSnap* out) noexcept;

}  // namespace edvr
