#pragma once
// Frame generation, Phase 2.3 (fix.frame_generation = on, TEMPORARY, hidden; docs/design-frame-generation-2026-10-26.md, "### Pacing: Lock and Fill"): Lock 2:1 at the runtime's
// pair boundary. Header-only and free of the host, so tools\openxr_frame_test drives exactly this against its fake runtime:
//   frameGenLockBeforeWait: at the head of WaitGetPoses (owner thread, before the game's own wait): the gate's step and, while engaged, the S slot through
//     FrameBoundary::syntheticSlot. A slot the boundary refuses (a frame open, a deferred wait) or that fails lets Lock go at once (slot_failed); the game's wait then runs as
//     it always does, so a disengage hands the game the next display slot, never a doubled one.
//   frameGenSlotViewPose: S's layer views. S was rendered for the head located at T_S (capture(), Phase 2.2) through the same eye-to-head as B, so each eye's pose is
//     head_S * head_B^-1 * eye_B, with B's fov (S is drawn with B's tangents); the compositor turns S by what remains.
// The key off: the host never calls in here (byte-for-byte today's frame loop).
#include "frame_boundary.h"
#include "seat_transform.h"
#include "../common/framegen_lock_report.h"

#include <openxr/openxr.h>

namespace edvr::openxr {

// What one pair boundary did.
struct FrameGenLockPair {
  FrameGenLockGate::Step step;      // the gate at this boundary (before any slot)
  bool slotRan = false;             // syntheticSlot was called
  XrResult slotResult = XR_SUCCESS;
  SyntheticSlotInfo slot;
  bool failEdge = false;            // the slot was refused or failed and Lock let go at this boundary
};

inline FrameGenLockPair frameGenLockBeforeWait(FrameBoundary& boundary, FrameGenLockGate& gate, const FrameGenLockConditions& conditions, uint64_t nowUs) {
  FrameGenLockPair p;
  p.step = gate.step(conditions, nowUs);
  if (!p.step.engaged) return p;
  p.slotRan = true;
  p.slotResult = boundary.syntheticSlot(p.slot);
  if (p.slotResult != XR_SUCCESS || boundary.failed()) p.failEdge = gate.fail(FrameGenLockReason::SlotFailed);
  return p;
}

// a * b for rigid poses: b's transform first, then a's.
inline XrPosef frameGenPoseMul(const XrPosef& a, const XrPosef& b) { return seatxf::toNatural(a, b); }
// a^-1 * b.
inline XrPosef frameGenPoseLeftDiv(const XrPosef& a, const XrPosef& b) {
  if (seatxf::isIdentity(a)) return b;
  return seatxf::toSeated(a, b);
}

// The pose of a rigid row-major 3x4 (an OpenVR HmdMatrix34 as the runtime builds it from an XrPosef, space_pose.h makeHeadPose).
inline XrPosef frameGenPoseFromRigid(const float m[12]) {
  const double r00 = m[0], r01 = m[1], r02 = m[2], r10 = m[4], r11 = m[5], r12 = m[6], r20 = m[8], r21 = m[9], r22 = m[10];
  double x, y, z, w;
  const double trace = r00 + r11 + r22;
  if (trace > 0.0) {
    const double s = std::sqrt(trace + 1.0) * 2.0;
    w = 0.25 * s; x = (r21 - r12) / s; y = (r02 - r20) / s; z = (r10 - r01) / s;
  } else if (r00 > r11 && r00 > r22) {
    const double s = std::sqrt(1.0 + r00 - r11 - r22) * 2.0;
    w = (r21 - r12) / s; x = 0.25 * s; y = (r01 + r10) / s; z = (r02 + r20) / s;
  } else if (r11 > r22) {
    const double s = std::sqrt(1.0 + r11 - r00 - r22) * 2.0;
    w = (r02 - r20) / s; x = (r01 + r10) / s; y = 0.25 * s; z = (r12 + r21) / s;
  } else {
    const double s = std::sqrt(1.0 + r22 - r00 - r11) * 2.0;
    w = (r10 - r01) / s; x = (r02 + r20) / s; y = (r12 + r21) / s; z = 0.25 * s;
  }
  XrPosef p;
  p.orientation = seatxf::toXr({x, y, z, w});
  p.position = {m[3], m[7], m[11]};
  return p;
}

// S's view for one eye: B's view turned by the head's step from B to S (head_S * head_B^-1 * eye_B), B's fov. All three poses in the same (seated) space.
inline XrView frameGenSlotView(const XrView& eyeB, const XrPosef& headB, const XrPosef& headS) {
  XrView v = eyeB;
  v.pose = frameGenPoseMul(headS, frameGenPoseLeftDiv(headB, eyeB.pose));
  return v;
}

}  // namespace edvr::openxr
