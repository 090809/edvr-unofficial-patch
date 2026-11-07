// Frame generation, Phase 2.3 (fix.frame_generation = on): Lock 2:1, the pure state machine and its slot schedule (src/common/framegen_lock_report.h,
// src/openxr/framegen_lock.h, FrameBoundary::syntheticSlot) against this rig's fake runtime. Included by openxr_frame_test.cpp after its Fake and Sink.
//
// LockDriver is the host's pair loop cut to what the schedule needs, in native_runtime_host.h's order: at the head of WaitGetPoses (before any wait) frameGenLockBeforeWait
// (the gate and, engaged, the S slot), then the game's own waitAndBegin, then the pair. With wired=false it is today's loop with no Lock code at all: the key-off identity
// is that the wired loop with the key off makes exactly the same XR calls in the same order, and hands the game exactly the same times and periods.
#include "../../src/openxr/framegen_lock.h"

namespace lockcases {
using namespace edvr;
using namespace edvr::openxr;

struct SlotSink : Sink {
  explicit SlotSink(Fake& f) : Sink(f) {}
  unsigned slotComposes = 0;
  bool sReady = true;            // the host's test: S consumed for B, posed, a format it can show
  unsigned fromS = 0, fromB = 0;
  XrResult slotResult = XR_SUCCESS;
  XrResult composeSlot(XrCompositionLayerProjection& layer) override {
    ++slotComposes;
    fake.trace.push_back('S');
    if (sReady) ++fromS; else ++fromB;   // never skipped: S, else B replayed with B's views
    layer.space = reinterpret_cast<XrSpace>(3); layer.viewCount = 2;
    static XrCompositionLayerProjectionView views[2] = {{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}, {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
    layer.views = views; return slotResult;
  }
};

struct Handed { XrTime time = 0; XrDuration period = 0; bool engaged = false; };

struct LockDriver {
  Fake& fake; SessionState& session; SlotSink& sink; FrameBoundary& boundary;
  bool wired = true;
  FrameGenLockGate gate;
  FrameGenLockConditions cond;
  uint64_t nowUs = 0, stepUs = 22222;    // a 45 fps game
  std::vector<Handed> handed;
  unsigned edges = 0, failEdges = 0, slotsRan = 0;
  uint64_t lateReal = 0;
  vr::Texture_t texture{};
  // One game frame: the pair boundary, the game's wait, and (unless the game drops it) both eyes. Returns the game's wait result.
  XrResult frame(bool submit = true) {
    nowUs += stepUs;
    FrameGenLockPair p;
    if (wired && cond.keyOn) {   // the host's own key test: off, nothing below is reached
      p = frameGenLockBeforeWait(boundary, gate, cond, nowUs);
      if (p.step.edge) ++edges;
      if (p.failEdge) ++failEdges;
      if (p.slotRan) ++slotsRan;
    }
    const XrResult r = boundary.waitAndBegin();
    if (p.slotRan && r == XR_SUCCESS) lateReal += boundary.lateFrames() - p.slot.lateBefore;
    if (r != XR_SUCCESS) return r;
    handed.push_back({boundary.frame().predictedDisplayTime, boundary.frame().predictedDisplayPeriod, gate.engaged()});
    if (submit) {
      boundary.setGeometryReady(true);
      boundary.submit(vr::Eye_Left, &texture);
      boundary.submit(vr::Eye_Right, &texture);
    }
    return r;
  }
};

FrameGenLockConditions allHold() {
  FrameGenLockConditions c;
  c.keyOn = c.channel = c.separate = c.projection = c.replay = c.sDelivering = c.notDeferred = c.periodDisplay = c.finishDone = true;
  return c;
}

// Predicted times strictly increase, every period handed is the runtime's own, and each step is P while disengaged and 2P while engaged, unless the runtime itself skipped.
bool monotonic(const std::vector<Handed>& h) {
  for (size_t i = 1; i < h.size(); ++i) if (h[i].time <= h[i - 1].time) return false;
  return true;
}
bool periodsAreRuntimes(const std::vector<Handed>& h, XrDuration p) {
  for (const auto& x : h) if (x.period != p) return false;
  return true;
}

// The gate alone, table-driven: the dwell, each reason in its order, a disengage at once, the dwell again after it, fail().
void gateTable() {
  FrameGenLockGate g;
  FrameGenLockConditions c = allHold();
  auto s = g.step(c, 1000);
  check(!s.engaged && !s.edge && s.reason == FrameGenLockReason::Dwell, "lock gate: the conditions just began to hold: dwell, not engaged");
  s = g.step(c, 1000 + kFrameGenLockDwellUs - 1);
  check(!s.engaged && s.reason == FrameGenLockReason::Dwell, "lock gate: 1 us short of the dwell: still not engaged");
  s = g.step(c, 1000 + kFrameGenLockDwellUs);
  check(s.engaged && s.edge && s.reason == FrameGenLockReason::None, "lock gate: held for 1 s: engaged, an edge");
  s = g.step(c, 2000000);
  check(s.engaged && !s.edge, "lock gate: engaged and holding: no edge");
  struct Row { bool FrameGenLockConditions::*field; FrameGenLockReason reason; };
  const Row rows[] = {{&FrameGenLockConditions::keyOn, FrameGenLockReason::KeyOff}, {&FrameGenLockConditions::channel, FrameGenLockReason::NoChannel},
                      {&FrameGenLockConditions::separate, FrameGenLockReason::NotSeparate}, {&FrameGenLockConditions::projection, FrameGenLockReason::NoProjection},
                      {&FrameGenLockConditions::replay, FrameGenLockReason::NoReplay}, {&FrameGenLockConditions::sDelivering, FrameGenLockReason::NoS},
                      {&FrameGenLockConditions::notDeferred, FrameGenLockReason::DeferredPacing}, {&FrameGenLockConditions::periodDisplay, FrameGenLockReason::PeriodHalved},
                      {&FrameGenLockConditions::finishDone, FrameGenLockReason::FinishPending}};
  uint64_t t = 3000000;
  for (const Row& r : rows) {
    FrameGenLockGate e;
    e.step(allHold(), t);
    e.step(allHold(), t + kFrameGenLockDwellUs);
    FrameGenLockConditions bad = allHold();
    bad.*(r.field) = false;
    const auto d = e.step(bad, t + kFrameGenLockDwellUs + 1);
    char text[160];
    std::snprintf(text, sizeof(text), "lock gate: %s fails: disengaged at once, an edge, said as that reason", frameGenLockReasonName(r.reason));
    check(!d.engaged && d.edge && d.reason == r.reason && frameGenLockBlocker(bad) == r.reason, text);
    const auto back = e.step(allHold(), t + kFrameGenLockDwellUs + 2);
    check(!back.engaged && back.reason == FrameGenLockReason::Dwell, "lock gate: ...and the conditions back again wait the dwell again");
  }
  FrameGenLockConditions two = allHold();
  two.projection = false; two.notDeferred = false;
  check(frameGenLockBlocker(two) == FrameGenLockReason::NoProjection, "lock gate: two failing: the first in order is said");
  FrameGenLockGate f;
  f.step(allHold(), 0); f.step(allHold(), kFrameGenLockDwellUs);
  check(f.fail(FrameGenLockReason::SlotFailed) && !f.engaged() && f.reason() == FrameGenLockReason::SlotFailed && !f.fail(FrameGenLockReason::SlotFailed),
        "lock gate: a failed slot disengages once (an edge), and a second fail is no edge");
  check(!f.step(allHold(), kFrameGenLockDwellUs + 5).engaged, "lock gate: ...and the dwell starts over after it");
  // The period test: P is the display's up to 1.5 P; a halved runtime (2P) is not; nothing known is not.
  check(frameGenLockPeriodIsDisplay(11111111, 11111111) && frameGenLockPeriodIsDisplay(16666666, 11111111) && !frameGenLockPeriodIsDisplay(22222222, 11111111) &&
            !frameGenLockPeriodIsDisplay(11111111, 0) && !frameGenLockPeriodIsDisplay(0, 11111111),
        "lock gate: the runtime's period is the display's up to 1.5 display periods; 2 is halved; unknown is not the display's");
  // S's delivery: none yet is not delivering; one consumed is; three misses still are; the fourth is not.
  FrameGenSDelivery dlv;
  check(!dlv.delivering(), "lock: no S consumed yet is not delivering");
  dlv.notePair(true);
  bool ok = dlv.delivering();
  for (int i = 0; i < 3; ++i) { dlv.notePair(false); ok = ok && dlv.delivering(); }
  dlv.notePair(false);
  check(ok && !dlv.delivering(), "lock: S delivered, then three pairs without S still deliver; the fourth in a row does not");
  dlv.notePair(false); dlv.notePair(true);
  check(dlv.delivering(), "lock: ...and one S again delivers");
  // Alternate pairs (the shadow key's f = 0.5 / f = 1 alternation before Lock engages) deliver.
  FrameGenSDelivery alt;
  bool altOk = true;
  for (int i = 0; i < 20; ++i) { alt.notePair((i & 1) == 0); altOk = altOk && alt.delivering(); }
  check(altOk, "lock: S on every other pair delivers");
}

// The slot time, the S-ready test and S's views.
void slotMath() {
  int64_t ts = 0;
  check(frameGenLockSlotTime(1000000000, 977777778, true, true, 11111111, ts) && ts == 1011111111, "lock slot time: engaged, T + P whatever the previous pair was");
  check(frameGenLockSlotTime(1000000000, 988888889, true, false, 11111111, ts) && ts == 1005555555, "lock slot time: not engaged, Phase 2.2's T + (T - T_A) / 2");
  check(frameGenLockSlotTime(1000000000, 0, false, true, 11111111, ts) && ts == 1011111111, "lock slot time: engaged needs no previous pair");
  check(!frameGenLockSlotTime(1000000000, 0, false, true, 60000000, ts) && ts == 0, "lock slot time: a period past 50 ms gives none");
  check(!frameGenLockSlotTime(1000000000, 0, false, false, 11111111, ts), "lock slot time: not engaged and no previous pair gives none");
  const uint64_t both[2] = {7, 7}, oneOld[2] = {7, 6};
  check(frameGenSReady(7, both, 7, true, true, true), "lock: S consumed for this pair on both eyes, posed: ready");
  check(!frameGenSReady(7, oneOld, 7, true, true, true) && !frameGenSReady(7, both, 6, true, true, true) && !frameGenSReady(7, both, 7, false, true, true) &&
            !frameGenSReady(7, both, 7, true, false, true) && !frameGenSReady(7, both, 7, true, true, false) && !frameGenSReady(0, both, 0, true, true, true),
        "lock: one eye's S of another pair, a pose of another pair, no pose, no head of B, a format it cannot show, or no pair: not ready (B is replayed)");
  // S's views: B's eyes 32 mm either side of a head at (0, 1.6, 0) looking down -z; the head at T_S has turned 5 degrees about y and moved 1 cm along +x.
  const double a = 5.0 * 3.14159265358979323846 / 180.0;
  XrPosef headB{{0, 0, 0, 1}, {0, 1.6f, 0}};
  XrPosef headS{{0, float(std::sin(a / 2)), 0, float(std::cos(a / 2))}, {0.01f, 1.6f, 0}};
  XrView eyeB[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
  eyeB[0].pose = {{0, 0, 0, 1}, {-0.032f, 1.6f, 0}}; eyeB[1].pose = {{0, 0, 0, 1}, {0.032f, 1.6f, 0}};
  eyeB[0].fov = {-0.9f, 0.8f, 0.85f, -0.95f}; eyeB[1].fov = {-0.8f, 0.9f, 0.85f, -0.95f};
  bool views = true;
  for (int e = 0; e < 2; ++e) {
    const XrView s = frameGenSlotView(eyeB[e], headB, headS);
    const double off = e == 0 ? -0.032 : 0.032;
    // Expected: the eye's offset turned by the head's turn, added to the head's new position; the eye's orientation the head's.
    const double ex = 0.01 + std::cos(a) * off, ez = -std::sin(a) * off;
    views = views && std::fabs(s.pose.position.x - ex) < 1e-6 && std::fabs(s.pose.position.y - 1.6) < 1e-6 && std::fabs(s.pose.position.z - ez) < 1e-6 &&
            std::fabs(s.pose.orientation.y - std::sin(a / 2)) < 1e-6 && std::fabs(s.pose.orientation.w - std::cos(a / 2)) < 1e-6 &&
            s.fov.angleLeft == eyeB[e].fov.angleLeft && s.fov.angleRight == eyeB[e].fov.angleRight && s.fov.angleUp == eyeB[e].fov.angleUp && s.fov.angleDown == eyeB[e].fov.angleDown;
  }
  check(views, "lock S views: each eye is B's eye carried by the head's step B -> S (its offset turned with the head), with B's fov");
  const XrView same = frameGenSlotView(eyeB[1], headB, headB);
  check(std::fabs(same.pose.position.x - 0.032) < 1e-6 && std::fabs(same.pose.orientation.w - 1.0) < 1e-6, "lock S views: no head motion leaves B's view");
  // The same math as the synthesizer's step (framegen_synth_math.h frameGenSlotStep is B's eye space into S's): from the rigid matrix the runtime hands the d3d11 half.
  float m[12] = {float(std::cos(a)), 0, float(std::sin(a)), 0.01f, 0, 1, 0, 1.6f, float(-std::sin(a)), 0, float(std::cos(a)), 0};
  const XrPosef fromM = frameGenPoseFromRigid(m);
  check(std::fabs(fromM.orientation.y - headS.orientation.y) < 1e-6 && std::fabs(fromM.orientation.w - headS.orientation.w) < 1e-6 && fromM.position.x == 0.01f,
        "lock S views: the head handed as a rigid 3x4 is the same pose");
  // A turn past 90 degrees about each axis (the other branches of the matrix-to-quaternion).
  bool branches = true;
  const double big = 170.0 * 3.14159265358979323846 / 180.0;
  for (int axis = 0; axis < 3; ++axis) {
    const double c = std::cos(big), s = std::sin(big);
    float r[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
    if (axis == 0) { r[5] = float(c); r[6] = float(-s); r[9] = float(s); r[10] = float(c); }
    if (axis == 1) { r[0] = float(c); r[2] = float(s); r[8] = float(-s); r[10] = float(c); }
    if (axis == 2) { r[0] = float(c); r[1] = float(-s); r[4] = float(s); r[5] = float(c); }
    const XrPosef q = frameGenPoseFromRigid(r);
    const float comp[3] = {q.orientation.x, q.orientation.y, q.orientation.z};
    branches = branches && std::fabs(std::fabs(comp[axis]) - std::sin(big / 2)) < 1e-5 && std::fabs(std::fabs(q.orientation.w) - std::cos(big / 2)) < 1e-5;
  }
  check(branches, "lock S views: a 170 degree turn about x, y or z converts to its quaternion");
}

// The schedule against the fake runtime.
void schedule() {
  const XrDuration P = 11;
  // Steady 2:1 after the dwell: each game frame is preceded by one S slot (W B S E), the game is told 2P steps, monotonic, its period P.
  {
    Fake fake; fake.step = fake.period = P; SessionState session; check(start(session, fake), "lock schedule: session starts");
    SlotSink sink(fake); FrameBoundary boundary(session, sink);
    LockDriver d{fake, session, sink, boundary};
    d.cond = allHold();
    for (int i = 0; i < 60; ++i) d.frame();   // 60 frames at 22.2 ms: engages after 1 s
    size_t firstEngaged = 0;
    while (firstEngaged < d.handed.size() && !d.handed[firstEngaged].engaged) ++firstEngaged;
    bool twoP = firstEngaged < d.handed.size();
    for (size_t i = firstEngaged + 1; i < d.handed.size(); ++i) twoP = twoP && d.handed[i].time - d.handed[i - 1].time == 2 * P;
    bool oneP = true;
    for (size_t i = 1; i < firstEngaged; ++i) oneP = oneP && d.handed[i].time - d.handed[i - 1].time == P;
    check(d.edges == 1 && firstEngaged == 46 && twoP && oneP, "lock schedule: steady 2:1: one engage edge once 1 s has held (frame 47 at 22.2 ms); before it P steps, after it 2P");
    check(monotonic(d.handed) && periodsAreRuntimes(d.handed, P), "lock schedule: steady: the game's times increase and every period it is told is the runtime's P");
    check(d.slotsRan == 14 && sink.slotComposes == 14 && sink.fromS == 14 && boundary.syntheticSlots() == 14 && fake.composeCalls == 60,
          "lock schedule: steady: one S slot per engaged game frame, each composed once, from S");
    const std::string tail(fake.trace.end() - 7, fake.trace.end());
    check(tail == "WBSEWBE", "lock schedule: steady: the order is the S slot's wait, begin, compose, end, then the game's wait, begin and end");
    check(d.lateReal == 0 && boundary.lateFrames() == 0, "lock schedule: steady: no late frame counted");
    session.abandonAfterOwnerDestruction();
  }
  // Late S: S is missing on some pairs; the slot still runs and shows B (never skipped); the game's steps stay 2P.
  {
    Fake fake; fake.step = fake.period = P; SessionState session; start(session, fake);
    SlotSink sink(fake); FrameBoundary boundary(session, sink);
    LockDriver d{fake, session, sink, boundary};
    d.cond = allHold();
    for (int i = 0; i < 46; ++i) d.frame();
    const auto before = d.handed.size();
    for (int i = 0; i < 10; ++i) { sink.sReady = (i % 3) != 0; d.frame(); }
    bool twoP = true;
    for (size_t i = before; i < d.handed.size(); ++i) twoP = twoP && d.handed[i].time - d.handed[i - 1].time == 2 * P;
    check(twoP && sink.fromB == 4 && d.slotsRan == 10 && d.edges == 1, "lock schedule: late S: the slot still runs and replays B (4 of 10), the game still told 2P, no disengage");
    session.abandonAfterOwnerDestruction();
  }
  // Late real: the game misses; the S slot's wait comes back a period late and the boundary counts it; the game's time follows it, never runs back.
  {
    Fake fake; fake.step = fake.period = P; SessionState session; start(session, fake);
    SlotSink sink(fake); FrameBoundary boundary(session, sink);
    LockDriver d{fake, session, sink, boundary};
    d.cond = allHold();
    for (int i = 0; i < 46; ++i) d.frame();
    fake.step = 2 * P;   // this frame's waits (the S slot's, then the game's) each advance the runtime two slots: the game's wait and the next S slot's each come back one late
    d.frame();
    fake.step = P;
    for (int i = 0; i < 3; ++i) d.frame();
    check(d.lateReal == 2 && boundary.lateFrames() == 2 && monotonic(d.handed), "lock schedule: late real: the two periods the runtime skipped are counted once each (late_real), and the times stay monotonic");
    check(d.handed.back().time - d.handed[d.handed.size() - 2].time == 2 * P, "lock schedule: late real: the next pairs are 2P again (no catch-up)");
    session.abandonAfterOwnerDestruction();
  }
  // A cut (a menu or a load: no projection), deferred pacing arriving, the runtime halving its period: each disengages at the pair boundary; the game's next step is P, not 2P.
  struct Cut { bool FrameGenLockConditions::*field; const char* name; };
  const Cut cuts[] = {{&FrameGenLockConditions::projection, "a cut (menu or load)"}, {&FrameGenLockConditions::notDeferred, "deferred pacing arriving"},
                      {&FrameGenLockConditions::periodDisplay, "the runtime halving its period"}, {&FrameGenLockConditions::sDelivering, "S stopping"}};
  for (const Cut& c : cuts) {
    Fake fake; fake.step = fake.period = P; SessionState session; start(session, fake);
    SlotSink sink(fake); FrameBoundary boundary(session, sink);
    LockDriver d{fake, session, sink, boundary};
    d.cond = allHold();
    for (int i = 0; i < 50; ++i) d.frame();
    const unsigned slots = d.slotsRan;
    d.cond.*(c.field) = false;
    d.frame();
    char text[200];
    std::snprintf(text, sizeof(text), "lock schedule: %s: disengaged at that pair boundary with no S slot, and the game's next time is one P on (fail-open never doubles)", c.name);
    const auto n = d.handed.size();
    check(!d.handed[n - 1].engaged && d.slotsRan == slots && d.edges == 2 && d.handed[n - 1].time - d.handed[n - 2].time == P && periodsAreRuntimes(d.handed, P), text);
    d.cond.*(c.field) = true;
    for (int i = 0; i < 20; ++i) d.frame();
    std::snprintf(text, sizeof(text), "lock schedule: %s: back, it waits the dwell again (no slot within 1 s)", c.name);
    check(d.slotsRan == slots && !d.gate.engaged(), text);
    for (int i = 0; i < 30; ++i) d.frame();
    std::snprintf(text, sizeof(text), "lock schedule: %s: ...and engages again after it", c.name);
    check(d.gate.engaged() && d.slotsRan > slots && d.edges == 3 && monotonic(d.handed), text);
    session.abandonAfterOwnerDestruction();
  }
  // A frame left open (one eye submitted, then the game waits again): the slot is refused with no XR call; Lock lets go (slot_failed); the game's wait closes the open frame
  // as it always does.
  {
    Fake fake; fake.step = fake.period = P; SessionState session; start(session, fake);
    SlotSink sink(fake); FrameBoundary boundary(session, sink);
    LockDriver d{fake, session, sink, boundary};
    d.cond = allHold();
    for (int i = 0; i < 50; ++i) d.frame();
    d.frame(false);
    boundary.setGeometryReady(true); boundary.submit(vr::Eye_Left, &d.texture);   // one eye only
    const auto waits = fake.waitCalls;
    d.frame();
    check(d.failEdges == 1 && !d.gate.engaged() && d.gate.reason() == FrameGenLockReason::SlotFailed && fake.waitCalls == waits + 1 && !boundary.failed(),
          "lock schedule: a frame left open: the S slot is refused with no wait of its own, Lock lets go (slot_failed), the game's wait runs as always");
    session.abandonAfterOwnerDestruction();
  }
  // The runtime failing the S slot's wait fails the boundary, as the game's own wait does.
  {
    Fake fake; fake.step = fake.period = P; SessionState session; start(session, fake);
    SlotSink sink(fake); FrameBoundary boundary(session, sink);
    LockDriver d{fake, session, sink, boundary};
    d.cond = allHold();
    for (int i = 0; i < 50; ++i) d.frame();
    fake.waits.push_back(XR_ERROR_RUNTIME_FAILURE);
    const XrResult r = d.frame();
    check(boundary.failed() && r == XR_ERROR_RUNTIME_FAILURE && d.failEdges == 1, "lock schedule: a failed S slot wait fails the boundary (as the game's wait would) and Lock lets go");
    session.abandonAfterOwnerDestruction();
  }
  // A slot whose layer the host cannot make (composeSlot unsupported) ends with no layer and is not a failure.
  {
    Fake fake; fake.step = fake.period = P; SessionState session; start(session, fake);
    SlotSink sink(fake); FrameBoundary boundary(session, sink);
    sink.slotResult = XR_ERROR_FUNCTION_UNSUPPORTED;
    SyntheticSlotInfo info;
    check(boundary.syntheticSlot(info) == XR_SUCCESS && !info.layers && !fake.lastHadLayers && fake.endCalls == 1 && !boundary.failed(),
          "lock schedule: a slot with no layer to show ends empty and the boundary stands");
    sink.slotResult = XR_ERROR_RUNTIME_FAILURE;
    check(boundary.syntheticSlot(info) == XR_ERROR_RUNTIME_FAILURE && boundary.failed(), "lock schedule: a slot whose compose fails fails the boundary, as compose() does");
    session.abandonAfterOwnerDestruction();
  }
  // frame_end_overlap: the previous pair's finish is deferred past Submit (publish then finishPair later); the S slot is refused while that frame is open and runs after it.
  {
    Fake fake; fake.step = fake.period = P; SessionState session; start(session, fake);
    SlotSink sink(fake); FrameBoundary boundary(session, sink);
    check(boundary.waitAndBegin() == XR_SUCCESS, "lock overlap: game frame begins");
    boundary.setGeometryReady(true);
    vr::Texture_t t{};
    bool ready = false;
    boundary.publish(vr::Eye_Left, &t, nullptr, vr::Submit_Default, ready);
    boundary.publish(vr::Eye_Right, &t, nullptr, vr::Submit_Default, ready);
    SyntheticSlotInfo info;
    const auto waits = fake.waitCalls;
    check(ready && boundary.syntheticSlot(info) == XR_ERROR_CALL_ORDER_INVALID && fake.waitCalls == waits && !boundary.failed(),
          "lock overlap: with the pair's finish still queued the S slot is refused before any XR call");
    check(boundary.finishPair() == XR_SUCCESS && boundary.syntheticSlot(info) == XR_SUCCESS && info.layers && fake.waitCalls == waits + 1,
          "lock overlap: once the queued finish has run (the owner FIFO's order) the S slot runs");
    session.abandonAfterOwnerDestruction();
  }
  // A pending pacer wait (deferred pacing's leftover) refuses the slot with no XR call.
  {
    Fake fake; fake.step = fake.period = P; SessionState session; start(session, fake);
    SlotSink sink(fake);
    FramePacer pacer;
    pacer.bind(&Fake::wait, reinterpret_cast<XrSession>(2));
    FrameBoundary boundary(session, sink, &pacer);
    boundary.waitAndBegin(FramePacing::Deferred); boundary.setGeometryReady(true);
    vr::Texture_t t{};
    boundary.submit(vr::Eye_Left, &t); boundary.submit(vr::Eye_Right, &t);   // turbo kicks the next wait at finish
    SyntheticSlotInfo info;
    check(boundary.syntheticSlot(info) == XR_ERROR_CALL_ORDER_INVALID && !boundary.failed(), "lock pacer: a pacer wait outstanding refuses the S slot, the boundary standing");
    boundary.drain();
    session.abandonAfterOwnerDestruction();
    pacer.stop();
  }
}

// The key-off identity: the wired loop with the key off (and with the key on but never engaged) makes the same XR calls in the same order and hands the same times as the
// loop with no Lock code, through the same script (steady frames, a dropped pair, a runtime skip, no-render frames).
void keyOffIdentity() {
  struct Run { std::string trace; std::vector<XrTime> times; std::vector<XrDuration> periods; unsigned composes = 0; uint64_t late = 0; };
  const auto run = [](bool wired, bool keyOn, bool neverHolds) {
    Fake fake; fake.step = fake.period = 11; SessionState session; start(session, fake);
    SlotSink sink(fake); FrameBoundary boundary(session, sink);
    LockDriver d{fake, session, sink, boundary};
    d.wired = wired;
    d.cond = allHold();
    d.cond.keyOn = keyOn;
    if (neverHolds) d.cond.projection = false;
    for (int i = 0; i < 80; ++i) {
      if (i == 20) fake.step = 22;
      if (i == 21) fake.step = 11;
      fake.shouldRender = !(i >= 40 && i < 43);
      d.frame(i != 30);
    }
    Run r;
    r.trace.assign(fake.trace.begin(), fake.trace.end());
    for (const auto& h : d.handed) { r.times.push_back(h.time); r.periods.push_back(h.period); }
    r.composes = fake.composeCalls;
    r.late = boundary.lateFrames();
    session.abandonAfterOwnerDestruction();
    return r;
  };
  const Run today = run(false, false, false), keyOff = run(true, false, false), notHeld = run(true, true, true), engaged = run(true, true, false);
  check(today.trace == keyOff.trace && today.times == keyOff.times && today.periods == keyOff.periods && today.composes == keyOff.composes && today.late == keyOff.late,
        "lock key off: the same XR calls in the same order, the same times and periods handed, the same composes and late count as the loop with no Lock code");
  check(today.trace == notHeld.trace && today.times == notHeld.times && today.periods == notHeld.periods,
        "lock key on, conditions never holding: identical to today as well (nothing runs before the dwell)");
  check(engaged.trace != today.trace && engaged.trace.find('S') != std::string::npos, "lock key on and holding: the trace differs (the S slots), so the comparison above can see one");
}

// Mutations: a policy that breaks a stated property must fail the property's check. Each mutant is driven through the same checks the schedule relies on.
void mutations() {
  const XrDuration P = 11;
  // 1. Skipping the slot on a missing S (instead of replaying B) hands the game a one-P step while engaged: the 2P check sees it.
  {
    Fake fake; fake.step = fake.period = P; SessionState session; start(session, fake);
    SlotSink sink(fake); FrameBoundary boundary(session, sink);
    LockDriver d{fake, session, sink, boundary};
    d.cond = allHold();
    for (int i = 0; i < 46; ++i) d.frame();
    bool skipped = false;
    for (int i = 0; i < 6; ++i) {
      d.nowUs += d.stepUs;
      const bool sMissing = i == 3;
      if (!sMissing) { SyntheticSlotInfo info; boundary.syntheticSlot(info); } else skipped = true;   // the mutant
      boundary.waitAndBegin();
      d.handed.push_back({boundary.frame().predictedDisplayTime, boundary.frame().predictedDisplayPeriod, true});
      boundary.setGeometryReady(true); boundary.submit(vr::Eye_Left, &d.texture); boundary.submit(vr::Eye_Right, &d.texture);
    }
    bool twoP = true;
    for (size_t i = d.handed.size() - 5; i < d.handed.size(); ++i) twoP = twoP && d.handed[i].time - d.handed[i - 1].time == 2 * P;
    check(skipped && !twoP, "lock mutant (skip the slot on a missing S): the 2P check fails, as it must");
    session.abandonAfterOwnerDestruction();
  }
  // 2. A gate that ignores a cut (stays engaged through it) keeps telling the game 2P: the cut's P-step check sees it.
  {
    Fake fake; fake.step = fake.period = P; SessionState session; start(session, fake);
    SlotSink sink(fake); FrameBoundary boundary(session, sink);
    LockDriver d{fake, session, sink, boundary};
    d.cond = allHold();
    for (int i = 0; i < 50; ++i) d.frame();
    d.frame();   // the mutant: the cut is not passed to the gate (d.cond stays allHold)
    const auto n = d.handed.size();
    check(d.handed[n - 1].time - d.handed[n - 2].time != P, "lock mutant (a gate that ignores a cut): the cut's one-P check fails, as it must");
    session.abandonAfterOwnerDestruction();
  }
  // 3. Telling the game the slot's own time (a synthesized T + P) instead of running its wait: the steady 2P check sees the one-P step.
  {
    Fake fake; fake.step = fake.period = P; SessionState session; start(session, fake);
    SlotSink sink(fake); FrameBoundary boundary(session, sink);
    LockDriver d{fake, session, sink, boundary};
    d.cond = allHold();
    for (int i = 0; i < 46; ++i) d.frame();
    SyntheticSlotInfo info;
    boundary.syntheticSlot(info);
    const XrTime told = info.predictedDisplayTime;   // the mutant hands this to the game
    check(told - d.handed.back().time != 2 * P, "lock mutant (the game told the S slot's time): the 2P check fails, as it must");
    session.abandonAfterOwnerDestruction();
  }  // 4. A boundary that let the slot's time go backwards would fail monotonic().
  {
    std::vector<Handed> h = {{100, P, true}, {122, P, true}, {111, P, true}};
    check(!monotonic(h), "lock mutant (a time running back): monotonic() fails, as it must");
  }
}

// The lines.
void lines() {
  FrameGenLockWindow w;
  w.engagedSeconds = 0.98; w.realSlots = 44; w.sSlots = 44; w.lateReal = 1; w.lateS = 2; w.fromS = 42; w.fromB = 2; w.disengages = 0;
  w.waitMs = {9.1f, 9.4f, 10.2f}; w.periodMs = {11.11f, 11.11f, 11.12f};
  check(formatFrameGenLockReport(w, FrameGenLockReason::None) ==
            "frame generation lock: engaged=0.98 slots=44/44 late_real=1 late_s=2 s_from=42/2 wait_ms=9.40/10.20 period_ms=11.11 disengages=0 reason=none",
        "lock line: the one-second line");
  FrameGenLockWindow idle;
  check(formatFrameGenLockReport(idle, FrameGenLockReason::NoS) ==
            "frame generation lock: engaged=0.00 slots=0/0 late_real=0 late_s=0 s_from=0/0 wait_ms=-/- period_ms=- disengages=0 reason=no_s",
        "lock line: never engaged: engaged=0.00 and the reason it is not");
  check(formatFrameGenLockEdge(true, FrameGenLockReason::None, 11.11) == "frame generation lock: engage reason=held_1s period_ms=11.11", "lock line: the engage edge");
  check(formatFrameGenLockEdge(false, FrameGenLockReason::NoProjection, 11.11) == "frame generation lock: disengage reason=no_projection period_ms=11.11",
        "lock line: a disengage edge");
  check(formatFrameGenLockEdge(false, FrameGenLockReason::DeferredPacing, 11.11) ==
            "frame generation lock: disengage reason=deferred_pacing period_ms=11.11 (Lock stands aside for deferred pacing in 2.3)",
        "lock line: deferred pacing's disengage says Lock stands aside");
  bool names = true;
  for (int r = 0; r < int(FrameGenLockReason::Count); ++r) names = names && std::strcmp(frameGenLockReasonName(FrameGenLockReason(r)), "unknown") != 0;
  check(names, "lock line: every reason has a name");
}

void run() { gateTable(); slotMath(); schedule(); keyOffIdentity(); mutations(); lines(); }

}  // namespace lockcases
