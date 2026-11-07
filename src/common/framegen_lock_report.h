#pragma once
// Frame generation, Phase 2.3 (fix.frame_generation = on, TEMPORARY, hidden; docs/design-frame-generation-2026-10-26.md, "### Pacing: Lock and Fill"): Lock 2:1, the runtime's
// half. Elite's frame B is shown at slot T, the synthetic frame S_B at T + P, Elite's next frame C at T + 2P. The runtime runs the S slot (its own xrWaitFrame, xrBeginFrame and
// xrEndFrame) at the head of C's WaitGetPoses, before C's own wait, so Elite is told C's real predicted time.
// Pure: no OpenXR, no D3D, no clock. The gate (when Lock is engaged), the slot's time, S's delivery, and the one-second and edge lines, whose format strings tools\edvr_log.py
// --health reads from this file. The runtime's frame loop (src/openxr/native_runtime_host.h, framegen_lock.h) feeds it.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "framegen_slot_report.h"

namespace edvr {

// Why Lock is not engaged (or why it let go), in the order the gate tests them: the first that fails is the one said.
enum class FrameGenLockReason : uint8_t {
    None = 0,
    KeyOff,          // fix.frame_generation is off (or the d3d11.dll is too old to carry it)
    NoChannel,       // the d3d11 half has no slot channel (an older d3d11.dll): no S can cross
    NotSeparate,     // the XR device is the game's: the transfer needs two
    NoProjection,    // the last frame was not a fresh pair the runtime composed (a menu, a load, a flat clear, a withheld or failed frame, a frame still open)
    NoReplay,        // B is not kept for a replay (resubmit off, or the exchange failed): a missing S would have nothing to show
    Menu,            // Phase 2.3b: EDVR's own menu was drawn into the last pair (the runtime draws it into B only: S would blink it)
    Unfinished,      // Phase 2.3b: the last pair's S was not finished as B is (framegen_slot_report.h kFrameGenFinish*: the why is said on its own line)
    NoS,             // the synthesizer has not delivered an S for several pairs
    DeferredPacing,  // deferred pacing (turbo, fix.weapon_stability's on-foot deferral) is on this frame: Lock stands aside for it in 2.3
    PeriodHalved,    // the runtime's predicted period is already about twice the display's (the vendor halved the rate)
    FinishPending,   // the previous pair's overlapped finish has not run (the owner FIFO says it cannot be; a guard)
    SlotFailed,      // the S slot itself was refused or failed (no frame was begun, or the runtime returned an error)
    Dwell,           // every condition holds, for less than kFrameGenLockDwellUs so far
    Count
};

inline const char* frameGenLockReasonName(FrameGenLockReason r) {
    switch (r) {
        case FrameGenLockReason::None: return "none";
        case FrameGenLockReason::KeyOff: return "key_off";
        case FrameGenLockReason::NoChannel: return "no_channel";
        case FrameGenLockReason::NotSeparate: return "not_separate";
        case FrameGenLockReason::NoProjection: return "no_projection";
        case FrameGenLockReason::NoReplay: return "no_replay";
        case FrameGenLockReason::Menu: return "menu";
        case FrameGenLockReason::Unfinished: return "s_unfinished";
        case FrameGenLockReason::NoS: return "no_s";
        case FrameGenLockReason::DeferredPacing: return "deferred_pacing";
        case FrameGenLockReason::PeriodHalved: return "period_halved";
        case FrameGenLockReason::FinishPending: return "finish_pending";
        case FrameGenLockReason::SlotFailed: return "slot_failed";
        case FrameGenLockReason::Dwell: return "dwell";
        default: return "unknown";
    }
}

// What the runtime knows at a pair boundary (WaitGetPoses, before any wait). Each field is true when its condition HOLDS.
struct FrameGenLockConditions {
    bool keyOn = false;
    bool channel = false;
    bool separate = false;
    bool projection = false;
    bool replay = false;
    bool noMenu = true;       // Phase 2.3b: true unless EDVR's menu was drawn into the last pair (a host from before 2.3b never sets it)
    bool sFinished = true;    // Phase 2.3b: true unless the last pair's S was left unfinished (frameGenSFinished)
    bool sDelivering = false;
    bool notDeferred = false;
    bool periodDisplay = false;
    bool finishDone = false;
};

inline FrameGenLockReason frameGenLockBlocker(const FrameGenLockConditions& c) {
    if (!c.keyOn) return FrameGenLockReason::KeyOff;
    if (!c.channel) return FrameGenLockReason::NoChannel;
    if (!c.separate) return FrameGenLockReason::NotSeparate;
    if (!c.projection) return FrameGenLockReason::NoProjection;
    if (!c.replay) return FrameGenLockReason::NoReplay;
    if (!c.noMenu) return FrameGenLockReason::Menu;
    if (!c.sFinished) return FrameGenLockReason::Unfinished;
    if (!c.sDelivering) return FrameGenLockReason::NoS;
    if (!c.notDeferred) return FrameGenLockReason::DeferredPacing;
    if (!c.periodDisplay) return FrameGenLockReason::PeriodHalved;
    if (!c.finishDone) return FrameGenLockReason::FinishPending;
    return FrameGenLockReason::None;
}

// The runtime's predicted period is the display's: not above 1.5 display periods. displayNs is the display's period (from its refresh rate, else the shortest predicted period
// the session has seen); 0 (nothing known yet) is not the display's.
inline bool frameGenLockPeriodIsDisplay(int64_t periodNs, int64_t displayNs) {
    return periodNs > 0 && displayNs > 0 && 2 * periodNs <= 3 * displayNs;
}

constexpr uint64_t kFrameGenLockDwellUs = 1000000;   // the conditions hold this long before Lock engages (and again after any disengage)

// Engage and disengage, only ever at a pair boundary. Engaged while every condition holds; any failure disengages at once; it engages again once they have held for
// kFrameGenLockDwellUs. step() returns whether this boundary is an edge, and the reason: the blocker while one stands, Dwell while waiting, None while engaged.
class FrameGenLockGate {
public:
    struct Step {
        bool engaged = false;
        bool edge = false;   // engaged changed at this boundary
        FrameGenLockReason reason = FrameGenLockReason::None;
    };
    Step step(const FrameGenLockConditions& c, uint64_t nowUs) {
        Step s;
        const FrameGenLockReason blocker = frameGenLockBlocker(c);
        if (blocker != FrameGenLockReason::None) {
            holding_ = false;
            s.edge = engaged_;
            engaged_ = false;
            reason_ = blocker;
        } else if (!engaged_) {
            if (!holding_) {
                holding_ = true;
                holdSinceUs_ = nowUs;
            }
            if (nowUs >= holdSinceUs_ && nowUs - holdSinceUs_ >= kFrameGenLockDwellUs) {
                engaged_ = true;
                s.edge = true;
                reason_ = FrameGenLockReason::None;
            } else {
                reason_ = FrameGenLockReason::Dwell;
            }
        } else {
            reason_ = FrameGenLockReason::None;
        }
        s.engaged = engaged_;
        s.reason = reason_;
        return s;
    }
    // The S slot was refused or failed: disengaged at once, and the dwell starts over. True when this was an edge.
    bool fail(FrameGenLockReason why) {
        const bool edge = engaged_;
        engaged_ = false;
        holding_ = false;
        reason_ = why;
        return edge;
    }
    bool engaged() const { return engaged_; }
    FrameGenLockReason reason() const { return reason_; }
    void reset() { *this = FrameGenLockGate{}; }

private:
    bool engaged_ = false, holding_ = false;
    uint64_t holdSinceUs_ = 0;
    FrameGenLockReason reason_ = FrameGenLockReason::KeyOff;
};

// Whether the synthesizer is delivering S: an S was consumed for one of the last kFrameGenLockSMissLimit pairs. One missing S is a late S (B is replayed); a run of them
// disengages. Under the shadow key with Lock not engaged S is built on every other frame only, which still delivers.
constexpr uint32_t kFrameGenLockSMissLimit = 4;
struct FrameGenSDelivery {
    uint32_t missRun = kFrameGenLockSMissLimit;   // nothing delivered yet
    void notePair(bool sConsumed) { missRun = sConsumed ? 0 : (missRun < kFrameGenLockSMissLimit ? missRun + 1 : missRun); }
    bool delivering() const { return missRun < kFrameGenLockSMissLimit; }
};

// The slot S is made for. While Lock is engaged it is the next display slot, T + P (the slot the runtime will run S in); otherwise Phase 2.2's T + (T - T_A) / 2, which is the
// same T + P once Lock has held for a pair. Valid from 1 to 50 ms past T, as 2.2's.
inline bool frameGenLockSlotTime(int64_t displayTime, int64_t previousDisplayTime, bool havePrevious, bool engaged, int64_t periodNs, int64_t& slotTime) {
    if (engaged && periodNs > 0 && displayTime > 0) {
        slotTime = 0;
        if (periodNs < kFrameGenSlotMinLeadNs || periodNs > kFrameGenSlotMaxLeadNs) return false;
        slotTime = displayTime + periodNs;
        return true;
    }
    return frameGenSlotTime(displayTime, previousDisplayTime, havePrevious, slotTime);
}

// S can be shown in the slot after pair `pair`: both eyes' S consumed for that pair, its pose located for that pair, and B's own head known (S's views are B's turned by the
// head's step B -> S). Anything else replays B with B's views and counts late_s.
inline bool frameGenSReady(uint64_t pair, const uint64_t consumed[2], uint64_t poseFor, bool poseValid, bool headBValid, bool formatShown) {
    return pair != 0 && consumed[0] == pair && consumed[1] == pair && poseFor == pair && poseValid && headBValid && formatShown;
}

// Phase 2.3b: one eye's answer for a pair says its S was NOT finished as B is: handed over without the finish (a d3d11.dll from before 2.3b, finish 0), or held back with a reason
// (finish 2 and up). An S not built, or held back busy, says nothing about finishing. `why` is the finish word to name (kFrameGenFinish*).
inline bool frameGenSUnfinished(uint32_t state, uint32_t finish, uint32_t& why) {
    const bool produced = state == 1u;   // EDVR_NATIVE_FRAMEGEN_S_PRODUCED
    if ((produced && finish != kFrameGenFinishDone) || finish > kFrameGenFinishDone) {
        why = finish;
        return true;
    }
    return false;
}

// ---- the lines ----
// Said once when the runtime first reads the key on (and again after it was off).
constexpr const char* kFrameGenLockArmedFmt =
    "frame generation lock: armed (fix.frame_generation = on: Lock 2:1, forced; it engages once its conditions have held for 1 s and says each engage and disengage)";
// Each edge, at the pair boundary it happens at. period_ms: the runtime's last predicted period.
constexpr const char* kFrameGenLockEngageFmt = "frame generation lock: engage reason=held_1s period_ms=%.2f";
constexpr const char* kFrameGenLockDisengageFmt = "frame generation lock: disengage reason=%s period_ms=%.2f%s";
// The note a deferred-pacing disengage (or block) carries: in 2.3 Lock stands aside for it.
constexpr const char* kFrameGenLockDeferredNote = " (Lock stands aside for deferred pacing in 2.3)";
// The S slot could not show S because of its format (the stereo renderer takes 8-bit RGBA/BGRA only): said once; the slot replays B.
constexpr const char* kFrameGenLockFormatFmt = "frame generation lock: S's format %u cannot be shown (8-bit RGBA/BGRA only); the S slot replays B";
// Phase 2.3b: an S was left unfinished (Lock lets go with reason s_unfinished, or does not engage): said when the why changes from the last one said.
constexpr const char* kFrameGenLockUnfinishedFmt = "frame generation lock: S is not finished like B (why=%s); the S slot does not show it";
// Once a second while the key is on. engaged: the seconds of the window Lock was engaged; slots: the real pairs composed while engaged / the S slots run; late_real: the display
// periods the boundary counted missed across the engaged pairs (Elite's frame late); late_s: S slots that showed B again (S missing, not consumed, busy, no pose, or a format
// it cannot show); s_from: S slots that showed S / that replayed B; wait_ms: the S slot's xrWaitFrame, median/max; period_ms: the runtime's predicted period, median;
// disengages: the edges to off in the window; reason: why it is off at the line (none while engaged).
constexpr const char* kFrameGenLockReportFmt =
    "frame generation lock: engaged=%.2f slots=%u/%u late_real=%u late_s=%u s_from=%u/%u wait_ms=%s/%s period_ms=%s disengages=%u reason=%s";

struct FrameGenLockWindow {
    double engagedSeconds = 0.0;
    uint32_t realSlots = 0, sSlots = 0, lateReal = 0, lateS = 0, fromS = 0, fromB = 0, disengages = 0;
    std::vector<float> waitMs, periodMs;
    void reset() { *this = FrameGenLockWindow{}; }
};

inline std::string formatFrameGenLockReport(const FrameGenLockWindow& w, FrameGenLockReason reason) {
    char buf[512];
    const std::string w0 = frameGenSlotMs(w.waitMs, false), w1 = frameGenSlotMs(w.waitMs, true), p0 = frameGenSlotMs(w.periodMs, false);
    std::snprintf(buf, sizeof(buf), kFrameGenLockReportFmt, w.engagedSeconds, static_cast<unsigned>(w.realSlots), static_cast<unsigned>(w.sSlots), static_cast<unsigned>(w.lateReal),
                  static_cast<unsigned>(w.lateS), static_cast<unsigned>(w.fromS), static_cast<unsigned>(w.fromB), w0.c_str(), w1.c_str(), p0.c_str(), static_cast<unsigned>(w.disengages),
                  frameGenLockReasonName(reason));
    return buf;
}

inline std::string formatFrameGenLockEdge(bool engage, FrameGenLockReason reason, double periodMs) {
    char buf[256];
    if (engage)
        std::snprintf(buf, sizeof(buf), kFrameGenLockEngageFmt, periodMs);
    else
        std::snprintf(buf, sizeof(buf), kFrameGenLockDisengageFmt, frameGenLockReasonName(reason), periodMs,
                      reason == FrameGenLockReason::DeferredPacing ? kFrameGenLockDeferredNote : "");
    return buf;
}

}  // namespace edvr
