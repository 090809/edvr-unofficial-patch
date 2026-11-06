# Concourse cull: on foot, the station outside the bar is not drawn (2026-11-05)

## Status

- **State:** Flight 1 flown 2026-11-05 flat on Steam, log `edvr_gfx_20261105_194826`, build c9f096db. The cull works and saves about 1.9 ms a frame; the glass is named. The second build (mirror by texture swap, clearing on approach) is being built. Not merged. Extends `fix.optimized_stations`; off unless the temporary keys turn it on.
- **Goals (Sean, 2026-11-05):** (1) on foot in the large-station concourse, cull everything outside the bay windows; (2) skylights and the window above the lift bank become a permanent mirrored surface, so missing structure is never seen; (3) approaching the bay windows clears the mirror and stops the cull, so the station interior is visible from there.
- **Direction:** identify records by the store's name, keep an allow-list, and make the panes opaque. The fitted hall, the shader-pair lists and the shell read-back from the earlier attempt are not carried over (see Ruled out).
- **Answered by Flight 1:** H1 yes, with named survivors. H2 yes. H3 yes for the bay window: VS `98397963AAEC45D3` / PS `66B08F89E4926C01`, n=36. H5 yes by eye: the room stayed lit. See "Flight 1 results".
- **Open hypotheses (Flight 2 answers them):**
  - **H4b** Elite's own glass shader with EDVR's textures in its material slots (metal 1, bright base, low roughness, `t8`-`t10`) reads as a mirror lit by the real probes. If `cb2[2].y` scales it down, mode `mirror-cb` replaces that row. The `material` log line gives the values.
  - **H6b** the same family, at other index counts, is the skylights and the lift-bank window. `concourse pane shares:` and mode `tint` name them. If a permanent window shares n=36 with the bay, the n test needs another discriminator.
  - **H7** pane coverage is a good approach trigger: no room coordinates, so no per-station table. Clear at 20% of the view (12% in VR), released after 10 m of walking away.
  - **H8** nothing visible draws over the opaque pane once the plume is withheld and the pane writes depth. Screenshots of the mirror will show any survivor.
  - **H6** `Standard_SocialArea` marks the large-station concourse at other stations too. The second station in the flight list answers it.
- **Temporary keys (all removed when the arc closes):** `diag.concourse_cull` (on, off), `diag.concourse_keep` (extra name prefixes), `diag.concourse_pane_mode`, `diag.concourse_panes`, `diag.concourse_clear_share`, `hotkey.concourse_cull`, `hotkey.concourse_pane_capture`, `hotkey.concourse_pane_mode`.
- **Ruled out:** see the section of that name. **Issues:** none yet.
- **Next flight:** the Flight 2 list under "Flight 1 results", once the build is installed.

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
- **Rule, per record.** Keep if the name is unreadable. Keep if the name is on the allow-list. Keep if the sphere is unreadable. Keep if it is small and near (radius at most 25 m, centre within 40 m), which protects a weapon or prop whose name is generic (`Barrel`, `Slider`, `Hinge`). Otherwise cull.
- **Where.** The same three levels as the seal: rig veto (no record of the rig kept), producer-B veto, and the store's clear for path A.
- **Cost.** The class is one bit in the existing node cache, so the name is read once per node per cache period.

### The panes

The glass is VS `98397963AAEC45D3` / PS `66B08F89E4926C01`, which is Elite's full forward PBR material drawn `One`, `DestAlpha`, depth test on, no depth write. It draws after sky, fog, flares and light volumes, so an opaque pane overwrites them. A pixel shader is not needed: the material takes its values from textures, so EDVR's own textures in its slots turn glass into a metal.

| Slot | Reads | Glass | The mirror |
|---|---|---|---|
| `t8` | x metalness, y output alpha | metal 0, F0 0.04 | metal 1 |
| `t9` | normal xy | | flat, with a tile seam grid and light noise |
| `t10` | rgb base colour, a roughness | | bright grey, low roughness, seams darker and rougher |

The reflection is the game's own probe cube array at `t5`, and the lights and shadows are the game's. `cb2[2].y` scales F0 and `cb2[2].z` scales roughness, and only row 2 of that buffer is read, so mode `mirror-cb` can replace it if the material's values mute the result.

| Mode | Around the draw | Result |
|---|---|---|
| normal | nothing | the game's glass |
| `mirror` | the three textures; `One`, `Zero`, RGB only; depth write on | a lit metal mirror, nothing behind |
| `mirror-cb` | as `mirror`, plus `cb2` row 2 = 0, 1, 1, 0 | the same with the material's scales forced |
| `tint` | white pixel shader; `BlendFactor`, `Zero`; colour per index count | identifies each window type by eye |

A draw is identified by VS and PS hash; the index count separates the bay (n=36, opens on approach) from the permanent panes. The start index is not stable across a reload.

**What draws after the panes.** The plume (census VS `EB787F98`, but PS `3B0B38CD` is the match because EDVR swaps the plume's VS) and its 1024 twin (`78F5F08D/34A1475B`) have depth test off, so they are withheld while the panes are opaque. Depth-tested late draws (`BBAD1CA8`, `820E5C13`) fail against the pane's depth write.

**A known loss:** the logo on the glass (same VS, PS `98337732`, n=162) draws before the pane, so the mirror covers it. It returns when the bay clears.

**The trigger (goal 3).** Pane coverage, not a room coordinate: an occlusion query around the n=36 draws gives its share of the view. This needs no table per station and covers every bay window at once. At 20% of the view (12% in VR), on two readings running: the camera position is latched, the cull lifts, 150 ms later the pane fades to the game's glass over 600 ms. It stays clear while the pane is large, and looking away does nothing. After walking 10 m from the last latched position it fades back, then the cull returns behind the opaque glass. Coordinates were the first plan, but Flight 1's walk could not place the pane, and a table would have needed flights at each station type.

**The fade.** `BlendFactor`, `InvBlendFactor` with the mirror's textures. At 0 the game's own draw returns, a small step to its faint reflection.

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

Then: `python tools\edvr_log.py --target steam --expect-build <commit> --grep "concourse"`.

## Performance candidates

Ranked by expected gain against risk. The first two and the plume are in the build.

1. **Rig veto on foot.** In the build. Part of the measured 1.9 ms.
2. **Cull by name in every view.** In the build. Shadow tiles -60%, probes -20%.
3. **Withhold what only the glass shows, while every pane is opaque:** fog `B12F7A61`, stars `68DDDEF0/F7A6E916`, glows `1C506222`, `359BF8FF`. They draw and are then overwritten, so skipping them is pure saving. Needs proof that none is inside the room; glows may be. (Not `529D6238`: see Ruled out.) Plumes are withheld in this build.
4. **Exterior light volumes.** About 4700 instances a frame in the main view. Most effective GPU target left, and hard: earlier work showed they cannot be cut by centre position. A pane depth write early in the frame would reject the part behind glass.
5. **Drop kept names that are outside the room:** the 81,744-index mesh alone is about 650k indices a frame. Flight 2's distances should say which are safe.
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
