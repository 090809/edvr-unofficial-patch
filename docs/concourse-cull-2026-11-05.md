# Concourse cull: on foot, the station outside the bar is not drawn (2026-11-05)

## Status

- **State:** Flight 9 flown 2026-11-06 flat on Steam, log `edvr_gfx_20261106_092241`, build 4eead728. Flight 10 flown 2026-11-06 on 990a2b44, log `edvr_gfx_20261106_095708`: Sean asked for brighter skylights, dimmer bay windows, a middle level elsewhere, and the shop window to clear like the bay. Flight 11 flown 2026-11-06 on 481d97b0, log `edvr_gfx_20261106_110851` (results below; the log hit its size cap at 11:22:26, so the trip back is lost). Flight 12 flown 2026-11-06 on f09f42df, log `edvr_gfx_20261106_114951`: results below. Build eb9b6b8e sets angled skylights to the bar window's level and adds a log-only lift door survey (full build green, optimized_station_test 1038 checks), installed on Steam 2026-11-06, not flown. Not merged. Extends `fix.optimized_stations`; the concourse cull is off unless the temporary keys turn it on, but the sealed-hangar withhold is on with `fix.optimized_stations`.
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
- **Open hypotheses (the next flight answers them):**
  - **H32** The lift's door records move with the doors, so the gap between `TurboLiftDoorLeft` and `TurboLiftDoorRight` says closed, and the first movement says opening. Unproven: doors animated by bones alone may leave every record still. `lift survey:` lines carry each part's positions (c, m, k, w) with the camera. A door that moves in none of them refutes this, and the fallback is the game's own lift state (`[lift+0x284]`, `comp+0x7A9`, `analysis\decomp\verify_20261027_onfoot_lift_look.txt`), which needs offsets for each game build.
  - **H33** The camera can be told to be inside a cabin rather than in front of a closed door, from its side of the door plane. The same lines give both cases. Culling the room with the player outside would be very visible, so no cull is built before this is answered.
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
- **Temporary keys (all removed when the arc closes):** `diag.concourse_cull` (on, off), `diag.concourse_keep` (extra name prefixes), `diag.concourse_pane_mode`, `diag.concourse_panes`, `diag.concourse_clear_share`, `diag.concourse_mirror_beyond`, `diag.concourse_glow` (sky, angled, other, bay), `hotkey.concourse_cull`, `hotkey.concourse_pane_capture`, `hotkey.concourse_pane_mode`.
- **Hangar to concourse and back (Sean, 2026-11-06):** steps 1 to 3 built, not flown; the seal on foot waits on the survey's numbers. See the section of that name at the end.
- **Ruled out:** see the section of that name. **Issues:** none yet.
- **Next flight:** the Flight 13 list, on build eb9b6b8e. Keys: `hotkey.concourse_cull` F2, `hotkey.concourse_pane_capture` F3, `hotkey.concourse_pane_mode` F4 in both Steam inis, `diag.concourse_glow` in `edvr-flat.ini`, and `log.max_mb = 200` there for these flights only (two censuses filled the default). Keys in both Steam inis: `hotkey.concourse_cull` F2 (the cull and the mirrors), `hotkey.concourse_pane_capture` F3 (logs a ranking by screen coverage and the kept names, changes nothing), `hotkey.concourse_pane_mode` F4.

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

**The lift (Sean, 2026-11-06, Flight 12): can the room be off while the player rides?** Not built. The camera holds still for the ride, the game keeps drawing the room being left, then cuts with the doors still shut and draws the destination behind them too. Measured, flat, small pad: going down, 12.2 ms for about 7 s in the cabin, then 11 to 10.4 ms for about 3 s after the cut, until the doors open. Going up, 8.4 ms for about 7 s, then 15 to 17 ms for about 2.5 s after arriving. The plan: read the doors (H32), tell inside from outside (H33), then while shut with the player inside keep only the cabin and what is right around it, and release on the first door movement. It would also cover the hangar side's cabin, which is most of what a seal on foot would gain there.

**Not in this plan:** the sun, which Sean wants decoupled and has its own doc. A player with no own pad (arrived by taxi) gets no seal on foot, the same as today.
