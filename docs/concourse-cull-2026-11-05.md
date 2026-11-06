# Concourse cull: on foot, the station outside the bar is not drawn (2026-11-05)

## Status

- **State:** BUILT (2026-11-05): the branch head is the first instrument build, full `build.bat` green (optimized_station_test 904 checks), `--dll-only` promoted. Design plus all of Flight 1's instruments: Extends `fix.optimized_stations`. The cull and the pane work are off unless the temporary keys below turn them on. Nothing is flown. Built: name allow-list cull at the rig, producer and store (`concourse_cull_core.h`, `optimized_station.cpp`), the names survey (rig and producer vetoes forward for its 2 frames), `concourse frame:` trace, panes (`concourse_pane.cpp`, hooked in the four draw thunks).
- **Goals (Sean, 2026-11-05):** (1) on foot in the large-station concourse, cull everything outside the bay windows; (2) skylights and the window above the lift bank become a permanent mirrored surface, so missing structure is never seen; (3) approaching the bay windows clears the mirror and stops the cull, so the station interior is visible from there.
- **Direction:** identify records by the store's name, keep an allow-list, and make the panes opaque. The fitted hall, the shader-pair lists and the shell read-back from the earlier attempt are not carried over (see Ruled out).
- **Open hypotheses (Flight 1 answers all of them):**
  - **H1** the name allow-list removes the ring, spokes and ships and keeps the room. Survivors and false culls are named by `concourse names:`.
  - **H2** the rig veto on foot gives a measurable CPU saving; the cull shrinks the shadow and probe views too. Read from `flat cpu 5s` or fpsVR, and a census, with the cull toggled at a fixed view.
  - **H3** each glass surface is a blended draw that can be named by its covered pixels. `concourse pane capture:` ranks them at each window. If no blended draw covers the pane, Elite has no pane mesh and EDVR must supply one.
  - **H4** Elite's own glass shader, drawn with no transmission, looks acceptable as a mirror (pane mode `mirror`). If it reads black, an EDVR pixel shader is the next step.
  - **H5** the room keeps its fill light with exterior rigs vetoed. The record-level hall cull kept it (18:45 flight); the rig veto is untested on foot.
  - **H6** `Standard_SocialArea` marks the large-station concourse, and a point fixed in the room keeps its coordinates in that record's frame across stations. `concourse frame:` logs the camera in that frame.
- **Temporary keys (all removed when the arc closes):** `diag.concourse_cull` (on, off), `diag.concourse_keep` (extra name prefixes), `hotkey.concourse_cull`, `hotkey.concourse_pane_capture`, `hotkey.concourse_pane_mode`.
- **Ruled out:** see the section of that name. **Issues:** none yet.
- **Next flight:** the Flight 1 list below, on this branch's build.

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

Elite's glass is drawn after lighting. Changing only the blend state around the draw changes what it does, with no new shader and no extra geometry:

| Mode | Blend | Result |
|---|---|---|
| normal | game's (`One`, `Src1Color`) | transmission |
| `tint` | `BlendFactor`, `Zero` | flat colour, ranked per triple, for identification |
| `mirror` | `One`, `Zero` | the game's own reflection and lighting, nothing behind |
| `grey` | `BlendFactor`, `Zero` | constant colour |

A draw is identified by VS hash, PS hash and index count. The start index is not stable across a reload.

**The fade (goal 3), after Flight 1.** Draw the pane first with a blend that scales the destination by `1 - k` (`Zero`, `BlendFactor`), then the game's own draw unchanged. `k = 1` is the mirror and `k = 0` is the original, with no pop.

**The trigger (goal 3), after Flight 1.** A zone in the `Standard_SocialArea` record's frame, as the hangar cell is held in the pad's frame. On entering: stop the cull, wait for the rigs to rebuild, then fade the mirror out. On leaving: fade in, then cull.

### Later, if the numbers ask for it

See "Performance candidates".

## Flight 1

Flat on Steam is enough; VR after. Fly from the hangar, so the lift transition is seen. Take a screenshot at each step.

1. Stand in the middle of the bar, away from the windows. Toggle `hotkey.concourse_cull` on. Note what vanished and anything wrongly missing. Hold still for 15 s, off for 15 s, on for 15 s (the A/B).
2. With the cull on, press `NumLock` for a census (flat), and capture the GPU profile if bound.
3. Face a bay window from 2 m. With the cull off, press `hotkey.concourse_pane_capture`. The top five candidates turn red, green, blue, yellow and magenta. Screenshot. Repeat under a skylight, then facing the window above the lift bank.
4. Press `hotkey.concourse_pane_mode` to see `mirror`, then `grey`. Screenshot each, facing a window, with the cull on.
5. Walk the bay window's length, then the room's edge, so `concourse frame:` traces the room. Stand still 3 s at each end of the bay window, and at the lift doors.
6. Take the lift to the hangar and come back, cull on. Note any flash at the doors.
7. If it is cheap, repeat steps 1 and 5 at a second station type.

After the flight: `python tools\edvr_log.py --target steam --expect-build HEAD --grep "concourse"`.

## Performance candidates

Ranked by expected gain against risk. Only the first two are in the build.

1. **Rig veto on foot.** In the build. Expect the hangar's -1.4 ms or better, since the same records are involved.
2. **Cull by name in every view.** In the build. Reaches the shadow and probe draws (about 85% of draw calls).
3. **Withhold what only the glass shows, while the panes are opaque:** plumes `EB787F98`, fog `B12F7A61`, flares `529D6238` (about 1000 draws a frame), sky `68DDDEF0/F7A6E916`. Needs proof that none of them is inside the room; flares may be.
4. **Seed the panes' depth at the start of the frame.** Exterior light volumes, sky and fog then fail the depth test where a pane covers them. Saves GPU in proportion to the pane's screen area; worth it only if the GPU profile shows those passes costing.
5. **The sun in the concourse.** The sun pair (`7E38A6AA/7CECABDE`) still draws. With mirrored skylights it may contribute nothing; the seal's sun skip can be A/B tested with one line.
6. **The reverse cull.** From the cockpit (docked with the pad raised, or flying inside), veto `Standard_SocialArea` and `SA_*` and show the panes as mirrors. The name list makes this cheap, and it was the original report (the concourse fully rendered while in the ship).
7. **Reflection probes and shadow tiles.** Three `2048` tiles draw identical caster sets each frame. Refreshing static tiles or probes less often could save more than any of the above, but it needs reverse engineering first, and moving NPCs complicate it.

## Ruled out

- `ruled out: shader-pair withholding for the exterior, because the room and the station share VS, PS and stencil (flights 10-26 10:03 and 12:54 lost the walls or NPC parts while the station stayed).`
- `ruled out: greeble names as station identity, because every named part at the glass is concourse kit (log 143601).`
- `ruled out: a fitted hall box as the cull key, because the ring's record has its pivot inside any such box (log 195538); it also needed the short axis clamped, and it could only shrink.`
- `ruled out: Elite+0x1600DD0 (the portal update) as the glass-clear gate, because three flights never saw it arm (logs 10-24).`
- `ruled out: culling light volumes by centre position, because it removes the room's fill (flights 10-26 16:47 to 17:13).`
- `ruled out: reading pool records back from the GPU to cull shell draws, because it pays traversal, submission and staging copies, and crashed once on a reused staging buffer; the store has the same answer earlier.`
- `ruled out: bay windows named by dual-source blend alone, because the pairs that survive the hall cull look like visors, not panes (not yet proven; the capture decides).`
