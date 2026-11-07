#pragma once
// The frame-generation shadow scorer's geometry (fix.frame_generation_shadow, TEMPORARY; docs/design-frame-generation-2026-10-26.md, Phase 1). Pure: the
// prediction of the next frame's camera from the last two, the three per-class transforms, the rig miss, and the constant block the scorer's shaders read.
// The DLL and the WARP rig (tools\framegen_shadow_test) both build the shader's inputs here, so a transform that drifts from the plan fails in the rig.
//
// Conventions. A "backward" transform maps a point in frame B's eye space to the same point in the previous real frame A's: x_A = R x_B + t, the form the
// motion pass uses (dp = R d z + t). The forward transform (A to B) is its inverse, x_B = R^T x_A - R^T t. Matrices are row-major 3x3.
// Per frame, the pass has the world delta W with translation tvCam (the camera rows' reading) and the head delta Hd with translation th; the ship's own
// motion is what is left of the world delta once the head is taken out: S = W Hd^T, s = tvCam - th (first order).
#include "framegen_shadow_report.h"
#include "temporal_math.h"

#include <cmath>
#include <cstdint>

namespace edvr {

// What one eye-frame of the pass knew, B -> A direction (a point of this frame's eye space to the previous frame's).
struct FrameGenDeltas {
    float W[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};    // the world delta (cand[2]), identity when not valid
    float tv[3] = {0, 0, 0};                      // tvCam
    float Hd[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};   // the head delta used (p.dR*)
    float th[3] = {0, 0, 0};                      // the head translation used (p.tvUsed xyz)
};

struct FrameGenAffine {
    float R[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    float t[3] = {0, 0, 0};
};

inline FrameGenAffine frameGenInverse(const FrameGenAffine& a) {
    FrameGenAffine out;
    temporalTranspose3(a.R, out.R);
    float rt[3];
    temporalApply3(out.R, a.t, rt);
    for (int i = 0; i < 3; ++i) out.t[i] = -rt[i];
    return out;
}

// The constant-velocity prediction of frame B's world transform from frame A's deltas and B's head: Wp = (W_A Hd_A^T) Hd_B, tp = (tv_A - th_A) + th_B.
inline void frameGenPredictWorld(const FrameGenDeltas& a, const FrameGenDeltas& b, float Wp[9], float tp[3]) {
    float hdAT[9], s[9];
    temporalTranspose3(a.Hd, hdAT);
    temporalMul3(a.W, hdAT, s);
    temporalMul3(s, b.Hd, Wp);
    for (int i = 0; i < 3; ++i) tp[i] = (a.tv[i] - a.th[i]) + b.th[i];
}

// How far the prediction is from what the pass measured at B: the angle of Wp^T W_B in degrees and the distance of the translations in millimetres.
inline void frameGenRigMiss(const float Wp[9], const float tp[3], const FrameGenDeltas& b, float* deg, float* mm) {
    float wpT[9], diff[9];
    temporalTranspose3(Wp, wpT);
    temporalMul3(wpT, b.W, diff);
    if (deg) *deg = temporalRotationAngleDeg(diff);
    if (mm) {
        double s = 0.0;
        for (int i = 0; i < 3; ++i) s += static_cast<double>(tp[i] - b.tv[i]) * (tp[i] - b.tv[i]);
        *mm = static_cast<float>(std::sqrt(s) * 1000.0);
    }
}

// The backward transform of each class, indexed as the stats are (0 far, 1 cockpit, 2 world): far [Wp | 0], cockpit [Hd_B | th_B], world [Wp | tp].
inline void frameGenBackward(const FrameGenDeltas& b, const float Wp[9], const float tp[3], FrameGenAffine out[kFrameGenShadowClasses]) {
    out[0] = FrameGenAffine{};
    for (int i = 0; i < 9; ++i) out[0].R[i] = Wp[i];
    out[1] = FrameGenAffine{};
    for (int i = 0; i < 9; ++i) out[1].R[i] = b.Hd[i];
    for (int i = 0; i < 3; ++i) out[1].t[i] = b.th[i];
    out[2] = FrameGenAffine{};
    for (int i = 0; i < 9; ++i) out[2].R[i] = Wp[i];
    for (int i = 0; i < 3; ++i) out[2].t[i] = tp[i];
}

// The scorer's constant buffer (b0 of every entry), laid out for the HLSL cbuffer P in src\d3d11\framegen_shadow_shader.h: 35 rows of 16 bytes.
struct FrameGenShadowCb {
    int32_t sz[4];        // render width, render height, output width, output height
    int32_t qz[4];        // quarter grid width, height, the extrapolation variant (0 ex2d, 1 ex2dh; set by the host before each fgShadowEx* dispatch), 0
    float tanA[4];        // the previous frame's tangents (left, right, bottom, top), jitter excluded
    float tanB[4];        // this frame's
    float dk[4];          // the depth encoding's A and B (depth = A + B / z), the ship's split in metres, 0
    float fw[9][4];       // forward (A to B) rows of each class: [class * 3 + row], xyz the row, w the translation component
    float bw[9][4];       // backward (B to A)
    float tb[9][4];       // B's TRUE backward rows whatever the frame's set (far [W_B | 0], cockpit [Hd_B | th_B], world [W_B | tvCam_B]): what the second score kernel uses
    float ha[3][4];       // A's head delta rows (A to the frame before it: the cockpit class's backward rotation of frame A), xyz of each row; the ex2dh variant takes the head out of A's vector with it
};
static_assert(sizeof(FrameGenShadowCb) == 560, "the scorer's cbuffer is 35 16-byte rows");

// The stats buffer: three blocks of one layout (per class hist, histId, pixels, sum, sumId; then holes and cells), then the two disagreement histograms.
//   [0, 299)    the main score (fgShadowScore)        [299, 598)  the mv set (fgShadowScoreB)       [598, 897)  the bdepth set (fgShadowScoreB)
//   [897, 945)  the world disagreement histogram      [945, 993)  the cockpit disagreement histogram
//   [993, 1292) the ex2d set (fgShadowExScore)        [1292, 1591) the ex2dh set                       [1591, 1595) rows off: world cells, world off, cockpit cells, cockpit off
constexpr uint32_t kFrameGenShadowStatsPerClass = 2 * kFrameGenShadowBins + 3;   // hist, histId, pixels, sum, sumId
constexpr uint32_t kFrameGenShadowStatsHoles = kFrameGenShadowClasses * kFrameGenShadowStatsPerClass;
constexpr uint32_t kFrameGenShadowStatsCells = kFrameGenShadowStatsHoles + 1;
constexpr uint32_t kFrameGenShadowStatsBlockUints = kFrameGenShadowStatsCells + 1;   // 299
constexpr uint32_t kFrameGenShadowStatsMvBase = kFrameGenShadowStatsBlockUints;
constexpr uint32_t kFrameGenShadowStatsBdepthBase = 2 * kFrameGenShadowStatsBlockUints;
constexpr uint32_t kFrameGenShadowStatsDisWorld = 3 * kFrameGenShadowStatsBlockUints;
constexpr uint32_t kFrameGenShadowStatsDisCockpit = kFrameGenShadowStatsDisWorld + kFrameGenShadowBins;
constexpr uint32_t kFrameGenShadowStatsEx2dBase = kFrameGenShadowStatsDisCockpit + kFrameGenShadowBins;   // 993
constexpr uint32_t kFrameGenShadowStatsEx2dhBase = kFrameGenShadowStatsEx2dBase + kFrameGenShadowStatsBlockUints;
constexpr uint32_t kFrameGenShadowStatsRowsOff = kFrameGenShadowStatsEx2dhBase + kFrameGenShadowStatsBlockUints;   // 1591
constexpr uint32_t kFrameGenShadowStatsUints = kFrameGenShadowStatsRowsOff + 4;   // 1595

// `hdA` is frame A's head delta (rotation, A's eye to the frame before it's), null for identity. `truth` is B's true backward rows (frameGenBackward of B's measured W and tvCam), null for identity rows.
inline void frameGenFillCb(FrameGenShadowCb& cb, uint32_t renderW, uint32_t renderH, uint32_t outW, uint32_t outH, const float tanA[4], const float tanB[4],
                           float depthA, float depthB, float splitMetres, const FrameGenAffine bwd[kFrameGenShadowClasses],
                           const FrameGenAffine* truth = nullptr, const float hdA[9] = nullptr) {
    cb = FrameGenShadowCb{};
    cb.sz[0] = static_cast<int32_t>(renderW);
    cb.sz[1] = static_cast<int32_t>(renderH);
    cb.sz[2] = static_cast<int32_t>(outW);
    cb.sz[3] = static_cast<int32_t>(outH);
    cb.qz[0] = static_cast<int32_t>((renderW + 3u) / 4u);
    cb.qz[1] = static_cast<int32_t>((renderH + 3u) / 4u);
    for (int i = 0; i < 4; ++i) {
        cb.tanA[i] = tanA[i];
        cb.tanB[i] = tanB[i];
    }
    cb.dk[0] = depthA;
    cb.dk[1] = depthB;
    cb.dk[2] = splitMetres;
    for (int c = 0; c < kFrameGenShadowClasses; ++c) {
        const FrameGenAffine fwd = frameGenInverse(bwd[c]);
        for (int r = 0; r < 3; ++r) {
            for (int k = 0; k < 3; ++k) {
                cb.bw[c * 3 + r][k] = bwd[c].R[r * 3 + k];
                cb.fw[c * 3 + r][k] = fwd.R[r * 3 + k];
            }
            cb.bw[c * 3 + r][3] = bwd[c].t[r];
            cb.fw[c * 3 + r][3] = fwd.t[r];
        }
        const FrameGenAffine t = truth ? truth[c] : FrameGenAffine{};
        for (int r = 0; r < 3; ++r) {
            for (int k = 0; k < 3; ++k) cb.tb[c * 3 + r][k] = t.R[r * 3 + k];
            cb.tb[c * 3 + r][3] = t.t[r];
        }
    }
    for (int r = 0; r < 3; ++r)
        for (int k = 0; k < 3; ++k) cb.ha[r][k] = hdA ? hdA[r * 3 + k] : (r == k ? 1.0f : 0.0f);
}

// Decodes one block of the stats buffer (kFrameGenShadowStatsBlockUints words) into per-class stats, holes and cells.
inline void frameGenDecodeBlock(const uint32_t* s, FrameGenShadowClassStats cls[kFrameGenShadowClasses], uint64_t& holes, uint64_t& cells) {
    for (int c = 0; c < kFrameGenShadowClasses; ++c) {
        const uint32_t* b = s + static_cast<uint32_t>(c) * kFrameGenShadowStatsPerClass;
        FrameGenShadowClassStats& st = cls[c];
        st = FrameGenShadowClassStats{};
        for (int i = 0; i < kFrameGenShadowBins; ++i) {
            st.hist[i] = b[i];
            st.histId[i] = b[kFrameGenShadowBins + i];
        }
        st.pixels = b[2 * kFrameGenShadowBins];
        st.sumErr1024 = b[2 * kFrameGenShadowBins + 1];
        st.sumErrId1024 = b[2 * kFrameGenShadowBins + 2];
    }
    holes = s[kFrameGenShadowStatsHoles];
    cells = s[kFrameGenShadowStatsCells];
}

// Decodes one eye's stats buffer (kFrameGenShadowStatsUints words) into the result the window merges.
inline void frameGenDecodeStats(const uint32_t* s, FrameGenShadowEyeResult& out) {
    out = FrameGenShadowEyeResult{};
    frameGenDecodeBlock(s, out.cls, out.holes, out.cells);
    frameGenDecodeBlock(s + kFrameGenShadowStatsMvBase, out.mv.cls, out.mv.holes, out.mv.cells);
    frameGenDecodeBlock(s + kFrameGenShadowStatsBdepthBase, out.bdepth.cls, out.bdepth.holes, out.bdepth.cells);
    for (int i = 0; i < kFrameGenShadowBins; ++i) {
        out.disWorld[i] = s[kFrameGenShadowStatsDisWorld + i];
        out.disCockpit[i] = s[kFrameGenShadowStatsDisCockpit + i];
    }
    frameGenDecodeBlock(s + kFrameGenShadowStatsEx2dBase, out.ex2d.cls, out.ex2d.holes, out.ex2d.cells);
    frameGenDecodeBlock(s + kFrameGenShadowStatsEx2dhBase, out.ex2dh.cls, out.ex2dh.holes, out.ex2dh.cells);
    out.rowsWorld = s[kFrameGenShadowStatsRowsOff];
    out.rowsWorldOff = s[kFrameGenShadowStatsRowsOff + 1];
    out.rowsCockpit = s[kFrameGenShadowStatsRowsOff + 2];
    out.rowsCockpitOff = s[kFrameGenShadowStatsRowsOff + 3];
}

// The median of a small sample set (the middle element, the upper one for an even count); 0 for none. The vector is reordered.
inline float frameGenMedian(float* v, size_t n) {
    if (n == 0) return 0.0f;
    for (size_t i = 1; i < n; ++i) {   // an insertion sort: a second's worth of samples is a few hundred
        const float x = v[i];
        size_t j = i;
        while (j > 0 && v[j - 1] > x) { v[j] = v[j - 1]; --j; }
        v[j] = x;
    }
    return v[n / 2];
}

// The q-quantile (0..1) of a sample set, the element at floor(q * n) clamped to the last; q = 0.5 is frameGenMedian. The vector is reordered; 0 for none.
inline float frameGenQuantile(float* v, size_t n, float q) {
    if (n == 0) return 0.0f;
    frameGenMedian(v, n);   // sorts
    size_t i = static_cast<size_t>(q * static_cast<float>(n));
    if (i >= n) i = n - 1;
    return v[i];
}

}  // namespace edvr
