#pragma once
namespace edvr {
// Present thread: arm the pinned camera wrappers, publish the native on-foot
// gate and drain bounded default diagnostics. No configuration key of its own.
void nativePreviewCameraBoundary(bool wanted,bool active);
void nativePreviewCameraStop() noexcept;
// TEMPORARY (the concourse cull's VR proving build, hotkey.concourse_vr_camera_probe; docs\concourse-cull-2026-11-05.md): a second client of the eye stub on 0x283FA20.
// Arm: for `frames` consecutive present-thread frames after `frame` (the press's own), the first call of the stub in each frame raises the head pose's translation by
// `offset` (metres). Installs the stub on demand when the ship preview has not. False with `why` when it cannot (not installed, already running, a bad count).
bool nativeCameraProbeArm(const float offset[3],unsigned frames,unsigned frame,const char** why);
// The present thread, once a frame while the probe runs: logs the window that ended, opens the next.
void nativeCameraProbeTick(unsigned frame);
bool nativeCameraProbeRunning() noexcept;
// For the tone draw's lines: 0 outside the probe, 1 the press's own frame (before), 2 a frame that writes, 3 the frame after. Wrote: whether that frame's first call was written.
unsigned nativeCameraProbePhase(unsigned frame) noexcept;
bool nativeCameraProbeWrote(unsigned frame) noexcept;
}
