// TEMPORARY (docs\concourse-cull-2026-11-05.md): the concourse camera test. A hook on 0x1EA9C0 (the frame view builder) that can move view 0's camera for a frame, to see
// whether the engine's cull, draws, constant buffers and image follow. Build 334925 only (the offsets were read there; concourse_camera_test.cpp labels each), flat only
// (the caller refuses the VR profile). Installed only when hotkey.concourse_camera_test is bound; the relay's gate is shut while the mode is 0, so unbound costs nothing.
// Control is called every tick: that is what closes the gate after a held write has gone back, and reopens it if one came out just as the mode went to 0.
// Remove with the concourse arc.
#pragma once

#include <cstdint>

namespace edvr {

struct ConcourseCameraTestSnap {
    uint64_t calls = 0, acted = 0, noView0 = 0, multiView = 0, faults = 0;
    uint64_t heldPutBack = 0, heldRewritten = 0;   // held writes: still in the camera at the next call and put back; gone, because the game wrote the camera in between
    uint32_t rvCount = 0, mainCount = 0, flags = 0, slotFlags = 0, slotIndex = 0;
    bool slotKnown = false, wroteLast = false;
    float before[3] = {}, written[3] = {}, slot[3] = {};
};

bool concourseCameraTestInstall(const char** why) noexcept;             // true when hooked; else false, and why (one sentence)
void concourseCameraTestControl(uint32_t mode, bool acting) noexcept;   // mode 0 off, 1 log only, 2 lift, 3 turn, 4 lift held, 5 turn held; acting: the camera may be written
bool concourseCameraTestHooked() noexcept;
void concourseCameraTestSnapshot(ConcourseCameraTestSnap* out) noexcept;

}  // namespace edvr
