// TEMPORARY (docs\concourse-cull-2026-11-05.md): the concourse camera test, version 2. Flight 20 refuted a write to RenderView+0x310 (the game rebuilds the eye cameras every frame
// and cb1 comes from another copy). This version hooks the camera builders on build 334925 (concourse_camera_test.cpp labels each offset): 0x283FA20 is the write point (the pose
// that goes into the eye builder), 0x2874760 names who hands view 0 its camera (the call site), and 0x1EA9C0, 0x2845730 are counted. Flat only (the caller refuses the VR profile),
// installed only when hotkey.concourse_camera_test is bound; the relay gate is shut while the mode is 0, and the eye stub returns at once then, so unbound costs nothing.
// Remove with the concourse arc.
#pragma once

#include <cstdint>

namespace edvr {

struct ConcourseCameraTestSnap {
    static constexpr int kCalls = 4;   // 0x1EA9C0, 0x2874760, 0x2845730, 0x283FA20
    static constexpr int kHist = 8, kEyeSites = 4;
    uint32_t hookMask = 0;             // bit i: hook i is installed (the order above)
    uint64_t calls[kCalls] = {};
    uint64_t eyeWritten = 0, eyeRefused = 0, noView0 = 0, multiView = 0, faults = 0;
    uint32_t refusedHow = 0;           // the last refusal: 1 null pose, 2 not a local copy (the persistent second composition), 3 not finite
    uint32_t rvCount = 0, mainCount = 0, flags = 0, slotFlags = 0, slotIndex = 0, passLast = 0xFFFFFFFFu, passMask = 0, mainRet = 0;
    bool slotKnown = false, mailKnown = false, mailIdentity = false;
    float rvPos[3] = {}, slot[3] = {}, mailT[3] = {}, eyeBefore[16] = {}, eyeAfter[16] = {};
    uint32_t histRet[kHist] = {};
    uint64_t histN[kHist] = {};
    uint32_t eyeSiteRet[kEyeSites] = {};   // who calls 0x283FA20, by call site
    uint64_t eyeSiteN[kEyeSites] = {};
};

bool concourseCameraTestInstall(const char** why) noexcept;             // true when at least one hook is in; else false, and why (one sentence)
void concourseCameraTestControl(uint32_t mode, bool acting) noexcept;   // mode 0 off, 1 log only, 2 lift, 3 turn; acting: the pose may be written now
bool concourseCameraTestHooked() noexcept;
void concourseCameraTestSnapshot(ConcourseCameraTestSnap* out) noexcept;

}  // namespace edvr
