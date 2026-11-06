#pragma once
// The frame-generation shadow scorer's statistics and its two log lines (fix.frame_generation_shadow, TEMPORARY; docs/design-frame-generation-2026-10-26.md,
// Phase 1). Pure: no Windows or D3D includes, so the test rig and tools\edvr_log.py's --health self-test read the same format strings the DLL logs with.
//
// An error is the absolute luma difference between the warped previous frame and the real one, 0..1. The scorer bins it into 48 log bins (frameGenShadowBin),
// keeps a pixel count and a fixed-point sum (error x 1024) per class, and the 1 s report reads percentiles back from the histogram. Errors are printed as
// integers in thousandths of luma ("milli-luma") to keep the line short.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>

namespace edvr {

constexpr int kFrameGenShadowBins = 48;
constexpr int kFrameGenShadowClasses = 3;   // 0 far, 1 cockpit, 2 world

enum FrameGenShadowSkip : int {
    kSkipNoPrevDepth = 0,
    kSkipNoPrevDelta,
    kSkipRowsInvalid,
    kSkipCameraJump,
    kSkipReadbackLate,
    kFrameGenShadowSkipCount
};

struct FrameGenShadowClassStats {
    uint32_t pixels = 0;
    uint64_t sumErr1024 = 0;                    // sum of the warped error x 1024
    uint32_t hist[kFrameGenShadowBins] = {};    // the warped error
    uint64_t sumErrId1024 = 0;                  // the same for the no-warp comparison (the same cell of the previous frame)
    uint32_t histId[kFrameGenShadowBins] = {};
};

// One eye's scored result for one frame.
struct FrameGenShadowEyeResult {
    FrameGenShadowClassStats cls[kFrameGenShadowClasses];
    uint64_t holes = 0;
    uint64_t cells = 0;   // every cell looked at, holes included
};

struct FrameGenShadowWindow {
    uint32_t framesScored = 0;
    uint32_t skipped[kFrameGenShadowSkipCount] = {};
    FrameGenShadowClassStats cls[kFrameGenShadowClasses];
    uint64_t holes = 0;
    uint64_t cells = 0;
    float gpuMsMedian = 0.0f, gpuMsMax = 0.0f;
    float rigDegMedian = 0.0f, rigDegMax = 0.0f, rigMmMedian = 0.0f, rigMmMax = 0.0f;
};

// Bin 0 for e < 1/1024, else clamp(1 + floor(4 * log2(e * 1024)), 1, 47).
inline int frameGenShadowBin(float e) {
    if (!(e >= 1.0f / 1024.0f)) return 0;   // also catches NaN
    const float l = 4.0f * std::log2(e * 1024.0f);
    if (!(l < 46.0f)) return kFrameGenShadowBins - 1;
    const int b = 1 + static_cast<int>(std::floor(l));
    return b < 1 ? 1 : (b > kFrameGenShadowBins - 1 ? kFrameGenShadowBins - 1 : b);
}

// The bin's representative error: the geometric middle of its edges, 2^((bin - 0.5) / 4) / 1024. Bin 0 is 0.
inline float frameGenShadowBinValue(int bin) {
    if (bin <= 0) return 0.0f;
    if (bin > kFrameGenShadowBins - 1) bin = kFrameGenShadowBins - 1;
    return std::exp2((static_cast<float>(bin) - 0.5f) * 0.25f) / 1024.0f;
}

inline uint64_t frameGenShadowHistTotal(const uint32_t hist[kFrameGenShadowBins]) {
    uint64_t n = 0;
    for (int i = 0; i < kFrameGenShadowBins; ++i) n += hist[i];
    return n;
}

// The representative error of the bin where the cumulative count first reaches q of the total (q in 0..1). An empty histogram reads 0.
inline float frameGenShadowPercentile(const uint32_t hist[kFrameGenShadowBins], float q) {
    const uint64_t total = frameGenShadowHistTotal(hist);
    if (total == 0) return 0.0f;
    if (q < 0.0f) q = 0.0f;
    if (q > 1.0f) q = 1.0f;
    uint64_t need = static_cast<uint64_t>(std::ceil(static_cast<double>(q) * static_cast<double>(total)));
    if (need < 1) need = 1;
    uint64_t run = 0;
    for (int i = 0; i < kFrameGenShadowBins; ++i) {
        run += hist[i];
        if (run >= need) return frameGenShadowBinValue(i);
    }
    return frameGenShadowBinValue(kFrameGenShadowBins - 1);
}

// The share (0..1) of the histogram's pixels in whole bins whose representative error is above threshold. An empty histogram reads 0.
inline float frameGenShadowShareOver(const uint32_t hist[kFrameGenShadowBins], float threshold) {
    const uint64_t total = frameGenShadowHistTotal(hist);
    if (total == 0) return 0.0f;
    uint64_t over = 0;
    for (int i = 0; i < kFrameGenShadowBins; ++i)
        if (frameGenShadowBinValue(i) > threshold) over += hist[i];
    return static_cast<float>(static_cast<double>(over) / static_cast<double>(total));
}

inline void frameGenShadowAddClass(FrameGenShadowClassStats& into, const FrameGenShadowClassStats& from) {
    into.pixels += from.pixels;
    into.sumErr1024 += from.sumErr1024;
    into.sumErrId1024 += from.sumErrId1024;
    for (int i = 0; i < kFrameGenShadowBins; ++i) {
        into.hist[i] += from.hist[i];
        into.histId[i] += from.histId[i];
    }
}

// Adds one eye's result into the window: counts, sums and histograms. The caller counts framesScored.
inline void frameGenShadowMerge(FrameGenShadowWindow& w, const FrameGenShadowEyeResult& eye) {
    for (int c = 0; c < kFrameGenShadowClasses; ++c) frameGenShadowAddClass(w.cls[c], eye.cls[c]);
    w.holes += eye.holes;
    w.cells += eye.cells;
}

// ---- the two log lines ----
// The format strings are the single source: the DLL logs with them, the rig renders from them, and tools\edvr_log.py --health reads them from this file.
constexpr const char* kFrameGenShadowArmedFmt =
    "frame generation (shadow): armed, quarter %ux%u per eye, classes far cockpit world (mover, reactive not scored)";
// The report is one line: head, then one class field per class, then the tail.
constexpr const char* kFrameGenShadowReportHeadFmt = "frame generation (shadow): frames=%u skipped=%u/%u/%u/%u/%u";
// name=share%,mean,p50,p95,over0.02%,over0.05%|id:mean,p95 -- errors in milli-luma, the shares in percent (the class's share is of all cells, the over shares of the class)
constexpr const char* kFrameGenShadowReportClassFmt = " %s=%.1f%%,%u,%u,%u,%.1f%%,%.1f%%|id:%u,%u";
constexpr const char* kFrameGenShadowReportTailFmt = " holes=%.1f%% gpu_ms=%.2f/%.2f rig_deg=%.3f/%.3f rig_mm=%.2f/%.2f";
constexpr const char* kFrameGenShadowClassNames[kFrameGenShadowClasses] = {"far", "cockpit", "world"};

inline std::string formatFrameGenShadowArmed(uint32_t qw, uint32_t qh) {
    char buf[160];
    std::snprintf(buf, sizeof(buf), kFrameGenShadowArmedFmt, static_cast<unsigned>(qw), static_cast<unsigned>(qh));
    return buf;
}

inline unsigned frameGenShadowMilli(double luma) {
    return luma <= 0.0 ? 0u : static_cast<unsigned>(luma * 1000.0 + 0.5);
}

inline std::string formatFrameGenShadowReport(const FrameGenShadowWindow& w) {
    char buf[768];
    int n = std::snprintf(buf, sizeof(buf), kFrameGenShadowReportHeadFmt, static_cast<unsigned>(w.framesScored), static_cast<unsigned>(w.skipped[0]),
                          static_cast<unsigned>(w.skipped[1]), static_cast<unsigned>(w.skipped[2]), static_cast<unsigned>(w.skipped[3]),
                          static_cast<unsigned>(w.skipped[4]));
    const double cells = w.cells ? static_cast<double>(w.cells) : 1.0;
    for (int c = 0; c < kFrameGenShadowClasses && n > 0 && n < static_cast<int>(sizeof(buf)); ++c) {
        const FrameGenShadowClassStats& s = w.cls[c];
        const double px = s.pixels ? static_cast<double>(s.pixels) : 1.0;
        const double mean = static_cast<double>(s.sumErr1024) / 1024.0 / px;
        const double meanId = static_cast<double>(s.sumErrId1024) / 1024.0 / px;
        n += std::snprintf(buf + n, sizeof(buf) - n, kFrameGenShadowReportClassFmt, kFrameGenShadowClassNames[c], 100.0 * static_cast<double>(s.pixels) / cells,
                           frameGenShadowMilli(s.pixels ? mean : 0.0), frameGenShadowMilli(frameGenShadowPercentile(s.hist, 0.50f)),
                           frameGenShadowMilli(frameGenShadowPercentile(s.hist, 0.95f)), 100.0 * frameGenShadowShareOver(s.hist, 0.02f),
                           100.0 * frameGenShadowShareOver(s.hist, 0.05f), frameGenShadowMilli(s.pixels ? meanId : 0.0),
                           frameGenShadowMilli(frameGenShadowPercentile(s.histId, 0.95f)));
    }
    if (n > 0 && n < static_cast<int>(sizeof(buf)))
        std::snprintf(buf + n, sizeof(buf) - n, kFrameGenShadowReportTailFmt, 100.0 * static_cast<double>(w.holes) / cells, static_cast<double>(w.gpuMsMedian),
                      static_cast<double>(w.gpuMsMax), static_cast<double>(w.rigDegMedian), static_cast<double>(w.rigDegMax), static_cast<double>(w.rigMmMedian),
                      static_cast<double>(w.rigMmMax));
    return buf;
}

}  // namespace edvr
