# VR Livery screen on foot: the commander model is misplaced on the Quest 3 (2026-11-09)

## Status

- **State:** OPEN, diagnosis only. No code changed.
- **Report (the pilot, 2026-11-09):** in the on-foot Livery screen (suit cosmetics) on the Quest 3, the viewer
  sits under the commander model's crotch. It should be behind the UI, as it is on the Pimax.
- **Flight:** Steam install, `edvr_gfx_20261109_063740.log` with `edvr_openxr_20261109_063742_222_5552.log`, build
  `v0.22.0-rc.7-272-g65dc3884` (matches HEAD 65dc3884). Runtime VirtualDesktopXR, Meta Quest 3, 90 Hz, render
  1504x1522 per eye, output 3008x3044, DLSS k. Native on-foot VR on.
- **Evidence:** eye dump `064024386001` (06:40:24, the Livery screen, LIVERY / GENERAL tabs in view). Dump
  `064036735002` (06:40:36) is after the screen closed (concourse kiosks).
- **F228 FLOWN (2026-11-09 08:21, Quest 3, `edvr_gfx_20261109_082137.log`, c25fac3e):** the hold works as built
  (Livery Show seen, F107 held 3630 calls) and moves the model, but does not fix it. Key on: the model is behind
  the pilot. Key off: it stands over him. The UI is right in both. See "Flight F228".
- **Leading hypothesis (H5):** in Livery the game renders from the commander's OWN camera, while the loadout
  browser (correct) renders from the third-person preview camera about 4 m out. Livery shows that preview camera
  for about a second at open, then drops it. The model stands at the commander, so the view is from inside it.
  EDVR's adjustments (H4) only decide which wrong view: behind (held off) or over (engaged). Same class as the
  weapon showroom on 11-01, which also used the commander's camera (the pilot's comparison).
- **Open:** H5's cause: why Livery drops the preview camera, and whether the Pimax does too on this build (no
  logged Pimax Livery visit exists; the 05:04 "Pimax visit" below was most likely the loadout browser). H1 weak form.
- **RE (2026-11-09, build 334925):** the Livery placement is authored scene geometry and reads no projection
  element (see "RE"). The notes wait in the analysis inbox, `20261109-090000-livery-anchor`, not yet filed.
- **F228 (c25fac3e, BUILT, INSTALLED Steam `--scope both` as v0.22.0-rc.7-274-gc25fac3e, NOT FLOWN):** F209's
  Show/Close watch also knows `HumanoidLiveryActivity` and `HumanoidWeaponLiveryActivity` (their vtable slots
  checked in the installed exe). While one reads open and the key is on, F107 holds the camera away, as at a
  buy screen. With the key off the screens are only logged. The full build ran green in a short-path worktree
  (`C:\wt99`, branch `claude/f228-short-build`, same commit): in this worktree's long path `launcher_test` fails
  two checks for path length alone.
- **RE 2 (2026-11-09, inbox `20261109-101500-livery-camera`, not filed):**
  - No EDVR patch is on Livery's camera choice (scan of the camera classes and the activity region). No HMD,
    projection or render-size input is either.
  - Livery and the loadout screens bind the same preview manager with the same mode byte 0.
  - The model is a separate preview avatar (six new AMCs, 0 parts at the open, filled later).
  - The lead is (b): the orbit distance is taken from the preview avatar's bounds while the avatar is still
    empty, so the camera collapses to the avatar centre (the commander) and is never recomputed. CANDIDATE.
- **Next (proposed, no build):**
  - On the Quest: open Livery, wait 20 s, hold Mouse 2 and rotate (the orbit input), then close and reopen it
    twice. Note any open where the model sits behind the UI, and take an eye dump.
  - Then the same on the Pimax.
  - (b) predicts that a later open, or a rotate after the avatar loads, puts the camera back out.
- **Reopen-and-rotate test FLOWN (08:53, Quest, c25fac3e, `edvr_gfx_20261109_085334.log`):**
  - The pilot: reopening and rotating did not move the model to the right spot.
  - The log holds ONE Livery open (Show 08:55:48.5, Close 08:56:26.2; the key off, then on from 08:56:13). It
    ends at 08:56:40, when on-foot stereo ended, so the reopens are not in it.
  - Inside that open the pattern repeats: the 4 m preview camera for about a second at the open (08:55:43-47)
    and at the close (08:56:27-28), and the commander's spot in between.
  - During the rotate (08:56:17-25) `cam_local` moved only 10-25 cm about one point. That is either the orbit on
    a collapsed radius or head motion; the once-a-second samples cannot split them.
  - (b) is not refuted.
- **Then, if needed (build):** log the CustomisationVisual's distance and pose fields (+0x340, +0x344, +0x580,
  +0x5A0, +0x5E0) once a second while Livery is open, from the remembered activity +0x680. Optionally, an
  exact-build hook on 0x12714C0 logging its mode.
- **Temporary keys:** `experimental.native_on_foot_livery_game_view` (on|off, default on, live, F8 row "Livery
  uses game view"). When the arc closes, the winning value becomes the behaviour and the key goes (ask first).
- **Issue:** none yet.

## What the flight shows

- **The screen.** The scene depth probe counts 16-18 draws a frame from 06:40:03 to 06:40:24 (the showroom: the
  model and its dark blue backdrop), against 120+ in the concourse before and after. The centre reads 2.05 to
  2.36 m. In the dump the legs fill the eye at about 2.2 m and the backdrop is at about 4.4 m.
- **The camera is the commander's own.** For the same seconds F107 never fires (770 of 770 pin calls accepted,
  `camera 3 m away 0 s (max 8.0 cm)`). The game's camera reads +8.0 cm over the animated neck (1.543 m), and the
  concourse frame's `cam_local` stays at the commander's spot (4.50 22.75 3.22). So the game drew the preview
  model and backdrop around the commander's camera, and placed the model above the eye: the pilot's head pitch
  reads +47 to +73 deg from 06:40:16 to 06:40:29 while he looked at it.
- **The game's own showroom camera also appears.** Twice it stood 22 m from the neck (episodes 2 and 4, at
  06:39:53 and 06:40:32, `cam_local` -5.97 20.09 -16.01), and once 3.9 m away (episode 3). F107 treated those as
  away, as designed. The misplaced view is the stretch in between, with the commander's camera.
- **EDVR's own view offsets are centimetres:** the raise +7.0 cm, the virtual neck 0. The misplacement is metres.

## The Quest-only input: the projection's vertical centre

`projection_query` lines in the runtime logs, the matrix EDVR answers for Elite's `GetProjectionMatrix`:

| Rig | m00 | m11 | m02 (horizontal centre) | m12 (vertical centre) |
|---|---|---|---|---|
| Quest 3, VirtualDesktopXR (this flight) | 0.922 | 0.896 | -0.243 / +0.243 | **-0.193** |
| Pimax (Frontier, `edvr_openxr_20261109_052110_952_45272.log`) | 0.939 | 0.939 | -0.178 / +0.178 | 0 |

The Quest's m11 and m12 give tangents up 0.90 (42.0 deg) and down 1.33 (53.1 deg): the frustum's centre is about
12 deg below the forward axis (F194 measured the same rig). The Pimax's vertical frustum is symmetric (46.8 deg each
way). Both rigs have a horizontal off-centre term of similar size, and the Pimax places the model correctly. So a
mishandled horizontal term would show on both rigs, and the vertical term is the one input only the Quest has.

## Hypotheses

- **H4, EDVR's native on-foot machinery engaged during Livery** (added after the RE and the Pimax visit, below).
  With the camera on the commander, F107 lets the body pin, the neck reference and the raise act. They act on the
  local avatar and the view while the game draws its showroom, and the model ends up wrong.
  - Confirms: holding F107 away while Livery is open (the F209 signal, extended) puts the model behind the UI on
    the Quest.
  - Refutes: the model stays wrong on the Quest with the hold on and the pin refusing every call.
  - Not established: how the pin or the raise would move the model by metres. Their logged moves are small
    (no clamp at 1.5 m or 0.8 m, raise +7.0 cm).
- **H1, the vertical off-centre frustum.** Weakened by the RE: the placement code does not read it. The Livery preview places the model behind the UI by a
  computation that reads the eye projection and treats it as vertically symmetric, or gets the sign of m12 wrong.
  It is right at m12 = 0.
  - Confirms: with a vertically symmetric frustum handed to Elite on the Quest, the model sits behind the UI.
  - Refutes: the model stays where it is with m12 = 0.
  - Not established: the game's code for the preview's anchor. The analysis repository has the strings
    ("Vendor Suit Livery", `decomp_3E67C20`), not the placement.
- **H2, the Steam install's settings.** The pilot flies the Quest on the Steam install and the Pimax on Frontier,
  and the two `edvr.ini` files differ (among others: `visual.world_scale` 110 on Frontier only, the
  supersample keys, `fix.openxr_resolution` entries).
  - Confirms: the Pimax on the Steam install shows the bug.
  - Refutes: the Pimax on the Steam install, same build, places the model behind the UI.
- **H3, another per-headset answer** (render size, the Pimax's canted eye-to-head rotation, IPD). Less likely: none
  of them moves anything by metres. H1's A/B would leave the model misplaced if one of these is the cause.

## RE (2026-11-09, build 334925)

From a Ghidra read of the installed build. The decompilations are in the analysis inbox (not yet filed).

- **The owners.** `HumanoidLiveryActivity` (suit, vtable 0x5612F30) and `HumanoidWeaponLiveryActivity` (weapons,
  vtable 0x5614328), OnOpen 0x482D230, OnClose 0x481E7A0. Each embeds a `CustomisationVisual` at +0x680.
- **The placement is authored.** 0x12714C0 picks nodes in a data-authored showroom scene. For the character
  these are the attach node `Root!AttachPoint_AvatarCentre` and the camera node `Root!Camera_AvatarPreview`. The
  camera pose (+0x5E0, written by 0x126DFF0) comes from node transforms, the avatar's bounds and the orbit
  inputs (pitch, yaw, zoom). Nothing from 0x1268000 to 0x1276000 reads a projection element, a FOV, the HMD
  pose, the eye-to-head transform, the render size or the UI panel's transform (CONFIRMED by scan and by eye).
- **The projection wrapper.** 0x4E2DD0 calls `GetProjectionMatrix` (return address 0x4E2E5C, the one the
  runtime logs) and stores the matrix transposed, with the third column negated. Its consumers are not isolated.
- **The signal.** Both Livery vtables carry the buy screens' shared Show 0x9C3960, IsOpen 0x9C3290 and Close
  0x9C22A0. Show sets byte +0x79 and Close clears it, so F209's existing hooks see Livery once its filter takes
  the two vtables (CANDIDATE until flown). The SuitUIStateMachine has states "Vendor Suit Livery" and "Vendor
  Weapons Livery".
- **Not established:** what draws the showroom from the commander's camera in HMD mode, and the consumers of the
  projection.

## Flight F228 (2026-11-09 08:21, Quest 3 under VirtualDesktopXR, Steam, c25fac3e)

Build matched (`--expect-build c25fac3e`). The pilot: "the model is now behind me with the key turned on, and
stands over me with it off". Eye dumps: `082443795001` (key on) and `082506704002` (key off), both looking at
the UI. They show the LIVERY / GENERAL panel front-left, a "rotate" hint to its right, and no model in the empty
space where it belongs. `082529135003` and `082540518004` are the loadout browser, with the model standing to the
right of its panel, correctly.

- **The signal works.** `HumanoidLiveryActivity Show` at 08:23:47.860, the shop signal ON, and Close at
  08:25:16.502. The key changes at 08:24:07.9 (off), 08:24:15.7 (on) and 08:24:57.9 (off) each flip the signal.
  With the key on, F107 refuses every call (episode 4: 3630 refused, "away by: the shop screen"), with the game's
  camera 0.3 to 0.5 m from the neck.
- **The render camera** (`concourse frame: cam_local`, once a second):
  - 08:23:41-44: the showroom spot (6.0 20.0 -17.5), about 21 m from the neck;
  - 08:23:45-46: the preview camera (6.62 23.09 2.29), 3.9 m from the neck;
  - 08:23:47: the showroom spot once more, and Livery's Show;
  - 08:23:48 to 08:25:15: the commander's own position (4.8 22.67 3.28), with the key on and with it off;
  - 08:25:17-37 (the loadout browser): the preview camera again (6.72 23.10 2.30, F107 at 3.9-4.0 m).
- The first flight had the same sequence: 3.9 m at 06:39:58-06:40:02, then the commander's camera from 06:40:03.

So the model stands where the game puts it, at the commander, and Livery renders from inside it. The loadout
browser keeps the preview camera and is right.

- ruled out: H4 as the whole cause, because with EDVR's on-foot adjustments held off for the whole screen
  (3630 calls refused) the model is behind the pilot, not behind the UI.

## The Pimax visit: EDVR stood down there and did not on the Quest

**Corrected after F228:** this stretch's signature (a far showroom camera, then a 4.0 m camera that restarts F107
every 8 s) is the loadout browser's in the F228 flight (08:25:19-08:25:40), not Livery's. Read it as a loadout
browser visit. The comparison below does not stand as evidence about Livery.

The pilot's correct Pimax view was on the same Steam install (Pimax OpenXR, `edvr_gfx_20261109_045925.log`,
d5241a7b). Its showroom visit (05:04:06 to past 05:06:44) has the same opening as the Quest's: a far showroom camera
(39 m), then a camera 4.0 m from the neck. Then the two differ:

- **Pimax:** F107 episodes 6 to 19 each last 8.0 s with exactly one far call ("by the distance test 1") and about
  715 near calls. The game's camera was on the commander nearly all the time there too. But one 4.0 m read about
  every 8 s restarted the episode the moment the last one ended, so the body pin refused 712 to 722 calls in
  every 8 s and the raise stayed ramped out for the whole visit.
- **Quest:** the far reads stop at 06:40:02. F107's episode ends at 06:40:07 and the pin accepts every call (770 of
  770) for the whole bad view.

So in these logs the model is right while EDVR's on-foot machinery is held off, and wrong while it is engaged.
Whether the Pimax stretch was the Livery screen is not proven from the log (the pilot's account and the matching
signature say so). Why the game gives the 4.0 m read on one rig and not the other is not known; it may not be
per-headset at all.

## The A/B instrument (proposed earlier, now second)

A temporary key that makes the runtime answer Elite with the frustum widened to vertical symmetry: the short edge
(up, 0.90) raised to the long edge's tangent (1.33). The projection layer is submitted with the same widened FOV, so
the image maps one to one and the compositor drops the extra rows. Cost on the Quest: about 19% more rendered rows
while it is on. The `projection_query` lines already show m12, so the log proves the key took effect.

This test does not settle the fix. If H1 holds, there are two candidate fixes:
- the symmetric frustum only while a preview screen is open, which needs a signal for that screen;
- correcting the game's placement itself, which needs the anchor's code by RE.

## Ruled out

- ruled out: H2, the Steam install's `edvr.ini`, because the pilot's Pimax view with the model behind the UI was
  on the same Steam install and ini (2026-11-09). Today's Steam Pimax sessions (Pimax OpenXR) ran d5241a7b and
  a89e193e, a few concourse-cull commits before 65dc3884.
- ruled out: H1 in its strong form, the Livery placement code reading the projection, because the placement
  region 0x1268000-0x1276000 and the activity's open, close, start and stop read no projection element, FOV or
  HMD input (RE above). The model's place is authored node geometry.
- ruled out: the showroom camera distances (22 m, 39 m, 3.9-4.0 m) as a computed fit-to-view distance, because
  the camera nodes are authored scene data and the camera code has no FOV or tangent math.
- ruled out: the Steam ini's `openvr.head_offset_forward/right/up`, `openvr.head_yaw_degrees` and
  `fix.head_offset_gate`, because no source file reads them (only `tools\config_test` and docs name them).
- ruled out: a zero seated-to-standing transform, because Elite never called
  `GetSeatedZeroPoseToStandingAbsoluteTrackingPose` in this flight (the runtime log's `system_unavailable` lines are
  slots 23 and 26 only, not 12).
- ruled out: a floor-height difference between the runtimes' natural spaces, because both logs read `LOCAL in STAGE
  (0.000 0.000 0.000)` (STAGE coincides with LOCAL on both).
- ruled out: EDVR's on-foot view offsets as the cause, because they are centimetres (raise +7.0 cm, virtual neck 0)
  and the model is metres off. F107 kept the camera as the commander's throughout, which is correct: it is.
