#pragma once
// The frame-generation synthesizer's statistics and its two log lines (fix.frame_generation_shadow, TEMPORARY; docs/design-frame-generation-2026-10-26.md, Phase 2.1,
// "The synthesis pass"). Pure: no Windows or D3D includes, so the test rig and tools\edvr_log.py's --health self-test read the same format strings the DLL logs with.
//
// The synthesizer builds, per eye and real frame B, the frame S one display period later; nothing is shown. Every other real frame it builds the f = 0.5 S (the one a
// later phase would show: Elite's step is two periods) and on the frames between it the f = 1 S, which lands on the next real frame C's time and is scored against C the way
// the shadow scorer scores a warp: the luma error per class of B, with the no-warp comparison (B against C) beside it. The classes, bins and class fields are the scorer's
// (framegen_shadow_report.h), so the two lines read side by side.
#include "framegen_shadow_report.h"

#include <cstdint>
#include <cstdio>
#include <string>

namespace edvr {

// One eye-frame's decoded result.
struct FrameGenSynthEyeResult {
    FrameGenShadowClassStats cls[kFrameGenShadowClasses];   // the score against C, by B's class (zero when this frame scored nothing)
    uint64_t cells = 0;                                     // the cells scored
    uint64_t filled = 0, left = 0, guard = 0;               // the render pixels this frame's S filled from the background, left holes, and read by the guard
};

struct FrameGenSynthWindow {
    uint32_t frames[2] = {};      // eye-frames built: [0] the f = 0.5 S, [1] the f = 1 S
    uint32_t scored = 0;          // f = 1 eye-frames scored against the next real frame
    uint32_t skipped = 0;         // eye-frames the module was called for and built nothing (no depth, no vectors, a jump...)
    FrameGenShadowClassStats cls[kFrameGenShadowClasses];
    uint64_t cells = 0;
    uint64_t pixels = 0;          // render pixels of the built eye-frames
    uint64_t filled = 0, left = 0, guard = 0;
    float gpuMedian[2] = {}, gpuMax[2] = {};
    uint32_t gpuSamples[2] = {};  // the timed frames behind each pair; 0 reads "-(n=0)"
};

// Adds one eye-frame's decoded counters (`built` of the kind 0 or 1, `renderPixels` of its render size); `r.cells` is non-zero only on a scored frame.
inline void frameGenSynthMergeFrame(FrameGenSynthWindow& w, int kind, uint64_t renderPixels, const FrameGenSynthEyeResult& r) {
    ++w.frames[kind ? 1 : 0];
    w.pixels += renderPixels;
    w.filled += r.filled;
    w.left += r.left;
    w.guard += r.guard;
}

inline void frameGenSynthMergeScore(FrameGenSynthWindow& w, const FrameGenSynthEyeResult& r) {
    ++w.scored;
    for (int c = 0; c < kFrameGenShadowClasses; ++c) frameGenShadowAddClass(w.cls[c], r.cls[c]);
    w.cells += r.cells;
}

// ---- the log lines ----
// The format strings are the single source: the DLL logs with them, the rig renders from them, and tools\edvr_log.py --health reads them from this file.
constexpr const char* kFrameGenSynthArmedFmt =
    "frame generation (synth): armed, render %ux%u output %ux%u per eye, S format %u, f=0.5 built on the even frames and f=1 scored against the next real frame (nothing is shown)";
// frames=<f=0.5 built>/<f=1 built> scored=<f=1 frames scored against the next real frame> skipped=<calls that built nothing> gpu_ms=<f=0.5 median/max(n=)>/<f=1 median/max(n=)>
// holes=<filled>%/<left>% of the render pixels guard=<read by the guard>% of them, then the scorer's three class fields: the f = 1 S against C by B's class.
constexpr const char* kFrameGenSynthReportHeadFmt = "frame generation (synth): frames=%u/%u scored=%u skipped=%u";
constexpr const char* kFrameGenSynthReportTailFmt = " gpu_ms=%s/%s holes=%.2f%%/%.2f%% guard=%.2f%%";

inline std::string formatFrameGenSynthArmed(uint32_t rw, uint32_t rh, uint32_t ow, uint32_t oh, uint32_t sFormat) {
    char buf[256];
    std::snprintf(buf, sizeof(buf), kFrameGenSynthArmedFmt, static_cast<unsigned>(rw), static_cast<unsigned>(rh), static_cast<unsigned>(ow), static_cast<unsigned>(oh),
                  static_cast<unsigned>(sFormat));
    return buf;
}

inline std::string formatFrameGenSynthGpu(const FrameGenSynthWindow& w, int kind) {
    if (!w.gpuSamples[kind]) return kFrameGenShadowGpuNoneFmt;
    char b[48];
    std::snprintf(b, sizeof(b), kFrameGenShadowGpuFmt, static_cast<double>(w.gpuMedian[kind]), static_cast<double>(w.gpuMax[kind]), static_cast<unsigned>(w.gpuSamples[kind]));
    return b;
}

// The one-second line. The caller says it when the module was called in the window (frames built or calls skipped); a window it was never called in says nothing.
inline std::string formatFrameGenSynthReport(const FrameGenSynthWindow& w) {
    char buf[1024];
    int n = std::snprintf(buf, sizeof(buf), kFrameGenSynthReportHeadFmt, static_cast<unsigned>(w.frames[0]), static_cast<unsigned>(w.frames[1]), static_cast<unsigned>(w.scored),
                          static_cast<unsigned>(w.skipped));
    const double px = w.pixels ? static_cast<double>(w.pixels) : 1.0;
    const std::string g0 = formatFrameGenSynthGpu(w, 0), g1 = formatFrameGenSynthGpu(w, 1);
    if (n > 0 && n < static_cast<int>(sizeof(buf)))
        n += std::snprintf(buf + n, sizeof(buf) - n, kFrameGenSynthReportTailFmt, g0.c_str(), g1.c_str(), 100.0 * static_cast<double>(w.filled) / px,
                           100.0 * static_cast<double>(w.left) / px, 100.0 * static_cast<double>(w.guard) / px);
    n = formatFrameGenShadowClasses(buf, sizeof(buf), n, w.cls, w.cells);
    return buf;
}

}  // namespace edvr
