// framegen_synth_report_test: the pure half of the frame-generation synthesizer (src/common/framegen_synth_report.h and framegen_synth_math.h; fix.frame_generation_shadow,
// TEMPORARY, docs/design-frame-generation-2026-10-26.md Phase 2.1): the exact text of the armed and report lines, the window's merge, the head's step from B to S (the rotation
// raised to the power f, the translation), the constant block's layout, the stats decode, and the CPU reference of a pixel's landing. Needs no GPU.
//
//   --dry-run    the same run (the rig never writes a file), for the gate's --dry-run convention
//   --self-test  every check
#include "framegen_synth_math.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

using namespace edvr;

namespace {

int g_fail = 0;

void check(bool cond, const char* what) {
    if (!cond) {
        std::printf("framegen_synth_report_test: FAIL: %s\n", what);
        ++g_fail;
    }
}

void checkText(const std::string& got, const char* want, const char* what) {
    if (got != want) {
        std::printf("framegen_synth_report_test: FAIL: %s\n  got:  %s\n  want: %s\n", what, got.c_str(), want);
        ++g_fail;
    }
}

constexpr float kPi = 3.14159265358979f;

void rotAxis(int axis, float deg, float m[9]) {
    const float r = deg * kPi / 180.0f, c = std::cos(r), s = std::sin(r);
    const float y[9] = {c, 0, s, 0, 1, 0, -s, 0, c};
    const float x[9] = {1, 0, 0, 0, c, -s, 0, s, c};
    const float z[9] = {c, -s, 0, s, c, 0, 0, 0, 1};
    std::memcpy(m, axis == 0 ? x : (axis == 1 ? y : z), sizeof(y));
}

void caseLines() {
    checkText(formatFrameGenSynthArmed(612, 699, 1224, 1398, 10),
              "frame generation (synth): armed, render 612x699 output 1224x1398 per eye, S format 10, f=0.5 built on the even frames and f=1 scored against the next real frame (nothing is shown)",
              "the armed line");

    FrameGenSynthWindow w;
    w.frames[0] = 40;
    w.frames[1] = 39;
    w.scored = 38;
    w.skipped = 2;
    w.gpuSamples[0] = 40;
    w.gpuMedian[0] = 0.31f;
    w.gpuMax[0] = 0.8f;   // the f = 1 frames were not timed: "-(n=0)"
    w.pixels = 1000000;
    w.filled = 2500;
    w.left = 100;
    w.guard = 15000;
    w.cells = 1000;
    w.cls[0].pixels = 300;
    w.cls[0].hist[0] = 300;
    w.cls[1].pixels = 100;
    w.cls[1].hist[23] = 100;       // about 0.0482
    w.cls[1].sumErr1024 = 5120;    // mean 0.05
    w.cls[1].histId[28] = 100;     // about 0.1146
    w.cls[1].sumErrId1024 = 10240; // mean 0.1
    w.cls[2].pixels = 600;
    w.cls[2].hist[31] = 600;       // about 0.193
    w.cls[2].sumErr1024 = 600u * 200u;
    const std::string line = formatFrameGenSynthReport(w);
    checkText(line,
              "frame generation (synth): frames=40/39 scored=38 skipped=2 gpu_ms=0.31/0.80(n=40)/-(n=0) holes=0.25%/0.01% guard=1.50%"
              " far=30.0%,0,0,0,0.0%,0.0%|id:0,0 cockpit=10.0%,50,48,48,100.0%,0.0%|id:100,115 world=60.0%,195,193,193,100.0%,100.0%|id:0,0",
              "the report line: counts, the two gpu fields, the hole and guard shares and the scorer's three class fields");
    check(line.size() < 400, "the report line is far under Log::note's 1165 characters");

    FrameGenSynthWindow idle;
    idle.skipped = 7;
    checkText(formatFrameGenSynthReport(idle),
              "frame generation (synth): frames=0/0 scored=0 skipped=7 gpu_ms=-(n=0)/-(n=0) holes=0.00%/0.00% guard=0.00%"
              " far=0.0%,0,0,0,0.0%,0.0%|id:0,0 cockpit=0.0%,0,0,0,0.0%,0.0%|id:0,0 world=0.0%,0,0,0,0.0%,0.0%|id:0,0",
              "a window that called the module and built nothing still says so (frames=0/0, the skips), so a synth that never ran reads differently from one that did");
}

void caseMerge() {
    FrameGenSynthWindow w;
    FrameGenSynthEyeResult built;
    built.filled = 10;
    built.left = 2;
    built.guard = 7;
    frameGenSynthMergeFrame(w, 0, 100, built);
    frameGenSynthMergeFrame(w, 1, 100, built);
    frameGenSynthMergeFrame(w, 1, 100, built);
    check(w.frames[0] == 1 && w.frames[1] == 2 && w.pixels == 300 && w.filled == 30 && w.left == 6 && w.guard == 21, "frames are counted by kind and the counters and pixels add");
    FrameGenSynthEyeResult sc;
    sc.cells = 12;
    sc.cls[2].pixels = 12;
    sc.cls[2].hist[5] = 12;
    sc.cls[2].sumErr1024 = 24;
    frameGenSynthMergeScore(w, sc);
    frameGenSynthMergeScore(w, sc);
    check(w.scored == 2 && w.cells == 24 && w.cls[2].pixels == 24 && w.cls[2].hist[5] == 24 && w.cls[2].sumErr1024 == 48, "a scored frame adds its classes and counts once");
    check(w.cls[0].pixels == 0 && w.cls[1].pixels == 0, "the other classes stay empty");
}

void caseDecode() {
    uint32_t words[kFrameGenSynthStatsUints] = {};
    words[0] = 3;                                                   // far hist bin 0
    words[2 * kFrameGenShadowBins] = 3;                             // far pixels
    words[kFrameGenShadowStatsPerClass * 2 + 2 * kFrameGenShadowBins] = 9;   // world pixels
    words[kFrameGenShadowStatsCells] = 12;
    words[kFrameGenSynthStatsFilled] = 40;
    words[kFrameGenSynthStatsLeft] = 5;
    words[kFrameGenSynthStatsGuard] = 77;
    FrameGenSynthEyeResult r;
    frameGenSynthDecodeStats(words, r);
    check(r.cells == 12 && r.cls[0].pixels == 3 && r.cls[0].hist[0] == 3 && r.cls[2].pixels == 9 && r.filled == 40 && r.left == 5 && r.guard == 77,
          "the stats words decode: the score block as the scorer's, then filled, left and guard");
    check(kFrameGenSynthStatsUints == 302 && kFrameGenSynthStatsFilled == 299, "the stats layout is 299 score words and three counters");
}

void caseRotation() {
    float R[9], half[9], full[9], twice[9];
    rotAxis(1, 6.0f, R);
    frameGenSynthRotationPow(R, 1.0f, full);
    double worst = 0.0;
    for (int i = 0; i < 9; ++i) worst = std::fmax(worst, std::fabs(full[i] - R[i]));
    check(worst < 1e-6, "R^1 is R");
    frameGenSynthRotationPow(R, 0.0f, half);
    worst = 0.0;
    for (int i = 0; i < 9; ++i) worst = std::fmax(worst, std::fabs(half[i] - ((i % 4 == 0) ? 1.0f : 0.0f)));
    check(worst < 1e-6, "R^0 is the identity");
    frameGenSynthRotationPow(R, 0.5f, half);
    temporalMul3(half, half, twice);
    worst = 0.0;
    for (int i = 0; i < 9; ++i) worst = std::fmax(worst, std::fabs(twice[i] - R[i]));
    check(worst < 1e-6, "(R^0.5)^2 is R");
    check(std::fabs(temporalRotationAngleDeg(half) - 3.0f) < 1e-3f, "R^0.5 of a 6 degree turn is 3 degrees");
    float q[9];
    rotAxis(0, 90.0f, q);
    frameGenSynthRotationPow(q, 2.0f, half);   // beyond the short way: 180 degrees about x
    check(std::fabs(half[4] + 1.0f) < 1e-5f && std::fabs(half[8] + 1.0f) < 1e-5f && std::fabs(half[0] - 1.0f) < 1e-5f, "R^2 of a 90 degree turn is the half turn");
    rotAxis(2, -170.0f, q);
    frameGenSynthRotationPow(q, 0.5f, half);
    check(std::fabs(temporalRotationAngleDeg(half) - 85.0f) < 1e-3f, "a big negative turn takes its half the short way");
}

void caseHeadStep() {
    // B's backward head delta is a 3 degree turn about y and a 20 mm shift; the forward step A to B is its inverse.
    FrameGenDeltas d;
    rotAxis(1, 3.0f, d.Hd);
    d.th[0] = 0.02f;
    d.th[1] = -0.01f;
    d.th[2] = 0.004f;
    float rot[9], t[3];
    frameGenSynthHeadStep(d, 1.0f, rot, t);
    float back[3], p[3] = {0.3f, -0.2f, -1.1f}, a[3], b[3];
    temporalApply3(d.Hd, p, a);                    // a point of B's eye space in A's
    for (int i = 0; i < 3; ++i) a[i] += d.th[i];
    temporalApply3(rot, a, back);                  // carried forward A to B by the f = 1 step
    for (int i = 0; i < 3; ++i) b[i] = back[i] + t[i];
    check(std::fabs(b[0] - p[0]) < 1e-5f && std::fabs(b[1] - p[1]) < 1e-5f && std::fabs(b[2] - p[2]) < 1e-5f, "the f = 1 head step is the forward step A to B (the inverse of the backward delta)");
    float rotH[9], tH[3];
    frameGenSynthHeadStep(d, 0.5f, rotH, tH);
    check(std::fabs(temporalRotationAngleDeg(rotH) - 1.5f) < 1e-3f, "the f = 0.5 head step turns half as far");
    check(std::fabs(tH[0] - 0.5f * t[0]) < 1e-7f && std::fabs(tH[1] - 0.5f * t[1]) < 1e-7f && std::fabs(tH[2] - 0.5f * t[2]) < 1e-7f, "and its translation is half");
    check(t[0] < -0.015f && t[1] > 0.005f, "the forward translation is the backward one reversed (the head moved +x in A's frame, so B is at -x of it)");
}

void caseCb() {
    FrameGenSynthCb cb;
    FrameGenDeltas d;
    rotAxis(1, 2.0f, d.Hd);
    d.th[0] = 0.01f;
    const float tan[4] = {-1.0f, 1.0f, -0.75f, 0.75f};
    frameGenSynthFillCb(cb, 128, 96, 256, 192, tan, 0.0f, 0.025f, 10.0f, 0.5f, d);
    check(cb.sz[0] == 128 && cb.sz[1] == 96 && cb.sz[2] == 256 && cb.sz[3] == 192 && cb.qz[0] == 32 && cb.qz[1] == 24, "the sizes and the quarter grid");
    check(cb.dk[0] == 0.0f && cb.dk[1] == 0.025f && cb.dk[2] == 10.0f && cb.dk[3] == 0.5f, "the depth encoding, the split and f ride dk");
    check(std::fabs(cb.hp[0][0] - d.Hd[0]) < 1e-7f && std::fabs(cb.hp[0][2] - d.Hd[2]) < 1e-7f && std::fabs(cb.hp[2][0] - d.Hd[6]) < 1e-7f, "hp holds the backward head rotation, row by row");
    float rot[9], t[3];
    frameGenSynthHeadStep(d, 0.5f, rot, t);
    check(std::fabs(cb.rs[1][1] - rot[4]) < 1e-7f && std::fabs(cb.rs[0][2] - rot[2]) < 1e-7f && std::fabs(cb.rs[0][3] - t[0]) < 1e-9f, "rs holds the step, translation in w");
    check(sizeof(FrameGenSynthCb) == 160, "ten rows of 16 bytes");
}

void caseTarget() {
    FrameGenSynthCb cb;
    FrameGenDeltas still;
    const float tan[4] = {-1.0f, 1.0f, -0.75f, 0.75f};
    frameGenSynthFillCb(cb, 128, 96, 128, 96, tan, 0.0f, 0.025f, 10.0f, 0.5f, still);
    float pp[2];
    int cls = -1;
    // The world with no head motion: the ship's screen motion m (the vector, B to A) carried back by f: p - f m.
    check(frameGenSynthTarget(cb, 40.0f, 30.0f, 50.0f, false, 6.0f, -2.0f, pp, &cls) && cls == 2 && std::fabs(pp[0] - 37.0f) < 1e-3f && std::fabs(pp[1] - 31.0f) < 1e-3f,
          "a world pixel with the head still moves by -f of its vector (50 m, vector (6, -2), f 0.5: (37, 31))");
    check(frameGenSynthTarget(cb, 40.0f, 30.0f, 0.0f, true, 0.0f, 0.0f, pp, &cls) && cls == 0 && std::fabs(pp[0] - 40.0f) < 1e-3f && std::fabs(pp[1] - 30.0f) < 1e-3f,
          "a far pixel with no vector stays");
    check(frameGenSynthTarget(cb, 40.0f, 30.0f, 0.5f, false, 9.0f, 9.0f, pp, &cls) && cls == 1 && std::fabs(pp[0] - 40.0f) < 1e-3f && std::fabs(pp[1] - 30.0f) < 1e-3f,
          "a cockpit pixel with the head still stays whatever its vector says (the class is the head's alone)");
    check(frameGenSynthTarget(cb, 40.0f, 30.0f, 10.0f, false, 0.0f, 0.0f, pp, &cls) && cls == 1, "exactly the split is the cockpit (z <= split)");
}

}  // namespace

int main(int argc, char** argv) {
    bool run = false;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--self-test") || !std::strcmp(argv[i], "--dry-run")) run = true;
        else {
            std::fprintf(stderr, "usage: framegen_synth_report_test --self-test | --dry-run\n");
            return 2;
        }
    }
    if (!run) {
        std::fprintf(stderr, "usage: framegen_synth_report_test --self-test | --dry-run\n");
        return 2;
    }
    caseLines();
    caseMerge();
    caseDecode();
    caseRotation();
    caseHeadStep();
    caseCb();
    caseTarget();
    if (g_fail) {
        std::printf("framegen_synth_report_test: %d check(s) FAILED\n", g_fail);
        return 1;
    }
    std::printf("framegen_synth_report_test: all checks passed\n");
    return 0;
}
