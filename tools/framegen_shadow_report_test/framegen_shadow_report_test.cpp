// framegen_shadow_report_test: the pure half of the frame-generation shadow scorer (src/common/framegen_shadow_report.h; fix.frame_generation_shadow,
// TEMPORARY, docs/design-frame-generation-2026-10-26.md Phase 1): the histogram binning, the percentile and share-over arithmetic, the merge, and the exact
// text of the armed and report lines.
//
//   --dry-run    the same run (the rig never writes a file), for the gate's --dry-run convention
//   --self-test  every check
#include "framegen_shadow_report.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

using namespace edvr;

namespace {

int g_fail = 0;

void check(bool cond, const char* what) {
    if (!cond) {
        std::printf("framegen_shadow_report_test: FAIL: %s\n", what);
        ++g_fail;
    }
}

void checkText(const std::string& got, const char* want, const char* what) {
    if (got != want) {
        std::printf("framegen_shadow_report_test: FAIL: %s\n  got:  %s\n  want: %s\n", what, got.c_str(), want);
        ++g_fail;
    }
}

void caseBins() {
    check(frameGenShadowBin(0.0f) == 0, "bin of 0 is 0");
    check(frameGenShadowBin(0.00097f) == 0, "bin just under 1/1024 is 0");
    check(frameGenShadowBin(1.0f / 1024.0f) == 1, "bin of exactly 1/1024 is 1");
    check(frameGenShadowBin(1.1f / 1024.0f) == 1, "bin 1 spans 1/1024 up to a quarter octave (2^(1/4) = 1.189)");
    check(frameGenShadowBin(1.2f / 1024.0f) == 2, "bin 2 begins past 2^(1/4)/1024");
    check(frameGenShadowBin(0.05f) == 23, "bin of 0.05 is 23");
    check(frameGenShadowBin(0.2f) == 31, "bin of 0.2 is 31");
    check(frameGenShadowBin(1.0f) == 41, "bin of 1.0 is 41");
    check(frameGenShadowBin(100.0f) == 47, "a large error clamps to bin 47");
    check(frameGenShadowBin(std::nanf("")) == 0, "NaN is bin 0");
    check(frameGenShadowBin(-0.5f) == 0, "a negative error is bin 0");
    check(frameGenShadowBinValue(0) == 0.0f, "bin 0 reads 0");
    bool roundTrip = true, rising = true;
    float prev = 0.0f;
    for (int b = 1; b < kFrameGenShadowBins; ++b) {
        const float v = frameGenShadowBinValue(b);
        if (frameGenShadowBin(v) != b) roundTrip = false;
        if (!(v > prev)) rising = false;
        prev = v;
    }
    check(roundTrip, "every bin's representative value bins back to it");
    check(rising, "representative values rise with the bin");
    check(std::fabs(frameGenShadowBinValue(23) - 0.0482f) < 0.0005f, "bin 23 reads about 0.0482");
}

void caseStats() {
    uint32_t empty[kFrameGenShadowBins] = {};
    check(frameGenShadowPercentile(empty, 0.5f) == 0.0f, "percentile of an empty histogram is 0");
    check(frameGenShadowShareOver(empty, 0.02f) == 0.0f, "share over of an empty histogram is 0");

    uint32_t h[kFrameGenShadowBins] = {};
    h[0] = 50;
    h[18] = 25;   // about 0.0203
    h[31] = 25;   // about 0.193
    check(frameGenShadowPercentile(h, 0.5f) == 0.0f, "p50 lands on the last of the 50 zeros");
    check(frameGenShadowPercentile(h, 0.51f) == frameGenShadowBinValue(18), "p51 steps into bin 18");
    check(frameGenShadowPercentile(h, 0.75f) == frameGenShadowBinValue(18), "p75 is the last of bin 18");
    check(frameGenShadowPercentile(h, 0.76f) == frameGenShadowBinValue(31), "p76 steps into bin 31");
    check(frameGenShadowPercentile(h, 1.0f) == frameGenShadowBinValue(31), "p100 is the top occupied bin");
    check(frameGenShadowPercentile(h, 0.0f) == 0.0f, "p0 is the first occupied bin");
    check(std::fabs(frameGenShadowShareOver(h, 0.02f) - 0.5f) < 1e-6f, "bin 18 (0.0203) and bin 31 are over 0.02");
    check(std::fabs(frameGenShadowShareOver(h, 0.05f) - 0.25f) < 1e-6f, "only bin 31 is over 0.05");
    check(frameGenShadowShareOver(h, 0.5f) == 0.0f, "nothing is over 0.5");
}

void caseMerge() {
    FrameGenShadowEyeResult a, b;
    a.cls[1].pixels = 10;
    a.cls[1].sumErr1024 = 100;
    a.cls[1].hist[5] = 4;
    a.cls[1].hist[9] = 6;
    a.cls[1].sumErrId1024 = 7;
    a.cls[1].histId[3] = 10;
    a.holes = 3;
    a.cells = 40;
    b.cls[1].pixels = 5;
    b.cls[1].sumErr1024 = 50;
    b.cls[1].hist[5] = 5;
    b.cls[1].sumErrId1024 = 1;
    b.cls[1].histId[3] = 5;
    b.cls[2].pixels = 2;
    b.holes = 1;
    b.cells = 20;
    FrameGenShadowWindow w;
    frameGenShadowMerge(w, a);
    frameGenShadowMerge(w, b);
    check(w.cls[1].pixels == 15 && w.cls[1].sumErr1024 == 150 && w.cls[1].sumErrId1024 == 8, "merge adds counts and sums");
    check(w.cls[1].hist[5] == 9 && w.cls[1].hist[9] == 6 && w.cls[1].histId[3] == 15, "merge adds histograms");
    check(w.cls[2].pixels == 2 && w.cls[0].pixels == 0, "merge keeps the classes apart");
    check(w.holes == 4 && w.cells == 60, "merge adds holes and cells");
    check(w.framesScored == 0, "merge leaves the frame count to the caller");
}

void caseLines() {
    checkText(formatFrameGenShadowArmed(612, 699),
              "frame generation (shadow): armed, quarter 612x699 per eye, classes far cockpit world (mover, reactive not scored)", "the armed line");

    FrameGenShadowWindow w;
    w.framesScored = 60;
    for (int i = 0; i < kFrameGenShadowSkipCount; ++i) w.skipped[i] = static_cast<uint32_t>(i + 1);
    w.cells = 1000;
    w.holes = 100;
    w.cls[0].pixels = 100;
    w.cls[0].hist[0] = 100;
    w.cls[1].pixels = 200;
    w.cls[1].sumErr1024 = 10240;   // mean 0.05
    w.cls[1].hist[23] = 200;
    w.cls[1].histId[0] = 200;
    w.cls[2].pixels = 300;
    w.cls[2].sumErr1024 = 30720;   // mean 0.1
    w.cls[2].hist[0] = 150;
    w.cls[2].hist[31] = 150;
    w.cls[2].sumErrId1024 = 30720;
    w.cls[2].histId[23] = 300;
    w.gpuMsMedian = 0.85f;
    w.gpuMsMax = 1.2f;
    w.rigDegMedian = 0.012f;
    w.rigDegMax = 0.034f;
    w.rigMmMedian = 0.4f;
    w.rigMmMax = 1.25f;
    const std::string line = formatFrameGenShadowReport(w);
    checkText(line,
              "frame generation (shadow): frames=60 skipped=1/2/3/4/5 far=10.0%,0,0,0,0.0%,0.0%|id:0,0 cockpit=20.0%,50,48,48,100.0%,0.0%|id:0,0 "
              "world=30.0%,100,0,193,50.0%,50.0%|id:100,48 holes=10.0% gpu_ms=0.85/1.20 rig_deg=0.012/0.034 rig_mm=0.40/1.25",
              "the report line");
    check(line.find('\n') == std::string::npos, "the report is one line");

    // An empty window still prints (no division by zero), every figure zero.
    FrameGenShadowWindow z;
    checkText(formatFrameGenShadowReport(z),
              "frame generation (shadow): frames=0 skipped=0/0/0/0/0 far=0.0%,0,0,0,0.0%,0.0%|id:0,0 cockpit=0.0%,0,0,0,0.0%,0.0%|id:0,0 "
              "world=0.0%,0,0,0,0.0%,0.0%|id:0,0 holes=0.0% gpu_ms=0.00/0.00 rig_deg=0.000/0.000 rig_mm=0.00/0.00",
              "an empty window");
}

}  // namespace

int main(int argc, char** argv) {
    bool run = false;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--self-test") || !std::strcmp(argv[i], "--dry-run")) run = true;
        else {
            std::fprintf(stderr, "usage: framegen_shadow_report_test --self-test | --dry-run\n");
            return 2;
        }
    }
    if (!run) {
        std::fprintf(stderr, "usage: framegen_shadow_report_test --self-test | --dry-run\n");
        return 2;
    }
    caseBins();
    caseStats();
    caseMerge();
    caseLines();
    if (g_fail) {
        std::printf("framegen_shadow_report_test: %d check(s) FAILED\n", g_fail);
        return 1;
    }
    std::printf("framegen_shadow_report_test: OK\n");
    return 0;
}
