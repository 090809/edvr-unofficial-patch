# The own hull as a separate layer: feasibility (2026-11-08)

Question: can EDVR keep the pilot's own hull (the parts outside the canopy,
e.g. an Anaconda's front V wings at 40-80 m) out of what DLSS and frame
generation see, have the world behind it, and put the hull back on top?
Motivation: `design-frame-generation-2026-10-26.md`, entry "2026-11-08:
EDVR's base frame, not frame generation: the wing gap smears with FG off".
The smear is terrain that is newly uncovered at the hull edge every frame
(20-30 render px a frame in a turn at 45 fps). DLSS has no history for it.

## Status

* **State:** desk study only. No code was changed and no flight was flown.
  The answer is yes, but not by changing what the game renders.
* **Finding 1, the hull is a deferred G-buffer object.** It is drawn by the
  pool families vs_66DE/ps_864F, 61AE/FC43, AACF/CF53 and DE54/E46E, plus the
  BBE5/DB3E decal overlay (depth writes off), into the same four-target
  G-buffer and D32 depth as the world. The shared deferred resolve lights
  it. The cockpit shell is a different pair (vs_BFE5 / ps_DB79) with
  stencil 144/148. The hull's stencil is the world's (0, 4), so stencil
  cannot isolate it.
* **Finding 2, EDVR already knows which pixels are hull.** The engine motion
  path patches exactly these pixel shaders to write the pool slot and depth
  to MRT6. The hull is one or a few pool records (record 2114 in run
  193124490001). What is missing is a per-frame list of the own ship's
  records. Per draw call EDVR does not know it: the pool draws are instanced
  across records.
* **Finding 3, re-rendering is the wrong lever.** Options (a) and (c), which
  remove the hull from the game's frame, would need the deferred lighting,
  fog, transparents and post re-run for one of the layers. No precedent in
  EDVR does that: every re-issue in the codebase is immediate and in place,
  and the one deferred replay (UI, 2026-09-23) was retired. The cost is
  several GPU ms, and every ms lost to frame rate widens the band.
* **Recommendation:** prototype option (e), the *hull hole-fill*. Before
  DLSS, replace the hull pixels with the world behind them, reprojected
  from earlier frames. Give DLSS that world-only image, then composite the
  hull (static in view) on top after DLSS. Frame generation gets the same
  two layers for free. This is untested; decide it offline first (below).
* **Smallest experiment (no flight, no build):** an offline script on the
  existing dumps 131530778003 and 132100543006. It measures what share of
  the band pixels (hidden bit 16 along the silhouette) a fill could cover
  from frames k-1..k-8, and how far that fill is from the raw C(k). Details
  are in "The deciding experiment".
* **Open questions for Sean (need `analysis\`, not in this fork):** where the
  own ship's rig is reachable each frame outside a station, and whether its
  records carry a stable "player ship" flag. Today
  `optimized_station_core.h:787-809` finds it by camera containment
  (radius < 40 m, too small for an Anaconda) or by name prefix taken at a
  station engage.
* **Ruled out (this study):**
  * ruled out: isolating the hull by stencil, because hull pixels carry
    stencil 0/4 like the world (kinematic doc, 2026-11-02 entry).
  * ruled out: a separate DLSS feature for the hull alone (b) as a fix,
    because it gives the world behind the hull no history. It is useful only
    on top of a world-only input.
  * ruled out: skipping the hull at draw level by shader pair, because
    those pairs also draw other ships and stations in the same instanced
    calls.

## 1. How Elite draws the own hull

- Deferred. The eye pass writes four G-buffer targets plus depth.
  The resolve pixel shaders read depth t0 and G-buffer t1-3
  (`design-flat-temporal-aa-2026-09-23.md:1818-1819`). The fourth target
  is emissive or zero (`kinematic-motion-injection-2026-09-19.md:1640-1647`).
  There is one pass per eye, and offscreen work (shadow maps, probes) runs
  first (`engine-render-pipeline.md` stage 4-5).
- Hull draws are pool families keyed in `engine_velocity.cpp` kFamilies
  (kinematic doc 1912-1920). The hull's depth writers in run 193124490001
  are vs_66DE/ps_864F (most), 61AE, AACF and DE54, all GEQUAL, write on and
  zero bias. Decals on the hull are vs_BBE5/ps_DB3E with depth writes off
  (kinematic doc 4486-4491).
- Cockpit: the shell is vs_BFE51414CC3024B4 / ps_DB79AE788E049DFD and the
  cockpit trim is at stencil 144/148 (kinematic doc 2321, 2330). The canopy
  glass is a separate transparent draw that EDVR already routes into a
  half-resolution glass target (`ui_layer.cpp` ~3128-3140,
  `dlss-blur-elements-2026-10-14.md` 442-483).
- What depends on the hull's depth or G-buffer:
  - deferred lighting, including the `lighting_volume` class in the GPU
    pass profile;
  - fog and atmosphere, which are depth-based;
  - transparents and particles, which are depth-tested against it;
  - the canopy glass drawn over it;
  - EDVR's temporal pass (path 11, `temporal_shader_source.h` ~984-1001 and
    ~1059-1062);
  - the hidden-history test (~684-729, written ~1082-1085);
  - the overlay guard.
  Shadow maps are separate passes. Leaving the eye pass alone leaves them
  intact.
- Elite has no occlusion culling. Everything in the frustum is submitted,
  and early-z rejects what is hidden (`engine-render-pipeline.md` stage 1).
  So the terrain behind the hull is drawn every frame and then discarded.
  That makes (a) possible, but it does not make it cheap: the lighting is
  the problem.

## 2. What EDVR knows about own-ship draws

- **Per pixel: yes, almost free.** The MRT6 slot and depth are written by the
  patched hull pixel shaders, and the compose already tests ownership
  bit-exactly against scene depth (kinematic doc 1928-1940). With a list of
  the own ship's slots, a hull mask is one compare per pixel inside the
  existing temporal pass.
- **Per draw: no.** A pool draw covers an instance range, and the record
  index lives in the instance stream (t33). The per-instance join is exact
  offline (stage 4, "the join is exact via each draw's instance range"), but
  the CPU does not read it at draw time. Splitting a draw by instance would
  be new.
- **Which records are "mine":** `optimized_station_core.h:787-809`
  (`kOwnShipRadiusM = 40`, `ownShipExempt`) and `optimized_station.cpp`
  (`setOwnPrefixAtEngage`, `g_ownCandidateName`) exist, but only in the
  station seal's context. The rig/record tables of the engine motion path
  (stage 2: rig -> collection -> records) are the place to pick out the
  smallest rig that contains the camera. A new per-frame "own slots" list
  would be built from that. It needs a rule for SRV, on-foot, carriers and
  multicrew (no own hull: the feature is off).

## 3. Precedents

- The UI layer redirects the game's own draw by binding EDVR's target
  for that one draw (`ui_layer.cpp` 2655 `beginInner`, 2435 `restore`).
  Draws that write depth or stencil get a second, colour-less issue into
  the game's depth. Its composite runs after DLSS (`uiLayerComposite`
  7728). Its misclassification history is the warning for any draw-level
  hull route: unnamed panel pairs smeared, and world screens were taken by
  mistake (`ui-layer-2026-09-23.md` 41, 430-450, 2078).
- The weapons-fire layer needed a tonemap-difference composite, because a
  post-tonemap add over a lit background was 40-66/255 off at p90
  (ui-layer doc 2068-2077). A hull composited after the game's tonemap has
  no such problem if it is cut from the game's own final image, as in (e).
- `vr_world_route` re-issues one draw immediately with PS slot 0
  swapped. Nothing in EDVR captures a draw to replay later.
- CPU: EDVR's draw path costs 283-349 ns a draw. Re-issues are negligible
  at a few per frame (`vr-draw-path-cpu-2026-11-01.md` Status).
- GPU (gpu pass profile, offline tables, 4 frames, low confidence):
  - scene_gbuffer 0.7-1.1 ms;
  - lighting_volume 0.1-0.6 ms;
  - EDVR's passes 4.9-5.8 ms of an 11-12 ms frame;
  - DLSS evaluate ~0.94 ms in the flat profile (`flat-cz-fps-2026-10-11.md:330`).
  Key: `hotkey.gpu_pass_profile = "F9 60"`; read the `gpu pass profile:`
  lines.

## 4. Options

| | What it takes | GPU / CPU | Patch fragility | If it fails, the pilot sees |
|---|---|---|---|---|
| (a) hull out of the G-buffer, re-issued into a second G-buffer and re-lit | a PS patch that discards own slots in the eye pass; a second issue of the hull families into a private G-buffer and depth; then re-running the deferred lighting, fog, transparents and post on it | lighting + post a second time: several ms; second issue ~0.1-0.3 ms CPU in space, far more at a settlement | the lighting and post sequence is the least mapped part; every Elite patch can move it | no hull, a flat-lit hull, a hull with wrong fog, or particles in front of it |
| (b) separate DLSS feature for the hull | a second NGX feature per eye on a hull-only image | +~1-2 ms a frame | low | nothing gained: the world behind is still unseen |
| (c) the world drawn a second time without the hull, for DLSS only | replay of the whole eye pass | doubles the eye pass's CPU and GPU | very high | frame rate collapse |
| (d) hull reused from the previous frame | an EDVR-kept hull layer, re-lit nowhere | cheap | low | stale lighting on the hull in turns (sun direction changes 1-3 deg a frame) |
| **(e) hull hole-fill + layered composite** | per-eye world cache (colour + depth at render resolution, kept from earlier frames); before DLSS, hull pixels (MRT6 slot in the own list) get the cached world reprojected by the camera term, with world MVs and depth; DLSS then sees a world-only frame; after DLSS, the hull, cut from the game's own frame and accumulated in view space (it is static there), is composited by depth under the cockpit and glass layers | estimate 0.5-1.5 ms an eye (splat + fill + hull accumulation + composite); no extra game draws | low: keyed on the families engine motion already keys; game lighting is untouched | where the fill has no data (terrain never seen) it falls back to today's behaviour; a wrong own list makes the hull vanish or turns another ship into an overlay; the hull edge needs its own AA (from accumulated jittered coverage) or it shows jaggies |

Notes on (e):

- DLSS's history is its own previous output. If no input frame contains
  the hull, the history behind the hull is terrain. It is carried by the MVs
  EDVR writes under the hull. When the wing moves off that terrain, DLSS
  accepts it instead of showing one jittered sample.
- The fill's quality decides the result. Terrain that slid behind a wing
  emerges after about wing width / speed frames (roughly 4-12 at 25
  px/frame). It was seen before and can be reprojected with its depth. The
  FG disocclusion fill (FG doc entry "2026-11-08: torn terrain behind the
  hull") is the closest existing code to reuse.
- Frame generation gains most: the synthesizer can warp the world layer
  with world MVs and the hull layer with near-zero MVs, then composite. The
  hull edge then has no disocclusion at all.
- Hard parts:
  - the composite order: cockpit nearer than hull, canopy glass over hull,
    the HUD layer last (already after DLSS);
  - hull-edge AA at output resolution;
  - particles and lights between canopy and hull, which are rare.

## The deciding experiment (offline, existing dumps)

On `eye_131530778003` (slide-away band, 2-5%) and `eye_132100543006`
(slide-into band, 25-37%), 16 frames each:

1. Hull mask from `EngineSlots.bin` and `EnginePool.bin`: the slots of the
   record(s) owning the 20-120 m pixels at path 11. Take the largest
   record(s) for this desk test.
2. For each frame k, forward-splat the non-hull pixels of frames k-1..k-8
   (C, Z, the camera rows in `motion.csv`) into k's view, nearest depth
   wins.
3. Measure over the band pixels (non-hull in k, hidden bit 16, within 32 px
   of the hull mask):
   - **coverage**: the share the fill reaches;
   - **error**: luma error of the fill against the raw C(k) at those pixels,
     against today's P(k) error there (from the FG doc entry, Laplacian
     P/C 1.15-1.26).

Verdicts, written before the run:

- Coverage at or above 70% and median luma error at or below 6/255
  (the open-terrain P residual): (e) is worth a build. The next step is a
  compile-only instrument that writes the fill under the hull in the dump
  only, without feeding DLSS, flown once.
- Coverage under 40% in the slide-into case (the case the pilot reported):
  the band is mostly terrain never seen, and (e) cannot fix it. Then the
  only remaining lever is frame rate, as the 2026-11-08 entry already says.
- In between: build (e) for frame generation only (layered extrapolation),
  where any coverage helps, and leave DLSS alone.

Flight confirmation, only after a build: in the temporal pass's window
line, a counter of hidden-history pixels along the hull edge, which
should fall to the fill's uncovered share. The pilot's view of the wing
gap in a turn, against `fix` off.
