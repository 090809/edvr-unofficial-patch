# The own hull as a separate layer: feasibility (2026-11-08)

Question: can EDVR keep the pilot's own hull (the parts outside the canopy,
e.g. an Anaconda's front V wings at 40-80 m) out of what DLSS and frame
generation see, have the world behind it, and put the hull back on top?
Motivation: `design-frame-generation-2026-10-26.md`, entry "2026-11-08:
EDVR's base frame, not frame generation: the wing gap smears with FG off".
The smear is terrain that is newly uncovered at the hull edge every frame
(20-30 render px a frame in a turn at 45 fps). DLSS has no history for it.

## Status

* **State (Step 1 v2 built, not flown):** section "Step 1 v2 build" at the
  end. v1 flew (section "Step 1 flight"): the mechanism works, the verdict
  was unreadable. v2 picks the own ship per record with a GPU own mask
  seeded from the first hull draw after the shell, filters the neighbours'
  pixels out of the private G-buffer, re-issues every lighting draw and
  arms up to three eye dumps itself. **Next flight:** fly over terrain with
  `diag.hull_layer = dump` and `fix.temporal_aa` on, no key presses; then
  `edvr_log.py --health` and `python tools\hull_layer_compare.py
  <edvr_logs>\eyes\eye_<stamp>` for each auto dump.
* **Temporary keys of this arc:** `diag.hull_layer` (off | dump).
* **Option (e), the hull hole-fill, fails** for the case the pilot reported
  (section "Result"): in dump 132100543006 only 5.8% of the band was visible
  in the previous 8 frames; 88% was still behind the ship after 8. Layering
  may still help frame generation and DLSS.
* **Draw order (section "The eye pass's draw order"):** option (a) is
  cheaper than Finding 3 says. The own ship is one contiguous block of 37-40
  G-buffer draws (54-57 records) in all six captures; the terrain is drawn
  after it and fills its pixels once the block is left out. The five offline
  checks passed: 100% of the keyed hull pixels at 10-150 m are the block's
  records, and the ship has 6 forward draws an eye. The composite needs a
  re-issued tonemap, since DLSS runs after it.
* **Binding map (section "The binding map"):** G-buffer RT3 is the HDR target
  itself (emissive); the lighting's second output is an R16 target nothing
  reads in-frame. Every input except AO (a compute chain) and the tonemap's
  exposure is made inside the eye pass. Open design items: the AO
  re-dispatch and the hull layer's anti-aliasing.
* **Findings 1-2:** the hull is a deferred G-buffer object (pool families
  vs_66DE, 61AE, AACF, DE54, plus the BBE5/DB3E decals) in the world's
  four-target G-buffer and D32 depth; the cockpit shell is vs_BFE5 / ps_DB79.
  EDVR's engine motion path already marks these pixels (MRT6).
* **Finding 3** (re-rendering costs several GPU ms) was wrong for (a): ~80
  immediate re-issues an eye and about one lighting pass.
* **Open questions for Sean (need `analysis\`):** where the own ship's rig is
  reachable each frame outside a station, and whether its records carry a
  stable "player ship" flag (`optimized_station_core.h:787-809` finds it by
  camera containment or by name prefix at a station engage).
* **Ruled out (this study):**
  * ruled out: isolating the hull by stencil, because hull pixels carry
    stencil 0/4 like the world (kinematic doc, 2026-11-02 entry).
  * ruled out: a separate DLSS feature for the hull alone (b) as a fix,
    because it gives the world behind the hull no history. It is useful only
    on top of a world-only input.
  * ruled out: skipping the hull at draw level by shader pair, because
    those pairs also draw other ships and stations in the same instanced
    calls.
  * ruled out: option (e), the hull hole-fill, for the pilot's case, because
    the band was not in any recent frame (section "Result").

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

## Result (2026-11-08, offline, no build)

Method as above, with changes forced by what the dumps hold:

- Per-frame depth is not in the dump (Z is frame 0 only). The walk back
  therefore uses frame k's own terrain vector for the first step. Each
  later step uses the median world vector of frame j in a 65 px window.
- Hull = decision path 11 with predicted depth over 10 m. "Seen" = world
  path (2), not hull, and its 3x3 neighbours world too.
- Crop: 1400x1400 (the D crop). MV sign: previous = current + MV, checked
  per dump (median one-step error 7.9 against 26.3, and 0.5 against 2.9).

Script and full tables: shared notes folder, `hull-fill-20261108\`
(`hull_fill_test.py`, `fill_out*.txt`). Results excluding the H6 frames
(rows unchanged, terrain MV 0):

| dump | band px, frames 1-15 | seen within 8 frames | fill error p50 / p90 (luma) | open-terrain floor p50 |
|---|---|---|---|---|
| 131530778003 (slides away, 2-5% of gap) | 36,000 | 44-99% a frame | 7-34 / 29-130 | 6-9 |
| 132100543006 (the reported smear, 25-37% of gap) | 318,910 | **5.8%** | 28.5 / 105.7 | 0.5-6 |

- In 132100543006, 86-97% of band pixels a frame were **still behind
  the ship or cockpit after 8 frames** (250-340 px of terrain travel).
  Only 2-7% left the crop. So the crop is not the cause: that terrain
  was hidden longer than the dump's whole history.
- Even where a fill was found, its error is 3-5 times the open-terrain
  floor. That comes from the walk's motion model (no depth, a constant
  local flow), not from the fill idea, but it does not change the
  verdict.
- **Verdict, against the pre-written thresholds: below 40% in the
  slide-into case. Option (e) cannot fix the reported smear.**
  - ruled out: filling the band from earlier frames (e), because 94%
    of it was not visible in any of the previous 8 frames (dump
    132100543006).
  - Options (a) and (c) remain the only ones that can supply that
    terrain, since the game draws it every frame and depth discards it.
    Their cost (re-lighting and post, several ms) was already judged too
    high. A lower frame rate widens the band.
- **What remains:**
  - the band is DLSS's limit at this frame rate (FG doc 2026-11-08);
  - for frame generation, layered extrapolation (world and hull warped
    separately) still removes the hull-edge disocclusion *between* game
    frames, where the world under last frame's hull is not needed. That
    is a separate design question for the FG arc, not a fix for DLSS.

## The eye pass's draw order (2026-11-08, offline, no build)

Question: can the own hull be split off at the G-buffer, before the lighting,
and what would re-lighting it cost? Source: the eye run ledgers
`edvr_logs\pool\draws_<stamp>.bin` (v2 rows: VS, PS, RT token, instance
range; `tools\eye_run_ledger.py` format), `inst_*` and `pool_*` for the
records. Six captures: 132100543006 (terrain, the smear), 222433569001,
225627723003, 120812713001 (space), 152830339005 (station), 202822011003
(busy station, 5,406 draws). Scripts: shared notes `hull-fill-20261108\`
(`draw_order.py`, `hull_block.py`, `block_pos.py`).

The frame, the same in all six (L then R where the stage is per eye):

1. Depth-only prepass into each eye, null PS, ~40-50 draws an eye
   (`ACE405F4`, 26k-104k vertices a draw, not pool draws: terrain). The
   hull is not in it.
2. G-buffer, left eye, then right. Inside it: cockpit shell (`BFE5` x15),
   pool draws, then **the own ship as one contiguous block of 37-40 draws**
   (AACF 10, DE54 13-15, 66DE 13, 61AE 1; 1-8 instances a draw), then the
   terrain (`6C320453`, `72BDD292`), then more pool draws.
3. Hull decals (`BBE5`) as their own run after each eye's G-buffer, 22-25
   draws an eye (hundreds at a station: other ships').
4. Screen-space passes, 3 draws an eye (`DEF19B03`, `53211E8C` x2).
5. **Lighting: 4-12 draws an eye** (`7E38A6AA`, `8289669D`, `24DE25E4`,
   `0357BBB2`, `963B52C7`), instanced volumes, 279-2,693 instances.
6. Forward and transparents into the lit target, 57-200 draws an eye
   (34-156 of them pool draws).

The block is the own ship:

- Its records are the same 54-57 in every capture and every frame, 60-75 m
  apart, and none of them is drawn by any other hull-family draw. At the
  busy station the 32 other ships' records are all outside it.
- The same 11-13 records also take the hull decals.
- Not yet checked: that the camera sits inside the block's extent (needs
  `camera_<stamp>.json` and the pool's frame of reference).

What this changes:

- The split can be per draw, not per pixel. The own ship arrives as one
  batch, not mixed into other ships' instanced calls. The ruled-out line
  above is about a rule by shader pair, and it stands; a rule by batch
  (the family block after the cockpit shell, or by its records) is new.
- The terrain is drawn after the hull with the prepass's depth. With the
  hull block left out of the main G-buffer, the terrain fills the hull's
  pixels by itself.
- Re-lighting a hull layer is ~10 draws an eye, not a re-run of the
  pipeline: re-issue the block (~40) and its decals (~22) into a private
  G-buffer and depth, then the 3 screen-space and 4-12 lighting draws with
  those bound. That is ~80 immediate re-issues an eye, ~25-50 µs of CPU at
  the measured 283-349 ns a draw. GPU: about lighting_volume's 0.1-0.7 ms
  again, less if scissored to the hull, plus the terrain now shaded under
  the hull. **Finding 3's "several ms" was wrong for (a).**

### The five checks (same day, offline; scripts `slot_check.py`, `forward.py`, `rdef.py`)

1. **Pixel ownership: passed.** EngineSlots (MRT6, code = 2 x slot + 1)
   against the block's records of the matching ledger frame (slots frame N
   = ledger frame N+1): **100% of the keyed pixels at 10-150 m** belong to
   block records, 451,748 px in 132100543006 and 427,584 px in 222433569001.
   So do 46-48% of the keyed pixels under 10 m (the ship's own near parts;
   the rest is the cockpit shell). One frame off, the match is 0%: record
   indices are reassigned every frame, so the own list must be rebuilt per
   frame from the block, never cached.
2. **Lighting inputs: rebindable.** From the shader notes in
   `design-flat-temporal-aa-2026-09-23.md` (1818, 4616-4624): the light
   draws load depth at t0 and the G-buffer at t1-3, and write two targets.
   `8289669D` / `C6E6E419` (point and spot volumes) adds a structured light
   list at t0 of the VS. `7E38A6AA` is the full-screen pass. The 3
   screen-space draws are a copy (`DEF19B03`) and full-screen triangles
   with inert PS (`53211E8C` / `B403F48C`). These shaders are not in this
   machine's `edvr_logs\shaders`, and their RDEF is stripped, so the
   exact slot list is a check for the first build's census.
   **Atmosphere and sky are in the forward pass**, not in the lighting:
   `41E245D4` and `D95905C1` (atmosphere, depth read as a texture) and
   `F8FA801F` (sky), one each an eye, right after the lighting. In the world
   layer they see world depth, which is right for the terrain behind the
   hull. The hull layer needs those 2 draws re-issued against its own depth
   (inscatter over 40-80 m is small, but the colour shift is visible in a
   thick atmosphere).
3. **Forward pass: 6 own-ship draws an eye.** In all three captures
   checked, exactly six forward draws take block records: `88DCF116`
   (the laser), `025B4B9F` and `0B5981F2` / `C5A5C7E8`, `F464E27C`,
   `6DB587D2` and `2B3F53DD` / `B2DE0A41`. They move to the hull layer like
   the block. Everything else in the forward pass (HUD, sky, starfield,
   atmosphere, billboards) stays in the world layer. The flight HUD
   `B7790CBF` is routed by the UI layer already.
4. **Composite: after DLSS, through a re-issued tonemap.** EDVR's temporal
   pass, and so DLSS, runs on the tonemapped eye (`black-holes.md` B4;
   `cockpit-hud-layer-design-2026-09-27.md` 117). So the hull layer cannot
   ride the game's tonemap. It needs the game's own tonemap draw re-issued
   on it, as the UI layer's tonemap re-issue does (do not transcribe the
   tonemap: the cockpit HUD design rejects that). Then it is upscaled and
   composited by depth against the main depth, which keeps the cockpit in
   front. Side effects to check: exposure is metered without the hull (the
   world-only image), and bloom from the hull's lights is lost unless the
   layer's own bloom is added.
5. **Shadows: unchanged.** The ledger has no depth-only eye draws (RT -1)
   in any of the six frames, so the shadow maps are drawn outside the eye
   pass. Leaving them alone keeps the hull's shadow on the terrain and on
   itself.

**Re-issue budget per eye:** the block (~40), its decals (~22), the
screen-space passes (3), the lighting (4-12), the 6 forward draws, the
atmosphere (2) and the tonemap (1-3). That is about 80-90 draws, all
immediate. The hull layer's own anti-aliasing is still open: a second DLSS
feature (~1 ms) or EDVR's TAA on a layer that is static in view.

### The binding map, from an existing census (same day)

The pilot's flight at 13:14 already carries a full draw census: census 7 in
`edvr_gfx_20261108_131109.log`, frame 1, 597 eye draws. It has no `mt=`
(render targets 1-3), which commit 6727f7ff adds. Script: shared notes
`census_hull.py`. Left eye (the right eye is the same at #405-552):

| Stage | Draw | Writes | Reads |
|---|---|---|---|
| G-buffer | hull block (`DE54` ...) | RT0 f23 `E76C20`, RT1-3 not recorded; depth f19 `E78A60`, GEQUAL, write on | material textures, the pool at VS t33 |
| Depth copy | #237 `DEF19B03`/`CB95394B` | f39 `E76EE0` | the eye depth `E78A60` |
| AO apply | #429 `DEF19B03`/`06CD42A9` | blends into f27 `E771A0` (a G-buffer target) | a 704x612 f60 target, made offscreen |
| Shadow mask | #430-431 `53211E8C` x2 | f60 `E77F60`, stencil 10 | depth copy, G-buffer `E75BA0` and `E771A0`, a 1024x512 lookup |
| Lighting | #432-435 (`7E38A6AA`, `8289669D`, `24DE25E4`, `0357BBB2`) | HDR f26 `E77460` (R11G11B10), additive | depth copy, G-buffer `E76C20`, `E75BA0`, `E771A0`, shadow mask; `24DE25E4` also a 1536x1536 shadow map and an 896-byte light list |
| Sky, atmosphere | #443 `F8FA801F`, #446 `41E245D4`, #447 `D95905C1` | HDR `E77460`; `41E245D4` writes depth too | depth copy, sky and LUT textures |
| Own forward | #450-480, the six pairs | HDR `E77460`, depth test on | their own textures and the pool, nothing screen-space |
| Tonemap | #592 `2D78DC3F`/`99C21CEB`, a 3-vertex triangle | f27 `E77CA0` (8-bit eye) | t1 = HDR `E77460`; t0 = `@2000`, not in the id table (an offscreen product: exposure or bloom) |

What it settles:

- **Closed by the `mt=` flight** (build 639c02a9 = 6727f7ff
  cherry-picked, `edvr_gfx_20261108_173723.log`, census 1 = capture
  180353328001, complete: 1,955 ids interned, no overflow):
  - The hull block and its decals write four targets: RT0 f23
    (normal or material), RT1 and RT2 f27, and **RT3 = the HDR target
    itself** (f26, the one the lighting then adds into), plus depth f19.
    The G-buffer's emissive goes straight into the HDR image. A hull layer
    therefore needs its own HDR target as RT3, cleared to zero. The
    lighting adds into it, and the world's HDR gets no hull emissive.
  - The lighting draws (now 11 an eye: `7E38A6AA`, `8289669D`, `963B52C7`,
    `24DE25E4`, `B121A79E`, `0357BBB2` x3+) write RT0 = HDR and RT1 = an
    eye-size f53 (R16) target. No recorded draw or dispatch in the census
    reads that R16 target (PS t0-7, VS t32-39, compute SRVs). For the
    re-issue a scratch R16 target is enough; whatever reads it later keeps
    the world's.
  - Census 2 (capture 180426745002, docking) stopped at the default
    4,096-line cap after frame 0 (26,231 draws). For station dumps set
    `advanced.census_lines = 16384`.
- Every screen-space input the hull layer needs is made inside the eye
  pass from depth and the G-buffer: the depth copy, the shadow mask and the
  lighting. All of them can be re-issued against a private G-buffer.
- Two inputs come from outside the eye pass:
  - **AO** is a compute chain. Depth is reduced to 176x153 (f39), then a
    dispatch (`ch=D31E7812`, 44x39 groups) writes a 704x612 f60 target,
    and #429 blends that into G-buffer `E771A0`. On a hull layer the AO
    would be the world's. Either re-dispatch the chain on the hull's depth
    or apply none to the hull.
  - **The tonemap's t0** (`@2000`) did not resolve (census 7's id table
    was full: 2,048 interned, 2,547 overflowed), and no draw, copy or
    dispatch in the census writes it by token. The tonemap reads only t0
    and the HDR, so no separate bloom texture enters it. t0 is most likely
    the exposure value from the compute exposure chain
    (`gpu-pass-profile-2026-10-12.md`, class `exposure`). Then the
    re-issued tonemap applies the world's exposure to the hull, which is
    what the game does today.
- The atmosphere writes depth, so in the hull layer it must run after the
  own forward draws or with depth writes off.

**Next: no flight of its own.** Every eye dump (`[hotkey] dump_eyes`)
already arms a three-frame census with offscreen draws (`DC eye capture
link`). Commit 6727f7ff adds `mt=` to the census, and it compiles (compile-only,
2026-11-08). Carried into the next flight build of any arc, one eye dump over
terrain names G-buffer RT1-3 and the lighting's second target. Read the dump
with `census_hull.py <log> --census N`. The open design items are then the
AO re-dispatch and the hull layer's anti-aliasing.

## Step 1 flight (2026-11-08 19:20, build c80a4ef1, dump 192424067001)

Log `edvr_gfx_20261108_192046.log`, build verified (`--expect-build
c80a4ef1`). VR, OpenXR (EDVR runtime), 2817x2448 render per eye.

- **What worked.** The layer ran (`state=applied`). It found a block in
  every eye-frame and re-issued it into private targets, and all three
  stages were substituted. The dump wrote HullHdr, GameHdrSun and
  HullDepth for both eyes. The private depth covers the lower 34.5% of the
  view, where the nose is. The game's frame was not touched.
- **The verdict cannot be read.** `hull_layer_compare.py` says REFUTED
  (99% of the private hull black), but GameHdrSun is black too: 0.1% of
  all pixels are non-zero, terrain included. The comparison point is
  wrong, not the layer.
  - ruled out: `7E38A6AA`/`7CECABDE` as the sun pass, because the game's
    HDR right after it is black on 99.9% of the frame (dump 192424067001).
    The sun and the other lights are in the volume draws after it.
    `24DE25E4` and `B121A79E` read the 1536x1536 shadow map.
- **The block over-takes.** The census of the same dump has 92 hull-family
  G-buffer draws an eye, not 39. Another ship's draws follow the own
  block with gaps of 1-13 pool draws. The layer took 84 in the dump frame.
  - ruled out: recognising the own ship as the contiguous hull-family run
    after the cockpit shell, because the game draws front to back and a
    neighbouring ship joins the run (census of 192424067001, 92 against 39
    an eye).
- **Two code bugs:**
  - A run never ended at the terrain (`ended=gap` on all 2,858 blocks).
    The "pool draw" test reads whether VS t33 is bound, but D3D state
    persists, so t33 is still bound on the terrain draws.
  - The second dump (192435503002) captured nothing. The 16-mirror cap was
    full of the first targets when the game made a new eye depth (log
    19:24:25).
- **The ledger lost its instance streams.** Both dumps of this flight
  wrote "0 instance streams (... stride 0)". Every earlier dump today
  wrote 19, including 180353328001 on 639c02a9. Without them the records
  per draw cannot be joined, which item 2 below needs. The suspect is the
  hull layer: it was on, and the code otherwise differs from 639c02a9 only
  by the FG gate commits. Check this with the key off before v2.
- **Later dumps of the same flight (19:28-19:30, key still on) have
  instance streams again** (19 each: 192850710003, 192950193004,
  193014549005, 193029413006, 193050273007). So the first two empty
  ledgers were not the layer's doing. Script: shared notes
  `own_records.py`, `cb_per_object.py`.
  - In all five, the **first hull-family G-buffer draw after the cockpit
    shell belongs to the own ship**. Clustering each draw's records by
    position (120 m) gives object A with exactly 54 records every time.
    100% of the keyed pixels at 10-150 m are A's, and 98-99% under 10 m.
  - Neighbour ships are drawn interleaved with A, for example
    `ABBBCAAAA...AACCBB` and `AAAA(x5)AAAAAAAA x AAAA...xxxxx`. So no run,
    gap or order rule can isolate A.
  - Per draw, nothing CPU-visible separates A from a neighbour. VS cb0 is
    unbound (`c=-`). VS t33 and t38 are the same views for all draws. Only
    the mesh range (`ia=` start index, base vertex) differs, and a
    neighbour of the same ship type would share it.
  - ruled out: telling the own ship's draws apart on the CPU by binding,
    order or contiguity, because neighbours interleave and share every
    binding (census 6 = 193029413006: 71 hull draws, 26 of them a
    neighbour's, all with identical tokens).
- **Own-ship identity on the GPU, per instance (the design for v2):**
  1. **Seed.** At the first hull-family G-buffer draw after the shell, the
     CPU knows its instance range (start instance, count) and has the
     instance stream SRV bound. A tiny compute dispatch reads the seed
     records' positions from the pool. It marks every pool record within
     120 m of their centroid in an own mask (2,048 bits). The log line
     prints the mask's count (54 expected for the Anaconda).
  2. **Filter in the pixel shader.** The four hull families are already
     patched by engine motion (MRT6 slot export, `dxbc_engine_velocity.h`).
     Add an own-mask SRV to the patch:
     - in the main pass, discard own instances (Step 2);
     - in the private re-issue, discard all others (Step 1).
     Then every hull-family G-buffer draw is re-issued (70-90 an eye),
     and mixed draws are handled per instance. Decals need the same
     filter.
  3. It depends on `fix.engine_motion` (the patched shaders). Without it
     the layer is off.
- **Next build (Step 1 v2):**
  1. Re-issue every lighting draw of the eye, not the sun alone, and
     snapshot the game's HDR after the last lighting draw, before the sky.
     The comparison is then exact, apart from AO and decals.
  2. Recognise the own ship by record, not by run. Options: a per-draw
     test of the draw's records against the camera (the own ship
     contains it); or keep the run but end it at the first hull draw whose
     records lie outside the first draws' cluster. Settle this offline on
     the ledger of a dump with a neighbour ship first.
  3. End a run on the terrain by its VS, without the t33 test. Evict
     mirrors of game resources not seen for a few frames.

## Step 1 build (2026-11-08, compile-only, not flown)

The hull layer in the eye dump only: a private, lit copy of the own ship's
G-buffer, sun and ambient only. Nothing the game draws or presents changes.
Source: `src\d3d11\hull_layer.cpp` (hull_layer.h says what it does), called
from the four draw thunks in `vscreen.cpp` right after the game's own issue.

**The key:** `diag.hull_layer = off | dump` in `[diag]`, TEMPORARY, default
off, live, VR only (not in the flat profile's list). With `dump` it runs every
frame; an eye dump (`hotkey.dump_eyes`) arms the capture for the next whole
frame. With it off the draw path pays one bool load. With it on, the private
mirrors cost video memory. Per eye at 2817x2448 there are nine of them:
RT0-2, the HDR, depth (8 bytes a texel), the depth copy, the shadow mask
and the R16. That is about 0.25 GB an eye and 0.5 GB for both. Expect the
frame time with the key on to be a little worse; that is not the layer's
production cost.

**What runs, per eye:**

1. *The block.* The run starts at the first hull-family draw (VS 66DE, 61AE,
   AACF, DE54) after the cockpit shell (VS BFE5) into the same eye depth, both
   drawn with four render targets and a depth bound. It holds up to 6 non-hull
   pool draws (VS t33 bound) in a row and ends at a non-pool draw, the terrain
   (`72BDD292...` or `6C320453...`) or a longer gap. The eye is known by the
   depth texture's identity; frames by vScreen's frame boundary. Each block
   draw is issued again into private RT0-3 and a private depth made from the
   game's own descs (RT3 is the private HDR). At the block's first draw they
   are cleared: colour to 0, depth and stencil to the values the game last
   cleared that eye depth to (0 and 0 if no clear was seen). Only RT0-3 are
   bound: engine velocity's slot target at RT6 is not written twice.
2. *The stages,* each once per eye after its block: the depth copy
   (`DEF19B03`/`CB95394B`), the shadow mask (`53211E8C` with `7EAC7196`, then
   `B403F48C`) and the sun and ambient (`7E38A6AA`/`7CECABDE`). Every bound
   target gets a private copy (the f39 copy, the f60 mask, the R16 scratch are
   made at their first use); the depth target must be the eye's private depth;
   every PS resource (t0-15) whose texture has a private copy is swapped for
   it, by texture identity. The game's clears of a texture with a private copy
   are made on the copy too.
3. *Left out, known differences:* the hull decals (`BBE5`/`DB3E`: their depth
   test would pass for other ships' decals against a cleared private depth),
   the AO apply (`DEF19B03`/`06CD42A9`: the private RT2 has no AO), the point
   and spot volumes (Step 1b), the six own forward draws and the atmosphere.

**The files** (edvr_logs\eyes, EDVRTEX1, read by `tools\eye_inputs.py`):
`eye_<stamp>_HullHdr.bin` (the private HDR right after the private sun draw,
R11G11B10, DXGI 26), `eye_<stamp>_GameHdrSun.bin` (the game's HDR right after
its own sun draw: emissive plus sun, the same stage) and
`eye_<stamp>_HullDepth.bin` (the private depth, DXGI 19). Eye 1's names end
in `_e1`. The header's frame is the layer's own frame count.

**The log lines** (graphics log, untagged; `edvr_log.py --health` reads them
as "hull layer"):

```
hull layer: diag.hull_layer=dump (TEMPORARY; ...)          at each change
hull layer: state=applied frames=2700 eye_frames=5400 blocks=5400 not_found=0
  hull_draws=37/39/40 gap_draws=120 reissued=232200 clears=5400
  subst=copy:10800,mask:32400,sun:21600 no_counterpart=copy:0,mask:0,sun:0
  ended=terrain:0,non_pool:5400,gap:0,frame:0                every 30 s (one line)
hull layer: dump <stamp> eye 0 frame N: HullHdr WxH format 26 written,
  GameHdrSun written, HullDepth written; block 39 draws     per eye, per dump
```

`state` is `no-eye-pass` (no eye drew the shell: never ran on an eye),
`no-block` (it ran and found nothing), `partial` (the block was re-issued but
no sun draw was substituted) or `applied`. `eye_frames` counts eye-frames that
drew the shell, `blocks` those with a block, `not_found` frames with no block
in either eye; `hull_draws` is min/median/max of the block's hull draws an
eye (the ledgers say 37-40); `subst` counts targets and resources swapped a
stage, `no_counterpart` frames where a stage's pair ran with no private eye
behind it (a use of the same pair before the block also counts there);
`ended` says how each run ended. No GPU time yet: the CPU time is the
`hull_layer` caller of the `draw reissue CPU:` line.

**Flight plan:** fly over terrain with `diag.hull_layer = dump`, take one eye
dump, land. First `python tools\edvr_log.py --target steam --expect-build
<commit> --health`: "hull layer" must be OK, with hull draws 37-40 an eye.

**Offline comparison:**
`python tools\hull_layer_compare.py <edvr_logs>\eyes\eye_<stamp>` (add
`--eye 1` for the right eye). Hull pixels are where the private depth is not 0.

- **Confirms Step 1:** HullHdr equals GameHdrSun on the hull pixels within a
  small tolerance (the tool: median luma ratio 0.95-1.35 and at least half the
  hull pixels within 5%), except where the decals or the game's AO differ
  (the layer has neither; where the game's AO darkens, the layer reads
  brighter).
- **Refutes it:** the layer is black or unlit on the hull (the lighting or
  shadow-mask bindings are wrong), offset (a whole-pixel shift matches better
  than none), or the private depth is empty (the block rule found nothing).

Not verified before the flight: the hash constants are from the 2026-11-08
ledgers and census; the clear values are the game's as the clear hook sees
them; that the shell and the block draw with four targets bound; and that no
third pass draws the shell into a G-buffer of its own (`eye_frames` would be
more than twice `frames`).

## Step 1 v2 flight (2026-11-08 20:19, build 472aaccf): no result, a shader keyword

Log `edvr_gfx_20261108_201922.log`, build verified. The layer never ran:
`hull_layer_count_cs` failed to compile at run time (`error X3000: syntax
error: unexpected token 'Pass'`). `Pass` is an HLSL keyword, and it was the
name of the cbuffer shared by the count and filter shaders. Every window
logged `state=no-shader`. Five dumps (202246398001 to 202450601005) carry
only GameHdrLit. A few eye-frames also logged `no-slot-target` (2-10 a
window); read that again on the next flight.

The fix renames the cbuffer, and adds `tools\hlsl_embedded_check.py`, gated
in `build.bat`. It compiles every `// hlsl-check:` shader of a source with
the system `d3dcompiler_47.dll`, so a run-time shader cannot fail first in a
flight. It reproduces this error on the 472aaccf source.

## Step 1 v2 build (2026-11-08, compile-only, not flown)

The fixes the v1 flight asked for (section "Step 1 flight"), still in the
eye dump only: nothing the game draws or presents changes. Source:
`src\d3d11\hull_layer.cpp` (hull_layer.h says what it does). Same key,
`diag.hull_layer = off | dump` (TEMPORARY, default off). It now also needs
engine motion (part of `fix.temporal_aa`, no key of its own): its patched
pixel shaders write the record slot at MRT6, and the filter reads it.

**What changed:**

1. *The seed and the own mask.* Per eye-frame, the first hull-family
   G-buffer draw after the cockpit shell is the seed. Its instance window
   (the stride-8 instance stream, record index first) and the pool (VS t33,
   336-byte records, position at bytes 16-27) are copied. One compute
   dispatch then marks every pool record within 120 m of the seed records'
   centroid in an own mask (one uint a record, sized from the pool's view).
   The count is read back 2+ frames later with `DO_NOT_WAIT`, never a stall.
   If the seed draw has no R32G32 slot target at RT6, the eye-frame does
   nothing (`no_slot`, said once as `no-slot-target`).
2. *Every hull-family draw, filtered per pixel.* From the seed to the eye's
   depth copy (or the next eye's shell), every hull-family G-buffer draw is
   issued again into private RT0-3, a private depth and a private slot
   target (cleared to -1, no slot). A full-screen pass then sets RT0-3 to 0
   and the depth to the clear value (SV_Depth, func ALWAYS) wherever the
   slot names a record outside the mask. A count pass counts those pixels
   (`filtered_px`). Neighbours in front of the own ship leave holes there,
   which is accepted. The run, gap and terrain rule and its t33 test are
   gone. Decals stay out.
3. *All the lighting.* Every draw with VS `7E38A6AA`, `8289669D`,
   `963B52C7`, `24DE25E4`, `B121A79E` or `0357BBB2` is re-issued, with any
   PS. Every target and PS resource that has a private copy is swapped; the
   shadow map and the light list stay the game's. Another VS that writes the
   eye's HDR between the shadow mask and the sky is logged as an unknown
   lighting pair. The depth copy and the shadow mask are unchanged.
4. *Fixes.* A private copy whose game texture was unseen for 120 frames is
   dropped (the cap is now 32). This is the v1 dump that captured nothing.
5. *Auto dumps.* The layer arms the eye dump itself (the hotkey's own call,
   `temporalPassArmEyeDump`) once it has seeded over terrain on 600 frames in
   a row with an own mask of 20-200 records. At most 3 a session, 60 s
   apart. The hotkey still works.

**The files:** `eye_<stamp>_HullHdr.bin` (the private HDR after the eye's
last private lighting draw), `eye_<stamp>_GameHdrLit.bin` (renamed from
GameHdrSun: the game's HDR after its last lighting draw, before the sky) and
`eye_<stamp>_HullDepth.bin` (the private depth after the filter). Eye 1's
names end in `_e1`.

**The log lines** (graphics log, untagged; `edvr_log.py --health` reads them
as "hull layer"):

```
hull layer: state=applied frames=2700 eye_frames=5400 seeds=5400 no_seed=0
  no_slot=0 refused=0 own=54/54/55 readbacks=5400 filtered_px=0/1200/9000
  hull_draws=70/84/92 reissued=453600 no_slot_draws=0 clears=5400
  filters=5400 subst=copy:10800,mask:32400,light:59400 light_draws=11/11/11
  no_counterpart=copy:0,mask:0,light:0 unknown_light_pairs=0
  ended=copy:5400,shell:0,light:0,frame:0 terrain_frames=2700 auto_dumps=0
                                                          every 30 s (one line)
hull layer: auto dump armed (reason: own mask 54 records, seeded over terrain
  for 600 frames in a row; 1 of 3 this session)
hull layer: dump <stamp> eye 0 frame N: HullHdr WxH format 26 written,
  GameHdrLit written, HullDepth written; own=54 filtered_px=3000 seed=3/3
  hull_draws=84 light_draws=11                              per eye, per dump
hull layer: unknown lighting pair vs <16 hex> ps <16 hex> writes the eye's
  HDR between the shadow mask and the sky (...)             once a pair
```

`state` is `applied`, `partial` (seeded, no lighting substituted),
`mask-pending` (seeded, no readback yet), `no-shader`, `no-slot-target`,
`seed-refused`, `no-seed` (the shell, no hull draw after it), `no-shell`
(hull draws, no shell) or `no-eye-pass`. `own` and `filtered_px` are
min/median/max over the window's readbacks; `seed=` in the dump line is the
seed records read / the seed's instances.

**Flight plan:** with the key on and `fix.temporal_aa` on, fly low over
terrain for 10 s or more at a time, and do not press the dump key. The
layer arms up to three dumps. Then:

1. `python tools\edvr_log.py --target steam --expect-build <commit>
   --health`: "hull layer" OK, `own` near 54 for the Anaconda, three `auto
   dump armed` lines.
2. For each dump: `python tools\hull_layer_compare.py
   <edvr_logs>\eyes\eye_<stamp>` (and `--eye 1`). It reads the own count
   from the dump's log line.

- **Confirms Step 1:** HullHdr equals GameHdrLit on the hull pixels (median
  luma ratio 0.95-1.35, at least half within 5%), with no better whole-pixel
  shift, and `own` is 54 (Anaconda). AO and decals may differ.
- **Refutes it:** the layer is black where the game is lit (the lighting
  bindings), offset, or the private depth is empty. It is also refuted if
  `own` is far from the ship's record count (the seed or the radius is
  wrong), or if the neighbour pixels survive the filter in HullHdr.
- **Inconclusive, not refuted:** GameHdrLit black on most hull pixels (the
  comparison point is wrong again), or the luma close but not within the
  limits.

**Not verified before the flight:**

- the instance stream binding (the stride-8 buffer in vertex slots 0-3,
  offset plus StartInstanceLocation x 8);
- that the pool view starts at element 0, and that pool positions are
  camera-near enough for a float centroid. The first seed logs the view
  once.
- the slot target's format (R32G32_FLOAT) and its clear (-1, no slot);
- the four lighting VS hashes from the `mt=` census, `B121A79E457669E8`
  among them;
- that `temporalPassArmEyeDump` may be called from the frame boundary;
- that SV_Depth with func ALWAYS writes the D32S8 private depth as
  expected. The filter leaves stencil as the neighbour wrote it.
- The shaders compile at the first seed (d3dcompiler_47.dll), a one-time
  hitch.
