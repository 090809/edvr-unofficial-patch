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
- **Leading hypothesis (H4):** EDVR's native on-foot machinery (the body pin, the neck reference's raise)
  misplaces the Livery model whenever it is engaged during the screen. On the Pimax it happened to stand down; on
  the Quest it did not (see "The Pimax visit").
- **Open:** H4; H1 in its weak form (the projection consumed downstream of the placement, by the render camera);
  H3. H2 (the Steam install's ini) is ruled out.
- **RE (2026-11-09, build 334925):** the Livery placement is authored scene geometry and reads no projection
  element (see "RE"). The notes wait in the analysis inbox, `20261109-090000-livery-anchor`, not yet filed.
- **Next flight (proposed, not built):** extend F209's buy-screen signal to the two Livery activities, so F107
  holds the camera away while Livery is open, behind a temporary A/B key. On the Quest, open Livery with the
  key on and then off, with an eye dump in each. H4 predicts the model behind the UI with it on. If it is not,
  the symmetric-frustum A/B (H1) is next.
- **Temporary keys:** none yet.
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

## The Pimax visit: EDVR stood down there and did not on the Quest

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
