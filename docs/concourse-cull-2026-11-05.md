# Concourse cull: on foot, the station outside the bar is not drawn (2026-11-05)

## Status

- **State:** Flight 9 flown 2026-11-06 flat on Steam, log `edvr_gfx_20261106_092241`, build 4eead728. Flight 10 flown 2026-11-06 on 990a2b44, log `edvr_gfx_20261106_095708`: Sean asked for brighter skylights, dimmer bay windows, a middle level elsewhere, and the shop window to clear like the bay. Flight 11 flown 2026-11-06 on 481d97b0, log `edvr_gfx_20261106_110851` (results below; the log hit its size cap at 11:22:26, so the trip back is lost). Flight 12 flown 2026-11-06 on f09f42df, log `edvr_gfx_20261106_114951`: results below. Flight 13 flown 2026-11-06 on eb9b6b8e, log `edvr_gfx_20261106_130939`: the doors can be read. Flight 14 flown 2026-11-06 on a3f17170, log `edvr_gfx_20261106_134043`: the lift cull reaches the cap on the hangar side and barely helps in the concourse. Flight 15 flown 2026-11-06 on 9a1d039f, log `edvr_gfx_20261106_141917`: the lift cull now works both ways, the view masks do what RE said, and they save less than the census suggested. Flight 16 flown on b4f82839, log `edvr_gfx_20261106_150756`: keeping by size refuted. Flight 17 flown on a2fca02e: Sean, of a capture under a skylight, "This looks like the right idea, but if I shift my position, it moves out of alignment. Also not sure what the white dots are?" f50a46a2 flown, log `edvr_gfx_20261106_184018`: two F6 captures; Sean, "it looks decent when stationary, I think with some glass/tinting effects it could work effectively if we capture the whole cube so there's some parallax". He did not step F9, so H43 is not answered. Build 2a140a30 flown, log `edvr_gfx_20261106_192543`: automatic capture refuted. Sean, with a clip: "this is what I was afraid of, I think we need to capture the station as a simple textured geometric shape once". See "A baked shell". Build bad4cf70 flown 2026-11-07, log `edvr_gfx_20261107_032222`: the ride camera test (Flight 20): the view slot follows and the constants do not. See "Flight 20 results". Build 1bba08e9 flown 2026-11-07, log `edvr_gfx_20261107_042506`: the second camera test (Flight 21): the VR camera path does not run in flat, and the log names flat's. See "Flight 21 results". Build ac4ab507 flown 2026-11-07, log `edvr_gfx_20261107_050539`: the third camera test (Flight 22): the whole picture follows a camera moved where flat makes it, so a chosen camera works in flat. See "Flight 22 results". Build 19881a34 flown 2026-11-07, log `edvr_gfx_20261107_060254`: the look test (Flight 23): the station mostly whole from the room's centre, at 7.2 to 8 ms a frame. See "Flight 23 results". Build c1f3ca32 (the source of 61b8e9f6, with a doc commit on top) flown 2026-11-07, log `edvr_gfx_20261107_064844`: the look on a ride up (Flight 24): the station is whole and lit from the first frame the clip caught at arrival, on a second ride. See "Flight 24 results". Build 5cdb2575 installed on Steam 2026-11-07, not flown: the first bake at arrival, shown in the windows (Flight 25). Sean's goal for the windows, 2026-11-06: retain as much of the cull's saving as possible while making the view through them the best. The lift cull is paused at his word: it stays on, with its last change, a release 5 s after shutting. Not merged. Extends `fix.optimized_stations`; the concourse cull is off unless the temporary keys turn it on, but the sealed-hangar withhold is on with `fix.optimized_stations`.
- **Goals (Sean, 2026-11-05):** (1) on foot in the large-station concourse, cull everything outside the bay windows; (2) skylights and the window above the lift bank become a permanent mirrored surface, so missing structure is never seen; (3) approaching the bay windows clears the mirror and stops the cull, so the station interior is visible from there. **Added 2026-11-06:** (4) light behind the mirror, for the impression of a brightly lit station beyond it; (5) with `fix.optimized_stations` on, the same outside draws go while the hangar is sealed.
- **Direction:** identify records by the store's name, keep an allow-list, and make the panes opaque. The fitted hall, the shader-pair lists and the shell read-back from the earlier attempt are not carried over (see Ruled out).
- **Answered by Flight 1:** H1 yes, with named survivors. H2 yes. H3 yes for the bay window: VS `98397963AAEC45D3` / PS `66B08F89E4926C01`, n=36. H5 yes by eye: the room stayed lit. See "Flight 1 results".
- **Answered by Flight 2:** H4b yes, by eye. H7 yes: lifts at 20.2-22.3% of the view, all near the bay. H6b no: the family covers every pane in the room at about 20 index counts, so no count names the exterior. See "Flight 2 results".
- **Answered by Flight 3:** H9 yes as far as the eye went: Sean called the mirror much better and reported no wrong pane, with 0 mark refusals. H10 yes: `culled PlantStack0-4` logged, and his screenshot has no plates. H11 not asked directly: no complaint about the grid, and the log shows `mirror-soft` tried. The side table needed one fix: one mesh drawn in two places flipped within a frame. See "Flight 3 results".
- **Answered by Flight 4:** H14 yes: Sean, "the turn away from the bay windows works well". H12 half: "I don't see anything missing or incorrect on the interior", no planet in his screenshot or his report, and the lists matched 15 of 24 families, but the haze and lasers stayed. See "Flight 4 results".
- **Answered by Flight 5:** H15 yes: `beams and bolts` is no longer on Sean's list, no lasers. H16 half: the wash went with `scatter`, so that is what it was; the ellipses and wedges stayed, so they were not the particles. H13 still open: `flares` matched and no flares in the screenshot; the room was not reported.
- **Answered by Flight 6:** H18 yes: Sean, "the green haze and light fills are gone", with `far glows limited` at about 8000 a second. The room was not reported wrong.
- **Answered by Flight 8:** H23 (a): Sean, "the bright washed out look stays on when F2 is off". Elite's own, and not this arc's.
- **Answered by Flight 9:** H24 half: Sean likes `mirror-brightest`, the best so far, but the pass brings the sun's moving shadows with it. He chose a steady light instead.
- **Answered by Flight 11:** H27 yes by eye: the skylights are the bright ones, up 0.866 -0.500 0.000. H28 half: the bay clears, a skylight looked at directly stayed mirrored with no `clearing pane` line, but the shop did not clear, at 7.3 to 8.3% of the view against 20%. H29 half: the cut is logged going up (784 m), and the way down fell past the log cap. H26 replaced by H30.
- **Answered by Flight 12:** H30 yes: Sean, "bar windows look great". H28 yes for the shop: "Window cleared at the ship services counter". H31 yes: frames in the cabin going down match the view across the room from there, and rise when the doors open on the hangar side.
- **Answered by Flight 13:** H32 yes: the door records move in `c`. H33 yes: the cabin record's distance tells inside from outside. See "Flight 13 results".
- **Answered by Flight 14:** H34 half: going up the ride sat at the cap (6.94 ms), and going down it read 11.8-12.2 ms against 12.2-15 ms. Sean: "No issue on the way down, but saw a small pop on the way up when the doors opened."
- **Answered by Flight 15:** H35 yes: going down the ride read 8.47 ms against 11.8-12.2, with 83% of rigs vetoed. H37 yes, with a smaller gain than expected: see "Flight 15 results".
- **Open hypotheses (the next flight answers them):**
  - **H36** The small pop on the way up, when the doors open, comes from the lift cull. Unknown: Sean had not said what popped or whether earlier rides had it. One candidate: the pane table is forgotten on each visit, and its first half second is visible as the doors open (`changed side` lines at 13:44:16.7).
  - **H38, refuted for the name list:** Sean, "The keep list leaves out large sections of the station interior". `concourse_keep` cost nothing measurable (13.5-13.7 ms against 13.3-14.7 ms with no list and 12.8 ms with the flat fill, one spot, a few seconds).
  - **H41** A view captured from a frame with the cull off behind opaque panes keeps nearly all of the cull's saving and looks like the station. By direction alone it read right at one spot, at no measurable frame cost (8.62-8.70 ms against 8.62 ms), and slid when the camera moved.
  - **H42** The white dots are texels the forward splat left empty, where the cube's density exceeds the screen's, showing the fallback (4.0 on a skylight). Filling from the mips removes them. Confirmed or refuted by eye.
  - **H45** A shell baked once from an unobstructed render looks right and costs about one small draw. It rests on a chosen camera during a lift ride, untested. The smallest test is in "Rendering from the concourse's centre"; Flight 20 flies it.
  - **H46, refuted by Flight 20 for this hook point:** moving view 0's camera before `0x1EA9C0` reaches the view slot and not `cb1[275]`, held or not. Sean, on all three rides: "nothing moved". A write further upstream is H48.
  - **H48, refuted by Flight 21:** `0x283FA20` and `0x2845730` never run in flat, so nothing was written. View 0's camera comes from the call at `0x287D95D`, in `0x287D5A0`.
  - **H49, confirmed by Flight 22 for the picture:** moving the camera on `0x281D190`'s stack after `0x2874290` returns moves `cb1` and the image, with no fault and nothing odd reported from the game. Whether the per-view cull follows the turn exactly has no census behind it yet.
  - **H50, half answered by Flight 23:** Sean, "the station appears mostly whole". Lighting and detail not reported yet; cached shadows, streaming and exposure after a jump not measured. One of the six directions showed only the skybox, and NPCs were left, see "Flight 23 results".
  - **H51, half answered by Flight 24:** at arrival the station can be drawn from the room's centre at once, whole and lit, about 20 ms after the cut. That was a second ride up. A true first ride, with the look on, is still to fly. The time is shorter than first thought: see "Flight 24 results".
  - **H43** A cube keyed by position around a fixed origin, with distance stored, keeps the view aligned as the camera moves and lets captures from different spots agree. It rests on v2 being relative to `cb1[275]` and on `cb1[279]` being the view Z axis, both INFERRED: a wrong row shows as the whole image swimming in the sphere and depth modes, not in the off mode. The three modes on one key compare them in a flight. 14 cube samples a pixel in depth mode, against about 7 in the others; about 1.4 GB. F4 against `mirror-steady` gives the cost.
  - **H40** The black flash Sean sees as the doors open (H36's pop) is the destination room's rigs catching up after a ride vetoed. The release at 5 s tests it, but the lift work is paused, so this is not being chased.
  - **H30** The two bands on the bar and shop windows are the glass shader's reflection, a horizon at eye level, and a flat fill with no glass shader removes them. Evidence: the mark covers 81 to 91% of those panes (n=30 drawn 686924, mirrored 627855), though the lower band is about a third of the pane. Flat fill also makes each pane one draw instead of two.
  - **H31** The lift's frame rate is the room behind the closed cabin. 12.7 to 13.2 ms from 11:22:22 in the cabin with the cull on, as the lift bank was in earlier flights (12 to 13 ms). Census 2: at least 5932 main-view draws, against 3974 standing in the room. Discriminator for whether the cut's change costs anything: `walk survey` frame means in the first 5 s after a cut on the way down. Earlier builds, with the cull still on for those 5 s, read p50 6.95 ms there and 7.17 ms after it.
  - **H27** The glass's lighting normal and the anchor's axes share a frame, so `abs(dot(n, up))` finds the skylights. Unproven: the log's `concourse pane: glow levels ... up X Y Z` gives the axis, and the eye says whether skylights are the bright ones. If every pane looks the same, this is refuted or `up` was not read.
  - **H28** A window that fills the entry share and reads exterior can clear like the bay. `concourse pane: the clearing pane is now n=N (share S%)` and the `concourse clear:` lines with n say which opened. Known risk: looking straight up at a skylight can clear it too, since no camera direction is available here. The same n opens together.
  - **H29** The lift is one camera cut, so the cull ends on it. `concourse cull: off (the camera cut to another room)` confirms. `the anchor record was not seen for 5 s` after a cut means a late sighting re-armed the anchor.
  - **H26** A flat colour added over the mirror, with no light or shadow in it, reads as a bright station behind the glass and stays still as the station turns. It costs almost nothing, a white pixel shader in place of a third full pass. The scene's HDR scale is unmeasured, so F4 steps 0.25 to 4 around a guess of 1. `mirror-brightest` is on F4 to compare.
  - **H25** The families marked `sealedToo` (lasers, plumes, trails, smoke, the planet, its atmosphere, the sun's glare) draw nothing a sealed hangar needs. No census has been taken in a sealed hangar, so this is by reasoning. Flares, the scatter, the particle batches and engine glows stay for that reason. `concourse withheld 30s:` says what matched there.
  - **H22** The hull is a nameless record. Not reported in Flight 8. If it still draws, an F3 press names what is kept.
  - **H20** and **H21** The ribbon: ask whether either trail is left.
  - **H6** `Standard_SocialArea` marks the large-station concourse at other stations too. Still unflown.
- **Temporary keys (all removed when the arc closes):** `diag.concourse_cull` (on, off), `diag.concourse_keep` (extra name prefixes), `diag.concourse_pane_mode`, `diag.concourse_panes`, `diag.concourse_clear_share`, `diag.concourse_mirror_beyond`, `diag.concourse_glow` (sky, angled, other, bay), `hotkey.concourse_cull`, `hotkey.concourse_pane_capture`, `hotkey.concourse_pane_mode`, `hotkey.concourse_view_test` (now the backdrop's capture), `hotkey.concourse_backdrop_parallax`, `diag.concourse_backdrop_radius`, `diag.concourse_glass`, `hotkey.concourse_camera_test`, `diag.concourse_bake_near`, `diag.concourse_bake_dirs`.
- **Hangar to concourse and back (Sean, 2026-11-06):** steps 1 to 3 built, not flown; the seal on foot waits on the survey's numbers. See the section of that name at the end.
- **Ruled out:** see the section of that name. **Issues:** none yet.
- **Answered by Flight 20:** H47 yes: the game writes this camera every frame before `0x1EA9C0` (284 of 284 held writes overwritten), so a write need not be put back.
- **Next flight:** Flight 25, the first bake at arrival, on 5cdb2575, as a true first ride; the list is in its section. Keys: `hotkey.concourse_camera_test` Insert in both Steam inis (F10 is Elite's screenshot key, and F11 is the photo camera in Sean's bindings); `diag.concourse_glass` in `edvr-flat.ini`; `hotkey.concourse_backdrop_parallax` F9 in both Steam inis; `diag.concourse_backdrop_radius`; `hotkey.concourse_view_test` F6 in both Steam inis; `hotkey.concourse_cull` F2, `hotkey.concourse_pane_capture` F3, `hotkey.concourse_pane_mode` F4 in both Steam inis, `diag.concourse_glow` in `edvr-flat.ini`, and `log.max_mb = 200` there for these flights only (two censuses filled the default). Keys in both Steam inis: `hotkey.concourse_cull` F2 (the cull and the mirrors), `hotkey.concourse_pane_capture` F3 (logs a ranking by screen coverage and the kept names, changes nothing), `hotkey.concourse_pane_mode` F4.

## What the earlier attempt learned

Grok session `render-thread-optimization`, branch `interior-portal-clear`, uncommitted, 2026-10-23 to 10-27, flat on Steam. Its notes are in `%USERPROFILE%\.grok\memory-v2\workspaces\edvr-unofficial-patch-2afe111a\topics\edvr-concourse-windows.md`. Facts worth keeping:

- Status.json Flags2 is the gate. `0x00014009` is on foot, in station, social space. `journalOnFootKnown()` is hard false in the flat profile, so flat reads Flags2 once `journalRawStatus().gameplay` is set. Gameplay is required: a leftover on-foot bit otherwise culls the main menu's hangar.
- The producer-B veto on foot works: about 185k-236k records dropped per 120 frames, 8k kept, room lit, faults none (logs 10-26 and 10-27).
- Light volumes are the room's fill. Additive cones and boxes (`8289669D`, `0357BBB2`, `A1B7CFCD`, about 512 cones) cannot be cut by centre position: dropping those outside the hall left the room dark, even with the ceiling opened.
- A hook on the context class also sees deferred contexts. The binding shadow follows the immediate context only, so a draw decision must read the shader bound on the issuing context.
- A flat draw never reaches `beginPanelOverride`. The four flat thunks return first, so a flat and a VR path are both needed.
- Ships, plumes and fog are not hall records. Engine trails are VS `EB787F98`, the green fog is `B12F7A61/42AC0CAC`, flares are `529D6238`, the sky billboard is `68DDDEF0/06332CA1`.

## What this review adds

**The store names every record, and the names split the scene cleanly.** The earlier work read names only from the settlement part test (greebles), found concourse kit only, and concluded the station had no name. The admission store's node names were logged by its own part census and never used for the cull. Log `edvr_gfx_20261025_133258`, census 2, at the glass, Flags2 `0x00014009`: 801 names, 5022 records. A prefix allow-list keeps 71 names and 488 records and culls 730 names and 4534 records, 90.3%.

- Kept: `Standard_SocialArea`, `SA_*` (`SA_StationHub_Compact`, `SA_Station_01_Bar`, `SA_TurboLiftBones`, posters), `PS_*`, `STAT_*`, `FUI_*`, `PROP_*`, `GUI_Interactive*`, `Interactive_*`, `PlantStack*`, `TP_*`, `HangarLobby`.
- Culled: `WheelSection*`, `Spoke*`, `RingRoad*`, `StrutA*`, `StadiumSegment*`, `SolarPanel*`, `HabitatRingA`, `DockingHub`, `Hanger*`, `LandingPad*`, `BlastShield*`, every ship part, every turret.
- These names recur in 31 logs across several sessions. They are probably one station; Flight 1 visits a second.

**Why the ring survived.** A record of the station model has its pivot near the player (2 to 13 m, log 195538), so a centre-in-box test keeps it. Its name does not depend on its pivot. This also answers Sean's 10-27 question about dropping external draws: judge the record that produced them, by name, where Elite still knows what it is.

**An opaque pane replaces the chase.** What survived the hall cull (lights, flares, fog, plumes, sky) is only visible through glass. Once the glass is opaque none of it needs its own rule, and interior fill stays untouched.

**The veto belongs at the rig.** The earlier work vetoed at producer B only. The hangar seal's rig veto (`0x431AFE0`, Elite's own off-screen branch) also skips the view loop, store, pose update, LOD and integrator, and gave -1.4 ms CPU (-26%) in the hangar (log 124634).

**Where the frame goes.** Log `edvr_gfx_20261026_185344`, census 1, two frames, hall cull on, at the window: 16,240 draws and 50.0M index-instances. The main view is 2.24M (4.5%). The `512x512` depth target is 20.0M (40%, 3868 draws). The six `2048x2048` tiles are 27.8M (55.5%). Depth-only passes are cheap per pixel (0.2 to 1.3 ms in the GPU profiles read), but they hold about 85% of the draw calls, which is render-thread and EDVR hook time. A veto at the rig reaches them; a draw-time filter in the colour view does not.

**The glass is not yet identified.** Dual-source blend (`bl=12,16,1/2,18,1`) is coloured transmission, and three pairs of it survive the hall cull: `E308565B/EDF86F13` n=1338 (5 or 6 draws, VS has an extra texture, probably NPC visors), `EFE42AC6/CC4E0188` n=804 (one draw, probably a helmet), `B553BB47/68ABCB9F` n=3 (the full-screen visor). `F512712C/AFED1D4B` with n=336 to 1098 appears at the glass before any cull (log 133258) and could be panes or ship canopies. Hence the capture by covered pixels.

## Design

### The cull

- **Gate.** Flags2 bit 0 (on foot) and bit 14 (social space), gameplay set, `fix.optimized_stations` on, and a record named `Standard_SocialArea` seen recently. The last condition keeps outposts and carriers out until they are added by name. The hangar seal is released on foot already, so the two never overlap.
- **Rule, per record.** Past a far limit (the sphere starts more than 200 m from the camera), cull whatever the name says, or if the name is unreadable; the anchor and the hub are exempt. Otherwise: keep if the name is unreadable, keep if it is on the allow-list, keep if the sphere is unreadable, keep if it is small and near (radius at most 25 m, centre within 40 m), which protects a weapon or prop whose name is generic (`Barrel`, `Slider`, `Hinge`). Otherwise cull.
- **Where.** The same three levels as the seal: rig veto (no record of the rig kept), producer-B veto, and the store's clear for path A.
- **Cost.** The class is one bit in the existing node cache, so the name is read once per node per cache period.

### The panes

The glass is VS `98397963AAEC45D3` / PS `66B08F89E4926C01`, which is Elite's full forward PBR material drawn `One`, `DestAlpha`, depth test on, no depth write. It draws after sky, fog, flares and light volumes, so an opaque pane overwrites them. A pixel shader is not needed: the material takes its values from textures, so EDVR's own textures in its slots turn glass into a metal.

| Slot | Reads | Glass | The mirror |
|---|---|---|---|
| `t8` | x metalness, y output alpha | metal 0, F0 0.04 | metal 1 |
| `t9` | normal xy | | flat |
| `t10` | rgb base colour, a roughness | | uniform bright grey, roughness 0.30 |

The reflection is the game's own probe cube array at `t5`, and the lights and shadows are the game's. `cb2[2].y` scales F0 and `cb2[2].z` scales roughness, and only row 2 of that buffer is read, so mode `mirror-soft` replaces it to compare a softer reflection.

| Mode | Around the draw | Result |
|---|---|---|
| normal | nothing | the game's glass |
| `mirror-steady-100` (default), `-25`, `-50`, `-200`, `-400` | `mirror`, then a third draw over it: a white pixel shader, `BlendFactor`, `One`, a pale colour at 0.25 to 4 | a mirror with steady light behind it; no shadows, and it does not move |
| `mirror-lit`, `mirror-bright`, `mirror-brightest` | `mirror`, then a third draw over it: a white, fully rough `t10`, `BlendFactor`, `One`, at 1, 2 or 4 | a mirror lit from behind, by the room's own probes and lights |
| `mirror` | a second draw with the three textures; `One`, `Zero`, RGB only; depth write on; marked pixels only | a lit metal mirror, nothing behind |
| `mirror-soft` | as `mirror`, plus `cb2` row 2 = 0, 1, 1.8, 0 | roughness 0.54, to compare |
| `tint` | as `mirror`, with a white pixel shader; `BlendFactor`, `Zero`; colour per index count | shows which pixels the mirror takes, by index count |

A draw is identified by VS and PS hash; the index count only separates the bay (n=36, opens on approach) from the rest. The start index is not stable across a reload, so the learned sides below last one session.

**Which pixels mirror (from the third build).** The whole room's glass is this one family, so the draw cannot say which side it is on. The depth buffer can. The game's own glass always draws, unchanged. Once a frame, before the first mirror, a colourless fullscreen triangle at z = 0.025 / D sets stencil bit 6 wherever the scene behind is farther than D metres (infinite reversed Z; 100 m by default, `diag.concourse_mirror_beyond`). Then each pane draws a second time with the mirror's textures and stencil `EQUAL 0x40`. So a pane with a wall behind it keeps the game's look, and a pane facing the emptied outside turns to metal, per pixel, with no table per station. Bit 6 is free: nothing after the opaque pass reads it, and the frame's clear resets it. The opaque pass's own stencil bits cannot serve, because a fullscreen draw clears them before the panes draw.

**Each pane's side.** When the cull lifts there is real structure behind the exterior panes, so the depth test would open holes in them. So one frame in eight each draw (keyed by index count, start index and base vertex) is counted twice: the game's draw and the marked mirror. At least 60% of its pixels mirrored (of at least 64) makes it exterior, otherwise interior. A reading under half the mesh's largest so far is ignored, because one mesh can be drawn in two places (Flight 3: 133k pixels at 66% and 17k at 30%, flipping within a frame). So that mesh takes the side of its large instance, and its small one is wrong while the cull is lifted. With the cull lifted, exterior panes draw whole without the stencil test, and interior ones not at all. The 60% keeps an interior pane out when the exterior is only seen through part of it. For 8 frames after the cull returns, a known exterior pane stays whole, because the frame in flight may still hold the depth of what was just taken away. The key is the mesh, so a mesh used both inside and out shows as `changed side` lines; the plain mirror phase is unaffected, because its test is per pixel.

**What the cull withholds (from the fourth build).** While the cull is on, whatever F4 says, these draws are not issued, so F4 at `off` shows exactly what is left. Nothing here can be seen past a mirror, so it is saving, not looks, except where a draw would paint over the glass. The bay's clear brings them all back.

| Sean's words | Family | Evidence |
|---|---|---|
| engine trails | plume `3B0B38CD` and its twin `34A1475B` (matched by PS because EDVR swaps the plume's VS) | depth test off, Flight 1 |
| smoke, volumetric | ribbon `B12F7A61/42AC0CAC`, volume `203DF517/EEAAC839`, haze `0A298DE7/6FD4C38B`, smoke `5E417E9D/BD801F2F` | `docs\per-object-motion.md` |
| ship lasers | beams and bolts `1F17BF54/A75C1DB6`, multicannon `939D01F2/C5DF9CC9`, shield strike `5C1D8EF5/C49F999F` | `docs\dlss-blur-elements-2026-10-14.md` |
| the planet | atmosphere `41E245D4`, `D95905C1`, `D1281DF4` (`docs\scanner-body.md`); body VS `72BDD292`, `26FC402B`, `9FFA5D5E`, `3530A6FD`, `71DD9863`, `ACE405F4`, `19F70CE8`, `C7FA0C0F` (`docs\eye-split.md`, `docs\fss-scanner.md`, `eye_draw_snapshot.h`) | older docs; not in view in any census here |
| the sun's glare | `8BD7C37A`, `1F3AD158` | `eye_draw_snapshot.h` |
| the haze (from the fifth build) | scatter `AFFEF018/41152F82`; particles `AFBCD3AD` and `9F4BBCFC/2BAE3742`, `9AEC596A/3789CA20`, `BBAD1CA8/E54F2A90`; shield strike part 2 `820E5C13/6EAA86EF` | Flight 4's census: what was left after the rest went; `docs\billboard-root-cause-2026-10-16.md` for the particles |
| the orange trail (from the seventh build) | ribbon `5EAFFCD0/DD371C57` | Flight 6's census: a strip, two draws a frame |
| not asked, paints on the mirror | flares `B75A6FF2/D56F859B` | after the panes, no depth test |

**Limited, not withheld (from the sixth build).** The light cones `1C506222/2519C905` and glow quads `359BF8FF/92FF8499` may also be the room's lamps, and nothing here separates them by draw. So while the cull is on they keep drawing, with the game's depth state plus stencil `NOT_EQUAL 0x40`: the mirror's mark, run first if it has not run this frame. A pixel with nothing within 100 m behind it is dropped, and one over the room stays. A cone outside can only show over far pixels, because the room's surfaces in front of it fail the depth test.

Not withheld: `88DCF116/494506A6` (the first-person laser), the sky `F8FA801F`, the stars and the space dust `E904D334` (see "Performance candidates").

The scope is on all four draw thunks. Beams, flares, stars, dust and the scatter are non-indexed, and until the fifth build only the indexed thunks looked at the lists.

**The sealed hangar (from the ninth build).** While the hangar is sealed, the families marked `sealedToo` are withheld too, with no diag key and whatever F4 says: everything above except flares, the scatter, the particle batches and the engine glow, which a hangar may draw itself (its own ship's glow on the pad, sparks, a lamp's glint). The light cones are not limited there.

**A known loss:** the logo on the glass (same VS, PS `98337732`, n=162) draws before the pane, so the mirror covers it. It returns when the bay clears.

**The trigger (goal 3).** Pane coverage, not a room coordinate: an occlusion query around the game's n=36 draws gives its share of the view. This needs no table per station and covers every bay window at once. At 20% of the view (12% in VR), on two readings running, the cull lifts, and 150 ms later the pane fades to the game's glass over 600 ms. From the fourth build it follows the view (Sean, after Flight 3: turning round at the glass left the cull off until he walked away). Under 40% of the entry share (8%, or 4.8% in VR) for 250 ms, it closes: straight back to the mirror and the cull if the bay is out of view (1% or less, or no reading), where a fade would only delay the saving, or by a fade if some of the bay is still in view. Coming back over the entry share during the fade turns it round. Coordinates were the first plan, but Flight 1's walk could not place the pane, and a table would have needed flights at each station type.

**Light behind the mirror.** The steady modes add a flat colour and need nothing below. The lit modes came first, and Flight 9 showed their cost: the sun's shadows, moving. The glass shader has no diffuse term. Its base colour only feeds F0, and the rest is the probes and lights through a specular lobe. So at roughness 1, F0 1, it returns a very soft sample of the room's light. Drawn additively over the mirror, it reads as light from behind and sits at the scene's own scale, which a constant in HDR units would not: nothing here has measured that scale. It follows the pane through its fade.

**The fade.** `BlendFactor`, `InvBlendFactor` with the mirror's textures, over the game's own glass, on exterior panes. At 0 the second draw stops.

### Later, if the numbers ask for it

See "Performance candidates".

## Flight 1

Flat on Steam is enough; VR after. Fly from the hangar, so the lift transition is seen. Take a screenshot at each step.

1. Stand in the middle of the bar, away from the windows. Toggle `hotkey.concourse_cull` on. Note what vanished and anything wrongly missing. Hold still for 15 s, off for 15 s, on for 15 s (the A/B).
2. With the cull on, press `NumLock` for a census (flat), and capture the GPU profile if bound.
3. Face a bay window from 2 m. With the cull off, press `hotkey.concourse_pane_capture`. The top eight candidates turn red, green, blue, yellow, magenta, cyan, orange and white. Screenshot. Each capture replaces the last one's colours, so one screenshot names one capture. Repeat under a skylight, then facing the window above the lift bank.
4. Press `hotkey.concourse_pane_mode` to see `mirror`, then `grey`. Screenshot each, facing a window, with the cull on.
5. Walk the bay window's length, then the room's edge, so `concourse frame:` traces the room. Stand still 3 s at each end of the bay window, and at the lift doors.
6. Take the lift to the hangar and come back, cull on. Note any flash at the doors.
7. If it is cheap, repeat steps 1 and 5 at a second station type.

After the flight: `python tools\edvr_log.py --target steam --expect-build c9f096db --grep "concourse"`.

## Flight 1 results

Flown 2026-11-05 flat, 3840x2160, log `edvr_gfx_20261105_194826`, build c9f096db. One census, cull on, at the bay window (19:51:12).

**The cull.** The ring, spokes and ships are gone and the room stayed lit (Sean's screenshots). 71 names kept, 768 culled; about 45% of rigs vetoed outright (119k of 263k in a 5 s window).

**Frame time.** At the bay the game sat at its 90 fps cap, so the A/Bs there read nothing. At cam_local (-4.7, 21.7, -18.7) it ran uncapped, and the cull was toggled twice:

| Window | Cull | Present p50 | Frames in 5 s |
|---|---|---|---|
| 19:53:58-19:54:03 | on | 12.29 ms | 404 |
| 19:54:03-19:54:08 | off for 2.9 s of it | 14.31 ms | 365 |
| 19:54:53-19:54:58 | on | 12.51 ms | 390 |
| 19:54:58-19:55:03 | off | 14.53 ms | 335 |
| 19:55:03-19:55:08 | on | 12.64 ms | 382 |

So the cull saves about 1.9 ms a frame there (13%). CPU timing was off, so the split between CPU and GPU is not known.

**The census.** Against the uncut census at the glass from 10-25 (log `133258`, census 2, frame 0, a different day, same kinds of names):

| View | Uncut indices | Cull on | Change | Uncut draws | Cull on |
|---|---|---|---|---|---|
| Main 3840x2160 | 7.38M | 0.88M | -88% | 2293 | 1355 |
| Shadow tiles 2048 | 29.41M | 11.67M | -60% | 10583 | 5642 |
| Probes 512 | 12.40M | 9.87M | -20% | 3322 | 3317 |
| Total | 49.2M | 22.4M | -54% | 16198 | 10314 |

The remaining 21.5M of shadow and probe indices is about what the middle of the room drew uncut on 10-25 (23.2M), so the room's own casters now dominate.

**What is still visible through the glass with the cull on.**

- Green plate cluster: probably the single mesh of 81,744 indices, `EB5234DB/E06E61AE`, one draw in the main view, 3 in the tiles and 4 in the probes. `SA_StationHub_Agricultural` (one record per frame) fits; not proven. It costs about 650k indices a frame.
- Orange gantry and crates below the bay window: a kept name, unidentified. Flight 2's `kept` lines add radius and distance.
- Glows `1C506222/2519C905` and `359BF8FF/92FF8499`, fog `B12F7A61/42AC0CAC`, stars `68DDDEF0` (8232 instances), plumes `EB787F98`. All but the plume draw before the pane.
- Light volumes: about 4700 instances (`0357BBB2` 3900, `8289669D` 780), many of them exterior. 33.9M samples (409% of the screen) on `A1B7CFCD` alone in one capture.

**The glass.** `98397963/66B08F89` n=36 covers 87.3% of the screen close up in the bay and 33-45% from about 7 m. The logo on the glass (same VS, PS `98337732`, n=162) rises and falls with it. n=192 and n=864 are in the same family; which windows they are was not captured.

**Why the tint showed only the visor.** The picks included `3064D7F4/CCDB2A91` n=192, the helmet visor (no depth test, 100.0% of the screen in every capture), and `C4B4B334/0146ABCC` n=3600, the GUI layer (a constant 83.2%). Both draw after the panes and, tinted, cover them. The mirror and grey keypresses went the same way, so H4 was not tested. The panes' PS has metal 0 and F0 0.04, so a plain opaque draw would have been nearly black anyway: hence the texture swap.

**Bugs in the instrument.** `callback cost` printed garbage about every 30 s. `TurboLiftDoor*` was culled beyond 40 m and kept inside it, a pop-in risk. Both are fixed in the second build.

**The room's coordinates.** Bar platform y 26.16, lower floor y 22.16, lift area y 21.68. Near the bay: (-17.5, 26.16, 39.6). Lift bank: (-4.7, 21.68, -18.7). `Standard_SocialArea` reports r=130.7 centred 256 m from the camera in the same frame, so its sphere does not bound the room.

## Flight 2

On the second build, flat. Start with F2 off, in the hangar, so the lift transition is seen. Screenshot each step.

1. F2 on in the room. Look at the bay window from across the room, a skylight, and the window above the lift bank: all mirrored? Anything mirrored that should not be? Anything drawing on top of a mirror?
2. Cycle F4 once at each window: `mirror-cb`, `tint`, `off`, back to `mirror`. Screenshot each. `tint` names each window type by colour, and the log says which index count it was.
3. With the mirror on, walk slowly up to the bay window, stop at the glass, look away and back, then walk at least 15 m away. Note the distance it opened at, whether the station was missing when it opened, and any flash.
4. Stand at the bay with `off`: is the view right? Look at the logo, then at a skylight (still mirrored?).
5. Lift to the hangar and back, cull on. Any flash at the doors?
6. If it is cheap, repeat steps 1 and 3 at a second station type.

Then: `python tools\edvr_log.py --target steam --expect-build e7b4b49b --grep "concourse"`.

## Flight 2 results

Log `edvr_gfx_20261106_040008`, build e7b4b49b, flat, 3840x2160, Steam, the same station type as Flight 1. NumLock was pressed at the bay window, and the log has no census from it and no line saying a press was missed, so nothing below comes from a census of this flight. Why the press did not land is not known. The stencil finding in Ruled out comes from Flight 1's census.

**Frame time.** `flat cpu` present p50, around the F2 press at 04:09:46: 16-18 ms in the 40 s before, then 11.1 ms (the 90 fps cap) almost throughout. With the bay clear and the cull lifted it was 15.6 ms. The cap hides the true figure, so the saving here is at least 5 ms, against 1.9 ms in Flight 1, whose panes were the game's glass. The position before the press is not logged, so the before and after are not at one proven spot. CPU timing was off.

**The mirror.** Sean: it works well on the exterior panes, and should be smoother, without the tile grid. It is also on every other pane in the room: a terminal's screen, the green and blue shop windows. The log shows why: about 20 index counts (6 to 864) pass the family test, and the four material lines logged all match (`t8`, `t9` 1x1; `t10` BC7 128x128; `cb2[2]` 1 1 1 0).

**The clear.** Lifts at 20.2-22.3% of the view, at z 26-34 in the room's frame, with the bay at z about 40. No lift elsewhere in the room, and it released on walking away each time.

**What is left outside.** With mode `off` and the cull on, one big plate cluster, a white bar and flares. `concourse names:` now gives distances, and the only kept names beyond 50 m are `PlantStack0-4` (x62, r 49-103 m, nearest 349-428 m), `HangarLobby` (794 m) and `PS_PP_Logo_Federation_Huge` (1190 m). `PlantStack` was on the allow-list by its name alone; the "plants" are the size of a building. The earlier guess that the cluster was `SA_StationHub_Agricultural` is dropped: its radius reads 0. `SA_StationHub_Windows_Exterior` (x2, r 35.9) is a record of its own. It may be most of the exterior panes, but nothing ties a draw to a record cheaply, and the depth rule does not need it.

## Flight 3

On the third build, flat, at the same station. Start with F2 off.

1. Stand still with a view of the room for a few seconds, then press F2 and stay still for 10 s. That gives a before and after at one place, if the frame rate is uncapped.
2. F2 on. The skylights, the lift-bank window, the terminals and the shop windows: is the exterior mirrored and the interior the game's own? Any pane mirrored in part, and is the edge where it should be?
3. The bay view with F4 at `off`: are the plates gone? What is left?
4. F4 at a skylight, for `mirror-soft`: which of the two looks right? Is the grid gone?
5. Walk up to the bay until it opens, stand there, and look at a skylight: still whole? Anything in a pane that should not be? Then walk 15 m away.
6. NumLock with the mirror on, looking at the bay from across the room, then NumLock at a shop window.
7. If it is cheap, repeat 2 and 5 at a second station type.

Then: `python tools\edvr_log.py --target steam --expect-build b19b368d --grep "concourse"`.

## Flight 3 results

Log `edvr_gfx_20261106_050431`, build b19b368d, flat, Steam, the same station type. Sean: "much better", with one issue (the cull stayed off after turning round at the bay) and three requests: remove the smoke and volumetrics, the ship lasers and their trails, and the planet.

- **The mark.** D32_FLOAT_S8X24_UINT, z 0.00025, about 440 passes per 5 s, 0 refused. Three sampled 5 s lines show 41-78 meshes known and 10-15 exterior.
- **The sides.** Exteriors read 99-100% (n=18: 7636 of 7656; n=30: 11659 of 11659), and interiors mostly 0%, with some in between (n=24: 496 of 1079). One mesh (n=12, start 12078150) logged 84 `changed side` lines: see "Each pane's side".
- **The plates.** All of `PlantStack0-4` culled. The farthest kept name is now at about 50 m.
- **NumLock.** Shift+NumLock worked: census at 05:14:00, phase clear, at the glass (z 39.6), with the cull lifted. So it shows the families but not what the cull leaves, and the planet was not in view.
- **Sean's screenshot, F4 at `off`, cull on.** Pale haze, a planet, stars, white bars and a long white line, pale wedges, a small white cloud, orange flares.
- **Frame time.** Mostly the 90 fps cap (p50 11.1 ms), with stretches at 12-13 ms; 16.9 ms in the 5 s window of the clear at 05:07:29-37.

## Flight 4

On the fourth build, flat, at the same station. Start with F2 off.

1. F2 on, F4 at `off`, look out through the bay from across the room. What is left? A planet, lasers, trails, smoke or flares still there means a family was missed: take a NumLock with it in view.
2. F2 off and on with F4 at `off`, looking at the room: does anything in the room change? Lamp glints, haze, a hologram.
3. F4 at `mirror`. Do the panes look clean, with nothing painted on them?
4. Walk up until the bay opens. Turn round: the skylights and the cull should return within a moment. Turn back: the bay should open again. Is the wait on turning back acceptable? Try a quick glance away too, which should not close it.
5. Back away from the bay while looking at it: where does it close, and does it flicker?
6. If it is cheap, repeat 3 and 4 at a second station type.

Then: `python tools\edvr_log.py --target steam --expect-build bc09083f --grep "concourse"`.

## Flight 4 results

Log `edvr_gfx_20261106_055426`, build bc09083f, flat, Steam, the same station type. NumLock at 06:00:41, cull on, F4 at `off`, in the mirror phase, looking out through the glass.

- **The clear.** Sean: the turn away works well, and nothing is missing or wrong in the room. Lifts at 20.2-23.1%, and it returns straight to the mirror at 0.0%.
- **Frame time, by F2.** `flat cpu` present p50, F2 off: 15-21 ms before the press at 05:56:53, 17-24 ms after the release at 05:56:57. F2 on: 11.3 ms in the 5 s window that held it, and 11.1 ms for most of the flight after, the 90 fps cap. So at least 4 ms and more often 6 or more, with the cap hiding the rest. The camera's position with F2 off is not logged.
- **The lists.** 15 families matched, including `planet surface`, `planetary 2` and `3`, and neither Sean's screenshot nor his report has a planet. Never matched: `beams and bolts`, `flares`, and with no ship or planet to show them `trail haze`, `body surface`, `planetary pipeline`, `terrain patch`, `beside the bodies`, `sun`, `sun arcs`.
- **Why the lasers and flares stayed.** The census's kind column: `1F17BF54` and `B75A6FF2` are `N`, DrawInstanced, and the scope was only on the indexed thunks. A census records a draw at the thunk's head, so a withheld draw still appears in it; the 30 s line is the evidence for what was skipped.
- **Why the haze stayed.** The plume was withheld at 12 draws a frame, yet the pale wedges are in the screenshot, so the wedges were never the plume; the "plume" label in the Flight 3 results was a guess. The trail ribbon and volume were withheld too, so the older note here calling `B12F7A61` the green fog is wrong, or at least not all of it.

## Flight 5

On the fifth build, flat, at the same station.

1. F2 on, F4 at `off`, looking out through the bay. Are the haze, the white bars, the flares and the ellipses gone? What is left? If anything stays, a NumLock with it in view.
2. F2 off and on, F4 at `off`, at a fixed view of the room: does the room change, a slight wash or a lamp's glint?
3. F4 at `mirror`: clean panes? Then the bay's clear once, for the view through it with everything back.
4. If it is cheap, a second station type.

Then: `python tools\edvr_log.py --target steam --expect-build 0e586d70 --grep "concourse"`.

## Flight 5 results

Log `edvr_gfx_20261106_061847`, build 0e586d70, flat, Steam, the same station type. Two NumLocks, 06:22:17 and 06:23:38. Sean's screenshot has F4 at `off` and the cull on: stars on a dark blue sky, five green ellipses and a large green glow at bottom left, pale wedges, faint thin streaks, no lasers, no flares, no planet. The sky was pale before: that wash was the `scatter`.

The 30 s line has all the new families matching, `particles B` most (52432 in one window). So the shapes that stayed are not particles. The census's blended main-view draws, less everything on the lists, leave the cones and glow quads as the only large additive family. The stars, the dust, some decals and the room's holograms and NPCs are the rest.

## Flight 6

On the sixth build, flat, at the same station.

1. F2 on, F4 at `off`, looking out through the bay. Are the ellipses, the green glow and the wedges gone? If anything big stays, press F3 with it in view. It logs what covers the screen.
2. F2 off and on, F4 at `off`, at a fixed view of the room: does a lamp, a glow or a wash change?
3. F4 at `mirror`: clean panes? Then the bay's clear once.
4. If it is cheap, a second station type.

Then: `python tools\edvr_log.py --target steam --expect-build d42c6471 --grep "concourse"`.

## Flight 6 results

Log `edvr_gfx_20261106_063641`, build d42c6471, flat, Steam. NumLocks at 06:39:14 and 06:40:39. Sean: "the green haze and light fills are gone", and two trails left, in screenshots sent to another session by mistake and to this one.

- **The orange double trail** (F4 at `off`), with a loop at its end, on a clean dark sky. In the second census the only thing on screen that fits is `5EAFFCD0/DD371C57`: topology 5 (a strip), n=95, two draws, VB stride 96. This is not the "grey ribbon" of `docs\per-object-motion.md`, which is `5E417E9D` and has been withheld since the fourth build.
- **The blue-white streak** on a mirrored pane, wavy and soft. Its census, taken at the lift bank (z -14.7), has 16251 lines in frame 0, 15910 of them in the 2048 and 512 passes, so the 16384 line cap ended it before the panes. It names nothing. The cap is 32768 now.
- **Why the streak is odd.** The glass shader has no screen-space input: `t0`-`t2` are raw lighting buffers, `t3`, `t4`, `t6`, `t7` shadows and lookups, `t5` the probes. And no blended draw of a trail reaches the 512 passes in the census that did complete. So neither a refraction nor a probe capture is shown, and a draw after the panes is the remaining route. The list after the panes in the second census is the visor, a glow, `B553BB47` and `5B0068AF`, none a trail, but that census was not looking at the streak.

## Flight 7

On the seventh build, flat, at the same station.

1. F4 at `off`, looking out: is the orange trail gone? Anything else left?
2. F4 at `mirror`, at the window where the streak showed: is it gone? If not, a NumLock with it in view. Its census now reaches the main view.
3. F2 off and on at a fixed view of the room: does anything change?
4. If it is cheap, a second station type.

Then: `python tools\edvr_log.py --target steam --expect-build a41de79b --grep "concourse"`.

## Flight 7 results

Log `edvr_gfx_20261106_065604`, build a41de79b, flat, Steam. NumLocks at 07:01:40 (z 30.8, the ship) and 07:02:59 (z -15.3, the entrance); both censuses reached the panes, so the longer cap works. F4 at `off` for both. Sean has moved his frame cap to 144.

- **Frame time.** `flat cpu` present p50 6.94 ms looking out from the bay, the 144 fps cap, and 13-14 ms at the lift bank.
- **The ship.** A dark hull with blue engine glows, in the first census only: `66DE2CAD/864F1F94` n=32310 one draw (the hull plating family), and `2D8263CC/89B662E2` n=36 six draws, additive, each with its own vertex buffer. Its other parts did not draw, so the name cull took them. Only records kept with no distance limit could carry a hull past 200 m, and that left the nameless ones. The 5 s line has about 15000-23000 nameless keeps. No survey ran then: it runs at an engage, and this ship came later.
- **The entrance.** Sean: "the lights are really bright, if I move a bit inside, it goes to normal". The far ceiling is paler than the near beams, so it may be haze rather than exposure, but `scatter` was withheld and no other fullscreen draw appears. See H23.

## Flight 8

On the eighth build, flat, at the same station.

1. At the entrance where it looks washed out, stay still. Press F2 off and on. Then F4 between `off` and `mirror`. Which of them changes the look, if any?
2. At the bay with a ship in the docking area, F4 at `off`: is it gone? If not, F3 with it in view.
3. F4 at `mirror`: are the two trails gone?
4. If it is cheap, a second station type.

Then: `python tools\edvr_log.py --target steam --expect-build b14a788d --grep "concourse"`.

## Flight 8 results

Flat, Steam, build b14a788d. Sean toggled F2 at the entrance: "the bright washed out look stays on when F2 is off". He asked for light behind the mirror, and for the same culling in a sealed hangar. His screenshot has the panes a flat blue-grey with a horizontal band, which is the probes' dark sky in the reflection now that the outside is gone.

## Flight 9

On the ninth build, flat, at the same station.

1. In the room with the mirrors on, F4 through `mirror-lit`, `mirror-bright`, `mirror-brightest` and `mirror`, from across the room and at a skylight. Which looks like a bright station beyond the glass? Is a lamp's highlight blotchy on a pane? The frame rate of each, if under the cap.
2. Board and stay on the pad, with the hangar sealed: does anything in the hangar go, such as a light, steam or a glint? Then launch: does anything pop back at the doors?
3. With a ship in the docking area, F4 at `off`: is it gone? If not, F3 with it in view.
4. Are the two trails gone?
5. If it is cheap, a second station type.
6. For `docs\sun-switch-re-2026-11-06.md`: in the concourse, F2 off, one NumLock.

Then: `python tools\edvr_log.py --target steam --expect-build 4eead728 --grep "concourse"`.

## Flight 9 notes

Log `edvr_gfx_20261106_092241`, build 4eead728. Sean sees rotating shadows on the lit mirrors, with a shadow's edge sharp across a pane. The glow pass is the game's glass shader at F0 1 and roughness 1, and that shader reads the sun's moment shadow map at `t7` (`ps_66B08F89`, four-moment gather, `cb1[297]`, `cb1[261]`), so the pane now shows the sun and its shadows, moving as the station turns. The look depends on the sun's direction, so it also changes over a rotation. He likes the look of the mode he is on, which the log says is `mirror-brightest`, and chose a steady light over keeping the sun without its shadows. The steady modes step around a guess at the HDR scale, so the first flight is to find the level.

## Flight 10

On build 990a2b44, flat, at the same station.

1. In the room, F4 through `mirror-steady-100`, `-200`, `-400`, `mirror-brightest`, `mirror`, `tint`, `off`, `-25`, `-50`. Which steady level looks right, from across the room and at a skylight? Does anything still move across a pane?
2. The sealed hangar: on the pad, is anything missing? Then launch.
3. A ship in the docking area, F4 at `off`: is it gone? If not, F3 with it in view.
4. Are the two trails gone?
5. If it is cheap, a second station type.

Then: `python tools\edvr_log.py --target steam --expect-build 990a2b44 --grep "concourse"`.

## Flight 11

On build 481d97b0, flat, Steam. One out-and-back, F2 pressed once while still seated (the walk survey runs only with the key armed).

1. Sit on the pad about 15 s, then get out, cross the bay, take the lift. In the room: do the skylights, bay windows and other windows look right? `diag.concourse_glow` in `edvr-flat.ini` changes them live.
2. Walk up to the ship services counter: does its window clear, and does it return when you turn away? If not, F4 to `tint` there and read the share from the log.
3. Look straight up at a skylight: does it stay mirrored?
4. Lift back, cross the bay, board, sit 15 s. Did anything vanish on the way out of the lift or after boarding?

Then: `python tools\edvr_log.py --target steam --expect-build 481d97b0 --grep "walk survey|concourse cull|concourse clear|clearing pane|guard"`.

## Flight 11 results

Log `edvr_gfx_20261106_110851`, build 481d97b0, flat, Steam, small pad.

- Sean: the skylights are brightest and the bay clears; the shop window did not clear; the bar windows show two bands; the angled skylights want less light than the overhead ones; frames dropped in the lift going back.
- The shop window is n=30, `start=7301760`, read exterior at 91%, 7.3 to 8.3% of the view at the counter, cam_local 9.2 26.17 30.8. In three flights elsewhere n=30 stayed under 3%. So n=30 now enters at 0.3 of the entry share.
- The walk from the ship to the lift stayed inside the bay box: the lift cabin sits at 7.0 17.9 -98.4 of half 76 37.5 129. Up the lift the frame mean climbs from 7 ms to 10.5-17 ms over the 8 s ride, then 17-19 ms for 3 s after the cut, then 11 ms.
- Names, up to 3.8 1.0 -43.8 in the bay: the seal would cull 89-90% of records, with the nearest would-cull 86 m from the camera. The survey spent its 300 reports in 11 s, because the pick cell flickers tick to tick on foot, so there is nothing from the lift door.
- ruled out: the mark as the cause of the two bands, because it covers 81 to 91% of those panes while the lower band is about a third of the pane.
- ruled out: the concourse names survey as the 3 s of 17-19 ms after arriving, because it gathers over 2 frames.

## Flight 12

On build f09f42df, flat, Steam. F2 once while still seated, as before.

1. Get out, cross the bay, stand about 5 s at the lift door, take the lift.
2. In the room: are the bar windows one even colour now? Do the angled skylights read dimmer than the overhead ones? `diag.concourse_glow` is `sky,angled,other,bay`, live, and the bay's 0.5 is a guess.
3. Walk up to the ship services counter: does its window clear, and does it return when you turn away?
4. Lift back. Say when the frames drop: in the cabin before it arrives, or after. Cross the bay, board, sit 15 s.

Then: `python tools\edvr_log.py --target steam --expect-build f09f42df --grep "walk survey|concourse cull|concourse clear|clearing pane|guard"`.

## Flight 12 results

Log `edvr_gfx_20261106_114951`, build f09f42df, flat, Steam, small pad.

- Sean: the bar windows look great; the same colour for all skylights that are not directly overhead; the shop window cleared at the counter; frames in the lift going down match the view across the room from the lift, then rise when the doors open on the hangar side.
- Cuts: 670 m going up, 673 m going down. `concourse cull: off (the camera cut to another room)` came 1 ms after the cut going down, and no `not seen for 5 s` line followed. H29 yes. The first 5 s after that cut read 11.49, 10.75, 10.42, 9.09, 9.01 ms with the cull off, with the camera still for the first three and the doors shut. Earlier builds, with the cull on for those 5 s, read p50 6.95 ms over a window that also held the corridor, so the two are not directly comparable.
- Names at the hangar's lift door, 7.2 19.4 -96.6 in the bay: would cull 90%, and the nearest would-cull is `HangerSmall` at 53.5 m (another bay's shell), then `DecalB` at 96 m. Nothing near the camera would go. So a seal on foot looks safe at this pad size, by the records. Not tried, and not asked for yet.

## Flight 13

On build eb9b6b8e, flat, Steam. F2 once while still seated, as before. No NumLock is needed: this flight is for the log. Move at an ordinary pace and pause where it says, so the log's positions can be told apart.

1. Get out, walk to the lift door, stand just outside it for 3 s. If it opens for you, step back 3 m and wait until it has closed, then walk up again.
2. Go in and ride up. Stay still until the doors have opened, then step out 5 m.
3. Turn round, stand just outside a lift door for 3 s, go in and ride down. Stay still until the doors have opened, then walk out. Board if you like.
4. Are the angled skylights right at the bar window's colour?

Then: `python tools\edvr_log.py --target steam --expect-build eb9b6b8e --grep "lift survey|concourse cull: the camera"`.

## Flight 13 results

Log `edvr_gfx_20261106_130939`, build eb9b6b8e, flat, Steam, small pad, three rides.

- The door records move in `c` (and in `k`), not in the pose translation. Between `TurboLiftDoorLeft` and `TurboLiftDoorRight` the gap is 1.49-1.50 m shut and 2.90-2.91 m open. Between the `2` pair it is 1.36-1.40 m shut and 2.64-2.66 m open. The first pair moves first on opening and the second about 0.3 s later.
- A record named exactly `TurboLift`, r 2.4, is the cabin. It is in the store only while the lift is in use, and it moves with the player through the cut. Camera to its centre: 0.81-0.98 m inside and shut; 2.32 m standing just outside the door; 3.24-7.48 m outside and shut.
- Both pairs read shut 6.05-6.16 s before each cut, and the doors start opening 46-145 ms after it. So the window is about 6 s a ride, all before the cut, and there is no shut time on the destination side worth using.
- Frame mean in that window: 13-15 ms going down, 8.5-9.9 and 10-11.8 ms going up.
- Sean did not report on the angled skylights in this flight.

## Flight 14

On build a3f17170, flat, Steam. F2 once while still seated, as before.

1. Ride up and down once each, and stay still in the cabin. Does the cabin stay lit and whole? Does anything pop when the doors open?
2. Note the frame rate in the cabin going down, against the 13-15 ms it was.
3. Step into a lift and, if the panel lets you, back out without riding, so the doors reopen with no cut. Is the room whole when they open?
4. If you have a minute: the `concourse_keep` test in "The view through the windows".

Then: `python tools\edvr_log.py --target steam --expect-build a3f17170 --grep "concourse cull: lift|concourse cull: on|concourse cull: off|walk survey: zone"`.

## Flight 14 results

Log `edvr_gfx_20261106_134043`, build a3f17170, flat, Steam, small pad.

- Going down: on for 5984 ms, ended by the cut, frame mean 11.76-12.20 ms. The 5 s line fell 0.1 s after the cut, so no counts for that ride.
- Going up: on for 6015 ms, ended by the cut, 6.94 ms throughout (the cap). 5 s line: rigs seen 601200, vetoed 481876, kept 29903. The cabin looked right, which shows it does not need the anchor or the hub.
- `concourse cull: on` came 21 ms after the cut going up. The doors had not started to open by then.
- `concourse_keep`: only `RingRoad02`, `07`, `08` and `RingRoadOffRamp02`, `03`, `08` came back. ruled out: the extra keep list as a way to see the big structure in this build, because the far rule culls an allowed name whose sphere starts past 200 m.

## Flight 15

On build 9a1d039f, flat, Steam. Take `concourse_keep` out of `edvr-flat.ini` first: it now works and would confuse the mask test. F2 once while still seated.

1. Ride up. If the pop comes, what popped: lighting, props, the windows?
2. Stand at the lift bank facing across the room, the costly spot, away from the bay windows. F6 steps the test: mode 1, 2, 3, 4, 5, then back to 0. Give each mode about 5 s, and press NumLock in modes 1, 2 and 3. In each: is the station there through the windows? Does the sun flood it? Does the room change?
3. F2 off for 5 s at the same spot, NumLock, F2 on.
4. Ride down, and press NumLock once the doors are shut. Frame rate in the cabin?
5. Put `concourse_keep` back, F4 at `off`, and F2 to compare at the same spot. Then take it out.

Then: `python tools\edvr_log.py --target steam --expect-build 9a1d039f --grep "concourse view test|concourse cull: lift|walk survey: zone"`.

## Flight 15 results

Log `edvr_gfx_20261106_141917`, build 9a1d039f, flat, Steam, small pad.

**The lift.** Going down: 5969 ms, frame mean 8.47 ms, rigs seen 452761, vetoed 373868, kept 21545. Going up: 6016 ms, 6.95 ms (the cap), twice. Sean has not said whether the pop came again.

**The views (VERIFIED from 23 dumps; the labels INFERRED from the flags and the census).** View 0: flags 0x3, layer 0x17, the main eye. Views 1-8: flags 0x5000, layers 0x11, 0x16 or 0x17: spot and cache shadows. Views 9-14: flags 0x4010, 0x4020, 0x4040, 0x4080, 0x4100, 0x4200, layer 0x17, cameras reaching to about 8000 m: the six sun cascades. So `+0x3DC` holds both kinds of shadow view, and it cannot tell them apart. In 1 dump of 23 a view had flags 0xB, layer 0x10, bit 3 set: possibly a probe face. Every outside rig had all three masks equal: 0x2 (75%), 0x1 (23%), 0x3 (2%).

**The census, frame 0, indices in millions and draws, at one spot (cam_local -0.49 22.16 -14.88). Frame 1 fell past the census cap in most, so frame 0 only.**

| Mode | Main | Tiles 2048 | 512 |
|---|---|---|---|
| 0, cull | 8.29, 8360 | 20.50, 12009 | 10.06, 4110 |
| 1, `+0x3DC` | 12.73, 9574 | 20.88, 12305 | 10.15, 4059 |
| 2, `+0x3DA` | 13.11, 10229 | 30.04, 15340 | 10.51, 5899 |
| 3, both | 12.80, 9840 | 22.50, 12625 | 10.44, 4359 |
| F2 off | 12.79, 10376 | 28.12, 14942 | 10.35, 4289 |

So zeroing `+0x3DC` keeps the whole main view and takes the tiles down to the cull's level, as RE predicted. `+0x3DA` does nothing here. The 512 row does not move in any mode.

**Frame time at that spot (camera still, steady readings, the view direction not logged).** Mode 0: 12.8 ms. Mode 1: 15.3. Mode 3: 15.5. Mode 4: 15.5. Mode 5: 15.4. F2 off: 16.5. Mode 2 climbed from 16.4 to 18.5 and is not read further.

- So the full cull saves about 3.7 ms there, and main view only about 1.2 ms. The shadow passes hold 76% of the indices the cull removes and only about a third of the time it saves, which refutes the expectation in "The view through the windows".
- ruled out: `+0x3DA` as a shadow mask, because no shadow view has flag bit 3, and mode 2's census matches F2 off.
- ruled out at this spot: the outside effects and the sun pair as a measurable frame cost, because modes 3, 4 and 5 read the same.
- The cause of the 2.6 ms left in the main view is not known, CPU or GPU: CPU timing was off.

**The keep list.** At cam_local 2.63 22.16 -14.65: flat fill 12.8 ms; plain glass 13.3-14.7 ms; plain glass with `concourse_keep` 13.5-13.7 ms. So the big structure costs nothing measurable there, and the flat fill itself saves about a millisecond over the game's glass. Few seconds each, one spot. The readings of 19-45 ms at 14:26:57-14:27:39 sit around an ini edit and are not counted: INFERRED that the game was in the background.

**Where that leaves the window view.** Candidates at that kind of spot: flat fill 12.8 ms, keep list with plain glass about 13.6, main view only about 15.4, F2 off 16.5. The keep list needs no offsets and no hook: if it looks right, it is the answer. A captured backdrop stays the fallback for a fuller view, since a frame that draws the outside can stay hidden behind an opaque pane. The mask test (`hotkey.concourse_view_test`) can go when this is decided.

## The window plan (Sean's goal, 2026-11-06)

Judge each candidate by how many of the 3.7 ms the full cull saves at the lift bank it keeps, and let Sean judge the view. Order: the size test first, since it is built and takes two minutes. Unless it wins clearly, a captured backdrop.

**The backdrop, as designed; a first prototype is built, with capture on a key press only, no refresh and no VR work. Softness: a cube's centre has fewer texels a degree than its edges, 27 at 3072 a face against about 38 pixels on a 4K screen.** The bay's lifting step already takes the cull off for 150 ms with the glass opaque (`kClearLiftMs`), flown. So a capture is that step, then a copy of the scene before the pane covers it, on the last frame. Nothing shows: the frame rate dips for 150 ms. The pixels go into a cube of view directions, about 100 MB at 2048 a face, softer than a 4K screen. A pane pixel looks up its direction: the glass PS has its position in `__USER_VERTEX_M_LIGHTINGPOSITION` (v2), and reads cb1 rows 277-279 against it; EDVR's flat logs read the camera at row 275. That the PS can use row 275 this way is INFERRED. The mark pass splits near from far, so what is within 100 m stays real and only the far station is backdrop. Captures: on a pane first seen, fading from a plain tint; after a few metres' walk; on a slow timer. Ships are left out of it. Known risks: ships and moving lights freeze between refreshes, DLSS may smear it on a turn, and the look-up's cost is unmeasured. First prototype: one capture from where Sean stands, to judge the look.

## Flight 16 results

Log `edvr_gfx_20261106_150756`, build b4f82839, flat, Steam, at cam_local 0.10 21.68 -17.05, camera still. Frame mean: flat fill before 13.2-15.4 ms; 150 m 14.5 rising to 16.9; 80 m 16.1-17.2; 40 m 17.5-18.2; 20 m 17.2-17.8; 10 m 16.9-18.2; flat fill after 14.7-16.9. Records kept by size per 5 s: about 97,000 at 150 m, up to about 190,000 at 10 m. Sean: "None of them seemed to impact frame rate, they all dropped well below the full cull."

- ruled out: keeping outside records by size, because even 150 m gives up most of the saving and the sizes barely differ from each other. About 1 ms of each is the plain glass.

## Flight 17 results, first part

Log `edvr_gfx_20261106_163158`, build a2fca02e. One capture, at cam_local -0.17 21.66 -16.75: lifted 328 ms including making the cube, scene 3840x2160 R11G11B10_FLOAT, 18 pane draws. Nothing was refused. Sean's screenshot, from nearer the bay, shows the flat light with patches of station view with stippled edges.

- INFERRED: the patches are the directions the far windows covered from the lift bank, where they subtend few degrees; walking closer, a window covers many directions never captured. The stipple is the capture's point splat. So one capture cannot fill a room. Sean had not yet pressed F6 again nearer the window.
- Frame time: `mirror-backdrop` and `mirror-steady` read the same at one spot (8.62-8.70 ms against 8.62 ms), so the look-up has no measurable cost.

**Next, if the look holds up when captured from close:** capture when a meaningful share of the visible panes has nothing captured, rate-limited, so the cost falls to nothing once the room has been seen; fill holes and uncovered areas from low mips of the capture instead of the flat colour, so they read as frosted glass in the station's colours. Whether to persist a cube per station is for later.

## Rendering from the concourse's centre (Sean, 2026-11-06)

Sean's idea: a cube around the room, filled from a snapshot at the room's centre, with the render off screen during a lift ride. F6 cannot do that: it records only what shows through the windows from where the player stands. RE, build 334925, not flown, `analysis\decomp\verify_20261106_concourse_probes.txt`:

- **The game's probe views: not the route.** They come from an EnvMap state machine, at a placed probe's position (INFERRED). Their faces draw no rigs: layer 0x10 against rig masks of `cfg & 0xF` gives zero (READ). Their size, 256 with 8 mips, is a code constant (READ). Their output feeds the game's reflection cubes through global objects. To carry the station a face would need a rig mask write, an unidentified target and lighting passes, and a way to stay out of the reflections. ruled out as the small route, because of those.
- **A chosen camera for whole frames: the smaller route, unproven.** `0x1EA9C0` is handed the render views and fills the view slots. A view's camera object is at `RV+0x310` (dirty byte `cam+0x230`). Editing it before `0x1EA9C0` should reach cull, LOD and the rig eval, and may reach the constant buffers, whose source was not found (INFERRED). The frame would still have to be kept from the player, which nobody has looked at. Appending a view is not small.
- **Smallest test:** during a ride, in a pre-forward hook on `0x1EA9C0`, move view 0's camera by a fixed offset, and log the view slot's position and the draw counts afterwards. If counts and image follow, the route works. A log-only form comes first. Offsets are per build.
- **Open beyond that:** no ride means no cube on a load straight into the concourse; the centre is unknown on a first ride up, since the anchor appears only 0.1-0.2 s before the cut.

## A baked shell (Sean, 2026-11-06)

**Flight result.** 30 automatic captures in about 70 s, one every 1.7 s or so while he moved, at 234-344 ms lifted each, with the missing share 0.2-5% each time. ruled out: capturing on movement, because walking keeps revealing directions, so it does not settle while the player moves, and the panes in his clip show flat colour with streaks between captures. The 19-45 ms readings then are partly the game in the background (his clip shows Alt-Tab).

**The proposal, not built.** Bake once, unobstructed, then draw a simple textured shape.
1. **Prove the camera.** Done for flat in Flight 22; VR has its own route at `0x283FA20`'s pose.
2. **Hide the frames.** Show the last cabin image at Present for those few frames. Not looked at yet. DLSS history would need a reset. VR not considered yet.
3. **Bake.** In one ride, from the room's centre, with the room's and cabin's rigs off and the outside on, ships left out: colour and depth for the faces windows look through.
4. **The shape.** A coarse grid per face, displaced by the baked depth, textured by the colour: one small mesh, so parallax comes from geometry with no per-pixel march and no captures at run time. Drawn behind the panes only. A tint from EDVR's glass shader on top, or the game's glass at about 1 ms.
5. **Keep it.** Per station by market ID (not read today), so a return costs nothing.

**The fallback, needing no hook:** real big structure through cheap glass. Flight 16's 150 m step read about 2 ms over the flat fill, about 1 ms of it the game's glass; EDVR's own glass could recover that, keeping about 2.7 of the 3.7 ms, live and exactly aligned. The gaps would have to be closed by name, from the `concourse names: culled` lines.

**Bake at the disembark fade (Sean, 2026-11-07).** It would cover the frames and come before the player sees a window. Two past disembarks (logs `edvr_gfx_20261106_141917`, `edvr_gfx_20261106_130939`): frames run at the cap up to the moment the camera jumps 10.37 m from cockpit to avatar, a `flat resolve reset event` fires there anyway, Status follows 0.7 to 0.95 s later, and that 5 s window held 561 and 613 frames against 720, so the game already drops frames there. Not known: when the screen is black relative to the jump, and whether the world is drawn under it. The blocker is the room's position: in the hangar `anchor age -1`, the anchor is not seen until the first ride's cut, and the concourse's offset from the hangar differs by pad and station. Loading straight in on foot has no disembark.

**A separate off-screen camera (Sean, 2026-11-07), RE only, not flown, `analysis\decomp\verify_20261107_second_view_owners.txt`:** the engine keeps four view owners (Primary, Secondary, Thumbnail, Auxiliary), each with its own G-buffer, RenderViews and pass objects, so a second view would not overwrite the main one (READ; the target names are in the exe). The Thumbnail owner has a camera setter, `0x2879AB0`, which fills `W+0x2D20` and `W+0x3010` and sets `W+0x3300` (READ, checked), and an enable byte, `W+0x2D10`. Against it: its RenderViews carry a flag that drops ordinary rigs, so no station draws (`0x432F180`, reported READ; a quick check of the first bytes found the `+0x3D5` test but not the flag test); its layer mask, shadows, size and the caller of the setter are not found; and it serves Holo-Me's isolated scene, so it may not see this world at all. A `SecondViewSceneTexture` owner (enable `W+0x331C`) has an unknown camera source and was not studied. Not small, with several unknowns. Parked in favour of the player's camera at the fade: the main thing a second view would save, hiding the frames, the fade already gives.

**Open:** no ride means flat fill until the first ride. The centre is unknown on a first ride up, so a first bake waits for a ride down. The lighting is frozen at the bake.

## Flight 25, the list as written

On build 5cdb2575, flat, Steam, with any anti-aliasing mode that runs the resolve (DLSS, DLAA, FSR, XeSS or TAA): the capture sits in its path. A clip would help. Insert steps the mode, with a notice: `log only`, `look from the room's centre`, `bake on arrival`, off.

1. Start the game fresh, loaded into the ship, docked. Disembark.
2. In the hangar, on foot, **before the first ride**: F2 once, and Insert three times to `bake on arrival`.
3. Ride up. Does the arrival look normal? It should hold the cabin for about a tenth of a second as the doors start. Any flash, jump or stutter you would notice?
4. Look at the windows, in the order you meet them. Do they show the station? Is it lit right against the room? Do people, crates or the helmet's arcs show in it? Is it sharp enough? The first half second may be plain, as the pane table learns its sides.
5. Walk with windows in view. Does the view hold, or slide? F9 steps how distance is used: `distance by depth` is the default.
6. Frame rate looking out from the lift bank, with the bake on the windows, against what you are used to. F4 steps the pane modes, and `Windows: captured view` is the bake's.
7. If you ride down and up again it bakes again. `diag.concourse_bake_near` (default 50) and `diag.concourse_bake_dirs` (default 11111, in the order +a2 -a2 +a0 -a0 up) in `edvr-flat.ini` are read at each ride up, so a window that goes blank with a digit at 0 needed that direction.

Then: `python tools\edvr_log.py --target steam --expect-build 5cdb2575 --grep "concourse bake|concourse camera test: (armed|mode 3 \(|mode 3: the window)|concourse cull: the camera cut|concourse pane: mode|status flags changed|walk survey: zone"`.

If the code never ran: no `concourse camera test: armed` line, so F2 or the mode was off. Not ready: `the cube is NOT READY`. Fired with nothing captured: `NOTHING CAPTURED`, and `concourse bake: a frame refused` if it got that far. Ran: `capture N` lines with the forward axis of each direction and the field of view, about 90, then `finished`, then `the hold ends` with its Presents and milliseconds. A direction missing from the capture lines means the tail is too short.

## Flight 24 results

Build c1f3ca32, flat, Steam, 1920x1080 this flight, log `edvr_gfx_20261107_064844`, with a 30 fps clip. The first ride up (06:52:07) ran with the mode off: Insert came at 06:52:39, after the ride down. So the one look was on the second ride up (06:53:06), and this is not yet a first visit.

- **Whole and lit at once.** Frame by frame, the clip goes from the shut cabin to the station, pad 31 and the hub, with nothing filling in afterwards that a 30 fps clip can see. Lighting looks right. The ride-down look's mid-distance structure is there too.
- **The empty direction is the floor side.** Six directions in order: 0 ahead, pad 31 and the hub; 1 a dark block with lit barrels; 2 pad 33; 3 the block and barrels again; 4 stars only; 5 the far wall, seen from below, past the room's leftovers. Direction 5 is up: people's soles face the camera. So direction 4 is straight down through the floor, out of the station's hull. No window faces it, so it costs nothing.
- **Room's leftovers, in the up direction.** People, crates, panels, doors and furniture hang in space above the centre, with the room's shell gone. None of it is on the keep list. Depth should remove them: all are nearer than the windows.
- **A dark band fills the lower part of directions 0 to 3,** with a pale surface below it. Not identified, and its distance is not known. If it is nearer than the windows, depth removes it. If it is past them, the windows should see it, and it would be right.
- **The visor is in the picture.** The helmet glass's arcs and the HUD are drawn over the look. A capture has to be taken before them.
- ruled out: the anchor seen well before the cut. Armed 6047 ms before; the cut's tick at 06:53:06.001 read the anchor 31 ms old, yet no tick before it fired, and the window opened 20 ms after it with the doors at 1.51 and 1.37, just starting (shut is 1.50 and 1.40). So the sighting looks like the cut's own frame, and the 125 to 188 ms ages on first rides look like that frame's stall, not time in hand. INFERRED from one fire; the first rides have no look to test it.
- **So the bake overlaps the doors starting.** The first 16 ticks ran about 9.6 ms apart, 153 ms in all, so six frames take about 60 ms, at 1080p. The first door pair moves from 46 to 145 ms after the cut. The picture would have to be held about 60 ms over a shut cabin whose doors are just starting. The second after the cut already runs slow on first rides (18.9 and 25.6 ms a frame), and the cut resets DLSS anyway. Not flown: the hold.
- No faults. Frame means in the look 6.9 to 7.1 ms once settled; 8.62 ms in the second holding the cut.

**Next, built as Flight 25:** at arrival, capture from the centre into the backdrop cube, before the visor, with a depth filter, holding the last image at Present meanwhile. Flown as a true first ride. VR has its own camera route and would come later.

**Only what the windows need (Sean, 2026-11-07):** "only capture the directions the available windows dictate", "and the dimensions necessary to do so": this concourse has a length of skylights looking up, no windows on the horizontal sides, and some angled ones near the roof. A first arrival cannot ask the room, since the panes are only learned once it is drawn. But the layout should be fixed against the anchor's axes, so a table in that frame holds for the room. So Flight 25's directions are the room's axes, with a key to leave any out. A narrower frustum draws less, and a strip plus patches stores far less than a cube, most of which no window looks at. Next after the look is judged: have the window draws log which parts of the cube they sample, as the room is walked, and bake exactly that.

## Flight 24, the list as written

On build c1f3ca32, flat, Steam. Hides nothing: the view jumps just before you arrive, turns through six directions, 0.8 s each, then comes back with you standing in the concourse's lift. A clip would help most here, since the first frames are what matter. Insert steps the mode, with a notice: `log only`, `look from the room's centre`, off.

1. Start the game fresh, loaded into the ship, docked: the first ride has to be the first time this session the room is seen. Disembark.
2. In the hangar, on foot, F2 once and Insert twice to `look from the room's centre`.
3. Ride up. It fires by itself just before arrival. In the first direction especially: is the station there at once, or does it fill in? Blurred, then sharp? Lit properly? Anything missing that the look from the ride down had? Which direction is the empty one, if any?
4. Ride down and up again for a second look. It fires on rides up only.

Then: `python tools\edvr_log.py --target steam --expect-build c1f3ca32 --grep "concourse camera test|concourse cull: the camera cut|flat camera origin|flat resolve reset|walk survey: zone|status flags changed"`.

If the code never ran: `not installed` or `NOT CALLED` per hook. Never armed: no `armed:` line, so F2 was off or the lift gate did not come on. Armed with no sighting: `the anchor was not seen`. Fired: `the window opens N ms after arming`, its gaps saying whether the doors were still shut, and the camera cut's line coming after it, which gives the time left. `the look's first 16 ticks` gives what a bake of that many frames would have cost right there.

## Flight 23 results

Build 19881a34, flat, Steam, log `edvr_gfx_20261107_060254`. Two looks on rides down; a ride up logged `NO ANCHOR HERE` and wrote nothing, as designed. Sean: "It only triggers going down. the station appears mostly whole, there was one angle which appeared to look into empty space (skybox only). I also was looking up and saw NPCs walking above me at one point".

- **A bake frame is cheap.** With the room off and the rest on, 7.14 to 8.06 ms a frame over both looks, against 12 to 13 ms in the cabin just before and after, with the cap at 6.94. Two rides, four directions' worth of seconds each. Records kept went from about 280 to about 2500 a frame. My earlier estimate of 16 ms a frame was far too high: six directions would be about 50 ms.
- **What that suggests about cost.** At the lift bank F2 off read 16.5 ms and the full cull 12.8 ms. Here the outside on and the room off reads under 8 ms. So the room itself may cost more than the outside. Different views, one spot: not measured directly.
- **The camera held.** `RV cam minus camera after` 0.00, written on every call, directions as asked, no faults. The centre, 47.11 -27.24 281.58, is 33 m from that lift's cabin. The window ended by time both rides, and the cut came 1.2 s later.
- **The empty direction.** Not yet known which of the six. If no window faces it, it costs nothing. If one does, a station part is being culled with the room: an allow-listed name that is really structure. Which it is decides whether the keep list needs work.
- **NPCs stayed.** They are not caught by name or by the small-record rule, so they do not reach the judgement that way. For a bake, depth may be the better filter than names: anything nearer the centre than the windows is the room's, whatever it is.
- **The disembark.** The game itself stalled for most of a second there: 32.26 ms a frame over 31 frames in the second holding the camera jump, then 7.35 ms. So up to about 0.7 s of the transition is already a held picture, the frames for a bake would cost about 50 ms of it, and nothing here says the screen is black yet. NumLock landed 1.8 s before the jump; the census shows nothing that names a fade, and when the screen goes black was not reported.

**The flow it has to serve (Sean, 2026-11-07):** "player docks -> disembarks -> enters the lift at the hanger -> a baked stand in for the cull will need to be generated by the time the lift doors open". A ride down is no use for that, so the capture does not start there. Past logs for a true first ride, from a disembark with no room seen before (`edvr_gfx_20261106_130939`), and a later ride up after a disembark (`edvr_gfx_20261107_060254`): the anchor is first seen no later than 156 and 188 ms before the cut, the doors open 46 to 145 ms after it, and the second after the cut holds a game stall already (18.9 and 25.6 ms a frame). So the room's position looked to be known about 200 to 330 ms before the doors open; Flight 24 corrects that, see its results. Once latched, the anchor's record stays readable from the hangar on later rides in a session (`concourse frame` lines). The cut also resets DLSS anyway. Not known: how complete and detailed the station is at that moment.

**Next, Flight 24:** the look on a ride up, from the anchor's first sighting.

## Flight 23, the list as written

On build 19881a34, flat, Steam. Hides nothing: the view jumps and turns, and that is the test. A clip would help, as the last one did. Insert steps the mode, with a notice: `log only`, `look from the room's centre`, off. The look runs for 4.8 s from the moment the lift's doors have shut, once per ride, on a ride that starts in the concourse; on one from the hangar it logs `NO ANCHOR HERE` and writes nothing.

1. Load into the ship, docked. Disembark. As the screen fades, press NumLock. When does it go fully black: before the view changes, or after?
2. Walk to the lift and ride up. In the concourse, F2 once, and Insert twice to `look from the room's centre`.
3. Ride down. When the doors have shut the view goes to the room's centre and turns through six directions, 0.8 s each: ahead, three quarter turns, straight up, straight down. What can you see? Is the station whole? Is it lit properly, or dark or flat? Sharp, or blurred and low in detail? Does it pop in over the 0.8 s? Is any of the room left floating, and any people or ships?
4. Ride up and down once more, for a second look.

Then: `python tools\edvr_log.py --target steam --expect-build 19881a34 --grep "concourse camera test|flat camera origin|flat resolve reset|flat draw census|DC (begin|end)|walk survey: zone|status flags changed"`.

If the code never ran: `not installed` or `NOT CALLED` per hook. A ride with no room: `NO ANCHOR HERE` and `written 0`. Ran: `written N`, `camera after` equal to the centre in the window's first line, and `RV cam minus camera after` near 0. `walk survey` frame means in the window price an unobstructed frame, one ride only.

## Flight 22 results

Build ac4ab507, flat, Steam, log `edvr_gfx_20261107_050539`. Log only, then a lift ride down and a turn ride up. Sean: "on the lift 30m, the elevator vanished and I saw some unculled structure and skybox and the turn 90 degrees shifted my view".

- **The picture follows.** Lift: `flat camera origin` step max 30.03, against 0.05 before and after, and `flat resolve reset event` fired at both ends of the window. `RV cam minus camera after` 0.00 and the view slot took it too. Turn: forward row (0.02 0.03 -1.00) to (-1.00 0.03 -0.02), written 36 of 36 calls an interval.
- **What he saw is what should be there.** The cull was on, so 30 m above the cabin leaves the kept names and the sky. It is not yet a look at the station itself.
- **One call a frame.** `0x2874290` matched `0x1EA9C0` call for call, other call sites 0. In log only, the camera equals the RenderView's.
- **No reset on the turn.** The reset fired on position only, so a bake's turned frames will need their own reset or to stay out of the history.
- **Frame time in the lift window:** 9.2 to 9.9 ms, against 12 to 13 ms just before and after. One ride, cull on; not a bake's cost.
- No faults, and the ride went on normally: the cut came 6 s after the doors shut, as on other rides. No census was taken.

**Smallest next test (H50), built as Flight 23:** in the window, put the camera at the room's centre with the cull off and the room's rigs vetoed (the keep list turned inside out), and step it through directions, with the view visible. That shows what a bake would capture, before any capture, shape or hiding is built. Then, in order: capture with the existing backdrop code, the shape, the trigger at the fade.

## Flight 22, the list as written

On build ac4ab507, flat, Steam. Hides nothing, and the view should jump: that is the test. F2 once. Insert steps the mode, with a notice: `log only`, `lift 30 m`, `turn 90 degrees`, off. Lift and turn act for 3 s from the moment the lift's doors have shut, once per ride.

1. Insert to `log only`. Stay on foot in the concourse about 10 s.
2. Insert to `lift 30 m`. Ride. When the doors have shut, does the view jump, and does it come back after 3 s? What can you see from up there? Does anything vanish or look broken?
3. Insert to `turn 90 degrees`. Ride back. Same questions.
4. On a ride where the view moved, a NumLock inside the 3 s for a census, if it is easy to catch.
5. Did your character, the cabin's sound or the lift itself do anything odd during the 3 s?

Then: `python tools\edvr_log.py --target steam --expect-build ac4ab507 --grep "concourse camera test|flat camera origin|flat resolve reset"`.

If the code never ran: `not installed` or `NOT CALLED` per hook. Ran and wrote, nothing followed: `written N`, with `flat camera origin` steady. Reached: `RV cam minus camera after` near 0, `flat camera origin` stepping about 30 m, and the view moves. In log only, `RV cam minus camera after` near 0 says this producer's camera is view 0's, with nothing written.

**Flat's camera, RE 2026-11-07, not flown, `analysis\decomp\verify_20261107_flat_camera_build.txt`:** `0x281D190` takes a pose record from the camera manager, builds a camera on its stack with `0x2874290`, then copies it into the camera set: slot 0 (the view node's copy, for draws), slot 1 (same camera with other near and far, for `RV+0x310`) (READ; that slot 1 differs only by near and far is INFERRED). `0x287D5A0` later hands the slots to the views. So the first write point upstream of every copy is that stack local. The pose record's upstream writers were not traced.

## Flight 21 results

Build 1bba08e9, flat, Steam, log `edvr_gfx_20261107_042506`. Log only, then a lift ride and a turn ride. All hooks installed, 0 faults.

- ruled out: `0x283FA20`'s pose as a write point in flat, because `0x283FA20` and `0x2845730` were `NOT CALLED` in every line, in log only and on both rides, so `written 0`. The mailbox route is VR's alone.
- **Flat's camera.** `view 0 camera handed over by the call at 0x287D95D` in every line. Call sites of `0x2874760`, once a frame each: `0x287D916`, `0x287D95D`, `0x288A35B`; `0x287D753` occasionally. `0x287D5A0` holds three of those.
- **Same path on foot and in the lift.** The call sites did not change across the rides.
- Nothing was written, so nothing could have moved; the eye has nothing to add here.

## Flight 21, the list as written

On build 1bba08e9, flat, Steam. Hides nothing, and may glitch. F2 once. Insert steps the mode, with a notice: `log only`, `lift 30 m at the eye pose`, `turn 90 degrees at the eye pose`, off. Lift and turn act for 3 s from the moment the lift's doors have shut, once per ride.

1. Insert to `log only`. Stay on foot in the concourse about 10 s. This names flat's camera with no writes.
2. Insert to `lift 30 m at the eye pose`. Ride. When the doors have shut, does the view jump, and does it come back after 3 s? Does anything vanish?
3. Insert to `turn 90 degrees at the eye pose`. Ride back. Same questions.

Then: `python tools\edvr_log.py --target steam --expect-build 1bba08e9 --grep "concourse camera test|flat camera origin|flat resolve reset"`.

If the code never ran: `not installed` or `NOT CALLED` per hook. Ran and wrote, nothing followed: `written N`, `RV cam minus eye pose after` not near 0, `flat camera origin` steady. Reached: that difference near 0, `flat camera origin` stepping about 30 m, a reset, and the view moves.

## Flight 20 results

Build bad4cf70, flat, Steam, log `edvr_gfx_20261107_032222`. Three rides; mode 5 (turn, held) was not flown. The hook ran every frame, 0 faults, and wrote the camera on every call in each window.

| Ride | Mode | Window | Slot minus written | `cb1[275]` step max in that 5 s | Resets |
|---|---|---|---|---|---|
| down | 2, lift | 03:25:16.5 to 19.5 | 0.00 (y 22.27 against a real -7.73) | 0.031 m | none |
| up | 3, turn | 03:25:37.6 to 40.6 | 0.00 | 0.047 m (position, not moved by this mode) | none |
| down | 4, lift held | 03:26:00.4 to 03.4 | 0.00 (y 25.90 against a real -4.10) | 0.0017 m | none |

- **The constants did not follow.** A 30 m move would read as a step of 30 in `flat camera origin`. The actual cuts on the same rides read 225. `flat resolve reset event` fired on the cuts and not in any window.
- ruled out: writing `RV+0x310` before `0x1EA9C0` as the way to move the game's constants, because `cb1[275]` stayed on the real camera, held or not, while the view slot's `+0x540` took the write. Holding it for the whole frame changed nothing, so `cb1` is not filled from this camera later in the frame either.
- **The game rewrites the camera each frame.** Mode 4: 284 held writes overwritten, 0 still there at the next call. `RV cam before` stayed on the real position.
- **By eye:** Sean, "No, nothing moved", on all three rides. So the objects' own matrices did not move either.
- **The cull probably did not follow.** A view turned 90 degrees would cull most of what the real camera sees, and that was not reported. INFERRED: he was asked whether the view moved, not whether anything vanished. Rigs seen were level (about 570 a frame in and out of the windows), but that list is ahead of the per-view cull, so it says nothing. Frame time rose to about 12 ms on both down rides at different moments (3.7 s and 0.9 s after the doors shut), which fits a change of view direction and is not evidence. The census in mode 4 (NumLock at 03:26:02.4, 42465 draws in 2 frames) has no unmoved census from that spot to compare with.
- **Noted, not chased:** after the window on a down ride, no `lift: doors shut` line appeared, though it did on the up ride. The lift work is paused.

**Upstream, RE pass 2026-11-07, build 334925, not flown, `analysis\decomp\verify_20261107_camera_upstream.txt`:**

- **Why the hook missed.** `0x2845730` builds the eye cameras with `0x283FA20` from a pose, then hands out two copies of one camera: `node+0x20`, and `RV+0x310` through `0x2874760` (READ). `cb1` is written at draw time from the draw view's camera (`0x596830`, via `0x5921F0`), not from `RV+0x310` (READ); that this camera is the `node+0x20` copy is INFERRED. So Flight 20 edited only the copy that feeds the cull context.
- **The pose is known to this project.** It is `W+0x3330`, the transition flash's mailbox (`docs\design-transition-flash-engine-fix-2026-09-23.md`, Static rounds 6 and 7): consumed and reset each frame, refilled by the `HMDCamera` tick through `0x2877090` (INFERRED to be that doc's `FUN_142874b20`).
- **In VR the route has already worked.** The flash was the whole frame drawn from the wrong place while the mailbox was empty. The ship-preview camera in rc.2 reaches the rendered eyes by a hook on `0x283FA20` (`native_preview_camera.cpp`), after a write to the stage output did not (`docs\design-native-onfoot-vr-2026-10-13.md`, 2026-10-26).
- **In flat it is not known.** That doc calls the job feeding the driver the per-frame VR job, and `HMDCamera` is a VR class, so `0x2845730` may not run in flat at all. `0x2874760` has six other callers (`0x287D5A0`, `0x287B050`, `0x28007D0`, `0x2801680`, `0x28740B0`, `0x2889940`), and any of them could hand view 0 its camera in flat.
- **The next test:** built as Flight 21 below. A static pass found no flat/VR test in these functions (INFERRED), so which path runs in flat is left to the log. `0x287D5A0` hands cameras from persistent blocks without going through `0x283FA20` (READ), and is the likeliest other path.
- **Still in the way even if it passes:** hiding the frames, DLSS history, a square wide view for the bake, the first-ride centre, and game code reading the pose (`[this+0x7B8]`, unknown; a write at `0x283FA20` is after that listener).

## Flight 20, the list as written

On build bad4cf70, flat, Steam. Hides nothing, and may glitch: this test only asks whether the view follows. F2 once. The camera modes act for 3 s from the moment the lift's doors have shut, once per ride. Insert steps the mode, with a notice: `log only`, `lift 30 m`, `turn 90 degrees`, `lift 30 m, held`, `turn 90 degrees, held`, off.

1. Insert to `lift 30 m`. Ride. Does the view jump when the doors have shut, and come back after 3 s?
2. Insert to `turn 90 degrees`, ride back. Same question.
3. Insert to `lift 30 m, held`. Ride. Does it jump where the first ride did not, or look different?
4. Insert to `turn 90 degrees, held`, ride back. Same question.
5. On any ride that moved the view, a NumLock inside the 3 s for a census, if it is easy to catch.

Then: `python tools\edvr_log.py --target steam --expect-build bad4cf70 --grep "concourse camera test|flat camera origin|flat resolve reset|walk survey: zone|lift survey"`.

What would appear if the code never ran: `the hook on 0x1EA9C0 is installed` is missing (or a line says why not), or `hook calls 0` with `THE HOOK WAS NOT CALLED`. Acting with no effect: `camera written on N` with the slot minus written not 0.

## Flight 19, the list as written

On build 2a140a30, flat, Steam. F2 once, then F4 to `Windows: captured view`: automatic capture starts with that, and F6 still captures by hand.

1. Look around the room, and walk. Do the windows fill by themselves? How long is a new direction blurred or flat before it sharpens? Do you notice the captures?
2. Stand still for 30 s looking at the same view, so the log can say whether the captures stop.
3. Walk with windows in view and step F9: `distance by depth`, `distance off`, `distance by sphere`. Which holds up best?
4. `diag.concourse_glass` in `edvr-flat.ini`, `r,g,b,haze,blur,fresnel`, live: what looks most like glass?
5. At the lift bank, camera still, F4 between `Windows: captured view` and `Windows: flat light`, about 5 s each, with the room filled, for the frame time.

Then: `python tools\edvr_log.py --target steam --expect-build 2a140a30 --grep "concourse backdrop|concourse pane: mode|walk survey: zone"`.

## Flight 18, the list as written

On build f50a46a2, flat, Steam. F2 once.

1. Under a skylight, looking up, press F6. Are the white dots gone? Is it sharp enough?
2. Walk a few metres, keeping it in view. F9 steps `Windows: distance by depth`, `distance off`, `distance by sphere`. Which holds alignment? If the whole image swims in sphere and depth and not in off, the camera row is wrong.
3. Press F6 from a second spot on the same window, and at a bay window, close but before it clears. Do the captures agree where they meet?
4. Turn quickly: does it smear?
5. With the camera still, F4 between `Windows: captured view` and `Windows: flat light`, about 5 s each, for the frame time.

Then: `python tools\edvr_log.py --target steam --expect-build f50a46a2 --grep "concourse backdrop|concourse pane: mode|walk survey: zone"`.

## Flight 17, the list as flown

On build a2fca02e, flat, Steam. F2 once. F6 lifts the cull for 200 ms and captures what is in view; the toast says the mode and the result.

1. At the lift bank, facing across the room, press F6. Do the windows in view fill with the station, and does anything flash? The windows not in view keep the flat colour.
2. F4 between `Windows: captured view` and `Windows: flat light`, with the camera still, about 5 s each. Is the captured view right, and is the image sharp enough?
3. Turn quickly: does it smear or swim? Walk 10-20 m: how soon does it read wrong? Press F6 again from there.
4. Look up at a skylight and press F6. Try to catch a ship in view, and say how much a frozen one bothers you.

Then: `python tools\edvr_log.py --target steam --expect-build a2fca02e --grep "concourse backdrop|concourse pane: mode|walk survey: zone"`.

## Earlier: the size test

On build b4f82839, flat, Steam. Take `concourse_keep` out of `edvr-flat.ini` first if it is there. F2 once. At the lift bank, facing across the room, away from the bay windows, F6 steps 150, 80, 40, 20 and 10 m, then back to the flat fill. About 5 s each, no NumLock. What is the smallest size that looks right? Is anything still missing there?

Then: `python tools\edvr_log.py --target steam --expect-build b4f82839 --grep "concourse view test|concourse names: culled|walk survey: zone"`.

## Performance candidates

Ranked by expected gain against risk. The first two and the plume are in the build.

1. **Rig veto on foot.** In the build. Part of the measured 1.9 ms.
2. **Cull by name in every view.** In the build. Shadow tiles -60%, probes -20%.
3. **Withhold what only the glass shows.** In the fourth build for smoke, trails, lasers, the planet and flares. Left: stars `68DDDEF0/F7A6E916` (about 9300 instances), space dust `E904D334/095030F2` (512 quads). They only appear past glass, and the mirror covers them, so they are cheap candidates once Flight 4 shows the list is safe.
4. **Exterior light volumes.** About 4700 instances a frame in the main view. Most effective GPU target left, and hard: earlier work showed they cannot be cut by centre position. A pane depth write early in the frame would reject the part behind glass.
5. **Drop kept names that are outside the room.** In the third build: `PlantStack` off the list, and any allowed name whose sphere starts more than 200 m from the camera is culled (the anchor and the hub excepted, because the gate reads them). Camera distance on purpose: in the hangar the `HangarLobby` is near and stays.
6. **The sun in the concourse.** The sun pair (`7E38A6AA/7CECABDE`) still draws. With mirrored skylights it may contribute nothing; the seal's sun skip can be A/B tested with one line.
7. **The reverse cull.** From the cockpit (docked with the pad raised, or flying inside), veto `Standard_SocialArea` and `SA_*` and show the panes as mirrors. The name list makes this cheap, and it was the original report (the concourse fully rendered while in the ship).
8. **Reflection probes and shadow tiles.** Three `2048` tiles draw identical caster sets each frame. With 21.5M of the remaining 22.4M indices here, refreshing static tiles or probes less often could save more than any of the above, but it needs reverse engineering first, and moving NPCs complicate it.

## Ruled out

- `ruled out: writing RV+0x310 before 0x1EA9C0 as the way to move the game's constants, because cb1[275] stayed on the real camera, held or not, while the view slot took the write (Flight 20).`
- `ruled out: shader-pair withholding for the exterior, because the room and the station share VS, PS and stencil (flights 10-26 10:03 and 12:54 lost the walls or NPC parts while the station stayed).`
- `ruled out: greeble names as station identity, because every named part at the glass is concourse kit (log 143601).`
- `ruled out: a fitted hall box as the cull key, because the ring's record has its pivot inside any such box (log 195538); it also needed the short axis clamped, and it could only shrink.`
- `ruled out: Elite+0x1600DD0 (the portal update) as the glass-clear gate, because three flights never saw it arm (logs 10-24).`
- `ruled out: culling light volumes by centre position, because it removes the room's fill (flights 10-26 16:47 to 17:13).`
- `ruled out: reading pool records back from the GPU to cull shell draws, because it pays traversal, submission and staging copies, and crashed once on a reused staging buffer; the store has the same answer earlier.`
- `ruled out: bay windows named by dual-source blend, because the bay pane is 98397963/66B08F89 with One, DestAlpha, 87% of the screen at the glass; no dual-source pair reached the capture's top 24 (log 194826).`
- `ruled out: picking panes automatically from the capture's top draws, because the visor and the GUI layer cover the screen in every capture and, tinted, hide the panes (log 194826).`
- `ruled out: the game's glass drawn opaque with its own material as the mirror, because its metalness is 0 and F0 is 0.04, so it would be nearly black (PS 66B08F89 disassembly; not flown, the visor hid the test).`
- `ruled out: 529D6238 as flares, because it draws last into its own target with no depth view and is what docs\desktop-window.md found Elite drawing into its desktop window; earlier notes here were wrong.`
- `ruled out: a room-coordinate zone as the approach trigger for now, because Flight 1's walk could not place the pane and pane coverage needs no per-station table (not flown; Flight 2 tests coverage).`
- `ruled out: the index count as the test for exterior glass, because about 20 counts pass the family and the interior shares them (Sean's screenshots and log 040008).`
- `ruled out: the opaque pass's stencil bits as a "nothing behind this pane" mask, because a fullscreen draw (#9100, write mask 0x0D, ref 0) clears them before the panes draw (Flight 1's census).`
- `ruled out for now: naming each draw's record through t33 and the instance stream, because it needs both mapped each frame and a proven join of record to pool row, and the depth rule needs neither. Worth returning to only if the depth rule fails.`
- `ruled out: a walk-away latch as the clear's release, because turning round at the bay left the cull off until Sean walked 10 m (Flight 3).`
- `ruled out: 88DCF116 as a ship's laser, because docs\design-flat-temporal-aa-2026-09-23.md names it the first-person laser, a pool draw; it stays in the room's list.`
- `ruled out: EB787F98/3B0B38CD as the pale wedges, because Flight 4 withheld it at 12 draws a frame and the wedges stayed in the screenshot.`
- `ruled out: B12F7A61/42AC0CAC as all of the green haze, because Flight 4 withheld it and the haze stayed.`
- `ruled out: the particle batches (2BAE3742, 3789CA20, E54F2A90) as the green ellipses and pale wedges, because Flight 5 withheld them all and the shapes stayed.`
- `ruled out: the cull as the cause of the washed-out look at the concourse's entrance, because it stays with F2 off (Flight 8).`

## Hangar to concourse and back

Sean's goal, 2026-11-06: leave the ship, cross the bay, take the lift, stay culled the whole way, and do the reverse. Sean approved steps 2 and 3 for the next build; steps 1 to 3 are built and not flown, step 4 is undecided.

**Measured (VERIFIED, log `edvr_gfx_20261106_083104`, flat, Steam, small pad, unless named).**

- The lift is a one-frame camera cut of about 1090 m, both ways: six trips, 1089.4 to 1091.4 m (`flat resolve reset event ... camera-cut=1`). No origin shift line comes with it.
- Going to the concourse, `concourse cull: on` comes 1 ms after the cut on all three trips, so the anchor is seen on the cut's tick.
- Coming back, the cull stays on for 5 s after the cut (4.99, 5.00, 5.00 s), which is `kConcAnchorFreshMs`. For those 5 s the concourse rule, which culls `Hanger*` and `LandingPad*`, runs on the hangar side. Nobody has reported it. UNKNOWN whether the lift cabin hides it.
- Flags2 bit 13 (in hangar) lags the cut by 0.13 to 1.07 s. That was the 2026-10 reason not to seal on foot in `docs\optimized-station-2026-10-12.md`: the flag is too late. The cut is not.
- The pad: on the 11-03 build (`edvr_gfx_20261103_044624`, no concourse key) the own pad is in every informative pass on foot, in the hangar and in the concourse. On this branch, with the key on, it is in 0 passes on foot, in the concourse and on the hangar side. INFERRED: the concourse store path returns before the seal's gathers.
- The box for a small pad is 152 x 75 x 258 m. UNKNOWN where the lift door sits against it. After one cut the camera travelled about 84 m in 20 s before it stopped.
- Frame time at the 144 cap (6.94 ms floor), present p50 per 5 s: hangar side on foot, unculled, 6.94 to 9.61 ms. Seated and sealed, 6.94 ms. Seated and unsealed, up to 11.41 ms. Concourse culled at the lift bank, 12 to 13 ms. So on this rig the on-foot seal has at most about 2.7 ms to save, and the cap hides anything under it. VR is unmeasured.

**Plan.**

1. Prove the seal guard first (build after 990a2b44). A seal on foot means more seals after an origin shift, which is where the boarding flicker is suspected.
2. Change rooms on the cut. A camera step over 100 m on foot ends the concourse cull on that tick and forgets the anchor. It removes the 5 s overlap whether or not the rest goes ahead.
3. A log-only walk survey: while on foot on the hangar side, once a second, the camera in the cell's frame, what the seal would veto by name, and frame time by zone. One flight: ship, bay, lift, concourse, and back. It answers whether the lift door, `HangarLobby` and the corridor fall inside the box, and how much the seal would save.
4. The seal on foot, if step 3 justifies it: held while seated, or on foot with Flags2 bit 13 and the camera in the box. It releases on the cut at once, since `kCameraFrames` (10) would leave the concourse empty for 10 frames. The pad gather comes back on the concourse path while the anchor is absent. The withheld families follow, since `feed.sealed` comes from the same flag.
5. Close the arc: default on with `fix.optimized_stations`, propose removing the temporary keys, a VR flight and a second station type.

**The walk survey's lines (log only, armed key, docked; removed when the arc closes).** `walk survey: on` once. Then each second, up to 900 a session: `walk survey: zone Z, ... cam in bay x y z of half a b c m, past the box by M m, pad NAME, pad in pass P, seal H, frame mean T ms over N frames`. Zones: `seated`, `bay`, `past the box`, `concourse`, `no cell`. Every 5 s and on a zone change while unsealed, up to 300 a session: `walk survey names (zone Z, cam ...): would cull A of B records (P%), N names within 300 m; nearest would-cull: ...`. A would-cull name close to the camera in the bay or the lift corridor is what a seal on foot would wrongly remove. `on` with per-second lines and no names line means the report never ran. `would cull 0 of 0` means the armed pass saw no records.

**The lift (Sean, 2026-11-06, Flight 12): can the room be off while the player rides?** Built after Flight 13, not flown (H34). The camera holds still for the ride, the game keeps drawing the room being left, then cuts with the doors still shut and draws the destination behind them too. Measured, flat, small pad: going down, 12.2 ms for about 7 s in the cabin, then 11 to 10.4 ms for about 3 s after the cut, until the doors open. Going up, 8.4 ms for about 7 s, then 15 to 17 ms for about 2.5 s after arriving. The plan: read the doors (H32), tell inside from outside (H33), then while shut with the player inside keep only the cabin and what is right around it, and release on the first door movement. It would also cover the hangar side's cabin, which is most of what a seal on foot would gain there.

**The view through the windows (Sean, 2026-11-06): does the mirror have to replace it?** Tested in Flight 15; the order below is from before that test, and its results change it: see "Flight 15 results". Sean asked whether a texture of the static view, or the lowest LOD with lighting tricks, could keep the look. Flight 1's census says most of what the cull removes is unseen shadow work, though the uncut census is from another day: main view 6.5M indices and 938 draws, sun tiles 17.7M and 4941, the 512 shadows 2.5M and 5. In order: (1) outside in the main view only, out of the shadow passes, with the sun off, since an outside with no casters would otherwise be flooded by sun; (2) a backdrop captured from frames with the cull off, sampled by view direction, which freezes ships and lets near structure slide; (3) the lowest LOD, which cuts indices and not draws, and is global unless it can be set per record. A haze or glare from the flat window shader could cover what is lost in any of them. RE, `analysis\decomp\verify_20261106_concourse_views.txt`, build 334925: no hook serves one view, since a rig is gathered once and each record stored once with a view mask, and passes filter by it (READ). The rig eval, `0x431DC40` in 334925 (`0x431AFE0` was 332841), takes a view only if one of three u16 masks on the rig passes: `+0x3DA` for view flag bit 3, `+0x3DC` for shadow-ish views, else `+0x3D8` (READ); which bit is which pass was not found. LOD is chosen per view and can be forced per record at existing hook sites, and lowers indices rather than draws (INFERRED). ruled out: `DAT_145ea39f8` as a mesh LOD control, because it is read only by terrain tile code. Cascades: no per-pass early-out, and the executor that runs them was not found, so with no outside in them they still cost their setup and the room's casters; stale moment-shadow data at t7 is valid to read. A test with no build, which Flight 14 found restored only RingRoad pieces within 200 m because the far rule beat the name, and which works from the next build: `concourse_keep = WheelSection,Spoke,RingRoad,StrutA,StadiumSegment,HabitatRingA` under `[diag]` in `edvr-flat.ini`, F4 at `off`, F2 to compare at one spot, which costs the big structure with its shadows.

**Not in this plan:** the sun, which Sean wants decoupled and has its own doc. A player with no own pad (arrived by taxi) gets no seal on foot, the same as today.
