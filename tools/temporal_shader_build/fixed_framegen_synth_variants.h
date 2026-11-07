#pragma once
// Build-only registry: the frame-generation synthesizer (fix.frame_generation_shadow, TEMPORARY; src/d3d11/framegen_synth_shader.h, docs/design-frame-generation-2026-10-26.md,
// Phase 2.1): one text, seven entry points (clear, splat, source, resolve, resample, luma, score). The text is the temporal pass's carry functions (cut from kTemporalCsHlsl
// between the two EDVR_TEMPORAL_CARRY markers) followed by the synthesizer's own, assembled once by frameGenSynthShaderSource(). New, so held to its own contract (symbol, source
// name, entry, profile, the text, the 8x8 group and the bindings each entry reflects) in temporal_shader_build's self-test; tools/framegen_synth_test runs the compiled bytes on WARP.
static std::vector<Variant> frameGenSynthVariants() {
    const char* text = edvr::frameGenSynthShaderSource().c_str();
    return {
        {"kFrameGenSynthClearBytecode", "framegen synth", "fgSynClear", nullptr, {}, false, text, "cs_5_0"},
        {"kFrameGenSynthSplatBytecode", "framegen synth", "fgSynSplat", nullptr, {}, false, text, "cs_5_0"},
        {"kFrameGenSynthSrcBytecode", "framegen synth", "fgSynSrc", nullptr, {}, false, text, "cs_5_0"},
        {"kFrameGenSynthResolveBytecode", "framegen synth", "fgSynResolve", nullptr, {}, false, text, "cs_5_0"},
        {"kFrameGenSynthResampleBytecode", "framegen synth", "fgSynResample", nullptr, {}, false, text, "cs_5_0"},
        {"kFrameGenSynthLumaBytecode", "framegen synth", "fgSynLuma", nullptr, {}, false, text, "cs_5_0"},
        {"kFrameGenSynthScoreBytecode", "framegen synth", "fgSynScore", nullptr, {}, false, text, "cs_5_0"},
    };
}
