# VR on foot: doubled HUD with the Genetic Sampler

## Status

- **State: OPEN, two parts.** Issue #200. A user on v0.22.0-rc.7 (VR, native
  on foot) reported "Double vision hud when using genetic scanner". Sean
  reproduced it on Frontier on 2026-11-08 and also found the sampler's pose
  wrong.
- **The double is the right eye's UI layer, with the sampler only.** In eye
  dump `132928432001` (sampler in SAMPLE MODE), R00 had its composite skipped
  and shows the whole HUD where the game drew it. R01 to R15, composited, show
  the bright HUD (radar, suit energy, the ring) (+607, +69) native px away,
  about 17 deg, with only the HUD's dark backing left at the right place. The
  left eye is right. 30 earlier dumps with other items agree within 5 px.
- **Its shape:** each eye's composited HUD lands outward of the game's (the
  right eye's to the right, the left eye's to the left), by a size that varies:
  607 px, then 288 and 126 (native, right eye). `marker_bound` in
  decisions.json is [3, 0] in R00 (right) and [3, 3] from R01 (displaced).
- **The left arm, two flights.** The two-hand detector acquires within
  kOffHandMaxM (40 cm), and the sampler's resting left hand sits at 40.0 cm,
  so it flapped (elbow 28 cm over the hand). ea13030c vetoed it (flown OK: the
  left arm wrote on 26 of 7131 calls), but the left hand also takes the
  canister out after the third sample (18 to 26 cm), and that came out with no
  hand. The build after it gives the sampler its own limits instead: acquire
  within 32 cm, release past 35 (`offHandLimits`). Not flown.
- **Held higher: the game's pose.** At level aim the game holds the sampler
  3.6 cm higher and 5.4 cm nearer its camera than the Profile Analyser, and
  EDVR adds the same offsets to both. A per-item correction is Sean's call.
- **Environment (Sean):** Frontier, game build 334925, OpenXR, DLSS at
  2016x2017 into 4032x4034, world scale 110%. Reporter: SteamVR lighthouse,
  RTX 4080, DLSS K.

**Open hypotheses** for the double, with the A/B that splits them:

- H7: a per-draw constant patch bound only on layered draws (the marker
  fix's private b0, which F85b also binds through) is applied to the HUD
  canvas draw in the right eye with the wrong camera. Confirmed if the double
  goes with `fix.native_on_foot_marker_fix = off` (live; it stands down F85b's
  binds too).
- H8: the layer's own take of the HUD places the right eye's draws wrong
  without any patch. Confirmed if the double survives H7's switch and goes
  with `fix.ui_quality = off` (live), which leaves the HUD in the game's
  frame.

**Ruled out** (evidence in the journal): one unpinned tool part, true stereo
at close range, a DLSS ghost of one overlay, a world marker missing the fix,
a second unplaced HUD (frame 0 shows one HUD), the layer's eye table (0
SWAPPED, 0 untold), aim-space reprojection (head 4.6 deg from the aim), the
viewport remap, the marker fix's logged move (+0.00003 NDC), the label
features (F95b, F97b, F98b), and F151's place bind (0 bound by place).

**Next flight** (the limits build): native on foot, detail log on. Take three
samples: the left hand should take the canister out, and rest otherwise (the
F69h line: `decision two-handed` only around the canister). When the HUD
doubles, take an eye dump, set the marker fix off and dump again, then UI
quality off and dump again, then put both back.

## Journal

### 2026-11-08: the report and its log

The bundle (`edvr-logs-20261106-202242`) holds the graphics log, the runtime
log (`edvr_openxr_20261106_115203_977_9852`, which ends 13:45:21), the
breadcrumbs, both inis and the game's graphics settings. `--health` covers
11:52:02 to 12:20:05. After the cap the log has only `mem:` lines, so nothing
on foot after 12:20 is recorded. The breadcrumbs carry heartbeats only, no
items. Before the cap the user held fists, the Energylink, the Arc Cutter, a
laser SMG and a kinetic pistol, and made no aim press on a tool
(`ADS item gate (F91)` IDLE).

For the Energylink, F141 reads its readout parts as `neither weapon- nor
avatar-anchored`, and F91 lists several parts, the Energylink's among them, as
`NO TABLE PART`. The user didn't report a double with it or with the Arc
Cutter.

A tester's weapon report of 2026-10-27, "HUD doubled at ADS", had an unproven
lead: the optic composite VS 025B4B9FF54622ED / PS 46F92DC71BF8DFA5 left in
the DLSS scene. The same VS is in this log's `composites left in the scene`
line. If that report was the same whole-HUD copy, the trigger is the game's
aim or tool mode rather than the sampler itself.

### 2026-11-08: the screenshot

The user sent one image, captioned "Double Vision". The positions below are
measured on it. The upper copy is A and the lower copy is B, with B about A +
(-200, +115) px.

- Sampler ring: A centre (840, 460), bright. B is a dark ring of the same
  size near (630, 570). The three blue bars at A (860, 580) have three dark
  bars at B (640, 680).
- Radar: A (300, 280), bright. B is a dark disc at the left edge near
  (110, 395).
- Green OFF circle: A (320, 740), bright. B is a dark circle near (110, 850),
  with faint OFF text.
- Suit energy: A (815, 200), bright, `100%` at (910, 215). B has a faint `100`
  near (700, 330) beside a dark bar.
- Top info panel: A's values (`CL-Y D16 A 11 A`, `0.41G`, `YES`, `157K`) are
  bright at y 130-140, and its labels are lost against the lit ramp door
  behind. B's labels (`MOON`, the region name, `GRAVITY`, `ATMOSPHERE`,
  `TEMPERATURE`) are bright at y 225-255, and its values are faint.

Not established from the image: which copy is the one EDVR placed, and
whether the other eye shows the same pair.

### 2026-11-08: Sean's flight on Frontier (build 4e7aefde)

Log `edvr_gfx_20261108_131702` (v0.22.0-rc.7-61-g4e7aefde, the flat DLSS
dropout branch, which has no on-foot change against d460e785), detail log on,
recording `20261108_132840`, eye dump `132928432001` (13:29:28.4) and its
airborne companion `132937599002` (13:29:37). The sampler's item name is
`$humanoid_sampletool_name;`.

Timeline. Elite's journal clock runs 21.5 s ahead of the PC's: Disembark
20:29:01Z against the recorder starting at 13:28:40.7, and Embark 20:30:13Z
against F115's boarding at 13:29:51.0. ScanOrganic (Log, Bacterium Cerbrus) at
20:29:54Z is therefore about 13:29:32.5 on the PC. The dump's own write froze
the game from 13:29:29.6 to 13:29:33.0 (`monitor: FREEZE -- 3386.7 ms`), so
the sample completed inside that freeze. The 16 dump frames are from about
4 s before it, with the sampler in SAMPLE MODE. The companion frames show
"PARTIAL SAMPLE OBTAINED - 1/3".

The dump shows the double, in the right eye only:

- R00 has `composite_applied: false` in `eye_132928432001_FinalCrisp.json`.
  Its HUD (radar, suit energy, the sampler's ring around the SRV's marker) is
  where the game drew it.
- R01 to R15 have the composite applied. The whole bright HUD sits
  (+607, +69) native px from R00's, by FFT registration of the HUD's cyan
  pixels over the 1400 px centre crop: the ring's bars at +16.5 deg, against
  -0.4 deg in R00. Only a dark ring and three dark bars (the HUD's backing,
  left in the game's frame) remain at R00's place. The readout part F85b pins
  ("GENETIC SAMPLER / SAMPLE MODE") moves about 40 px, not 600.
- The left eye's HUD is the same in all 16 frames (L00 to L01: (-3, +2) px),
  with the ring near the view's centre at (+1.1, +2.6) deg.
- The right eye's layer is live, not stale: elements that appear during the
  16 frames ("Discovered", the ring's centre icon) appear in it.
- The companion (13:29:37, sampler still held): R01 skipped, the next frame
  (+126, +25) px from it.
- 30 earlier FinalCrisp dumps on this install with a composite-skipped frame
  (other items, unarmed, aboard): the skipped frame and the next agree within
  5 px in all but one (about 70 px, likely head motion). The displacement
  needs the sampler.

The log at the dump: `ui quality: layer: interface composite (vs
B10B032BDFD46700 ps DB899F4BD577F2E5) into a 2016x2017 R8G8B8A8_UNORM target:
eye unknown` and the same for a new pair, VS C4B4B334B26E81A9 /
PS 0146ABCC53240479, both at 13:29:28.53. Over 13:29:03 to 13:29:33,
`composites left in the scene` lists VS 025B4B9FF54622ED / PS
46F92DC71BF8DFA5 at 2.00 a frame (one per eye, not taken). The on-foot marker
fix was 100% applied in both eyes with no refusals.

The left arm with the sampler (recording, item serial 2 = the sampler):
F221 found the left elbow 27.0 to 27.9 cm over the hand at the hip four
times in 15 s, and F218/F219 found the left elbow stepping over 5 cm against
the chest 5.1 times a second while walking. The IK events include 26
`grip_loss/pole_change` (left-hand grip error out to 33 cm). `left elbow
(F98a)` with the sampler: the swivel off the stance plane 67-89 deg mean (max
102), over 45 deg in nearly every call; flexion animated 19-25 deg, solved
72-117 deg. The cause: `gameAuthoredLeftArm` (`native_on_foot_core.h`) named
the Energylink and the Profile Analyser, but not the sampler. So the two-hand
detector ran for a one-handed tool and pulled the left hand toward the
first-person left hand.

Held higher. EDVR treats the sampler as it treats the Profile Analyser:
weapon weight 1.00, the eye 10 cm forward (F88), the neck lowered about 10 cm
(F97a), the armed eye level 1.670 m against 1.657 m (F98c). The eye-to-rig
line (F87h, `eye to weapon root`, hip, median over Sean's recent logs) puts
the first-person rig's root 3.0 cm above the eye and 1.4 cm behind it for the
sampler. For the Profile Analyser it is 5.2 cm below and 9.3 cm behind, for
the pistol 4.0 and 10.9, for the rifle 8.1 and 12.7. Matched for aim pitch
(|pitch| under 8 deg, the pitch from F137's line in the same second), the
eye is 1.2 cm over the sampler's root and 3.9 cm ahead of it (n 13), against
4.8 and 9.3 for the Profile Analyser (n 16). So at level aim the game holds
the sampler 3.6 cm higher and 5.4 cm nearer its camera than the analyser, and
EDVR adds the same offsets to both.

Ruled out for the right eye's displaced HUD (the flight's own lines):

- ruled out: the UI layer filing the right eye's draws under the wrong eye,
  because the eye check against the game's Submit read 4948 matched,
  0 SWAPPED, 0 could not be told (13:29:03) and 4563, 0, 0 (13:29:33). The
  only no-eye draw was one dust draw in the dump's first frame.
- ruled out: aim-space content skipping the game's reprojection, because
  F85b put the head 4.6 deg from the aim at the dump (yaw -1.32, pitch -3.06)
  and 5.2 deg at the companion, against offsets of 16.9 and about 3.5 deg.
- ruled out: the layer dropping a viewport origin, because
  `uiLayerMapViewport` maps the origin (`ui_layer_math.h`).
- ruled out: the on-foot marker fix moving the HUD, because its patched rows'
  translation in the right eye was x +0.00003, y -0.00166 NDC (13:29:20,
  F66f/F67d line).
- ruled out: the label features (F95b, F97b, F98b), because the eyes agreed
  to 0.18 px and the SRV's nameplate stays put between R00 and R01.

The displacement's shape. The HUD-plane draws' b0 rows (decisions.json) carry
no translation, and their rows 5 and 7 are the same in both eyes. Only row 4's
third term differs (+0.2014 against -0.1548). The right eye's shift,
+0.285 NDC across and +0.034 down at 2016x2017, is what a point at the HUD's
0.19 m shows from a camera about 5.8 cm to the side and 0.7 cm up or down,
which is the eye separation (Elite's 55.1 mm at world scale 110%). Swapping in
the left eye's row 4 would give 0.356 instead. So the right eye's redirected
HUD is placed as if from the left eye's camera position, through the right
eye's own projection. (Retracted the same day: see the next entry, where the
size varies.)

### 2026-11-08: the second flight (ea13030c, Frontier)

Log `edvr_gfx_20261108_142644`, build v0.22.0-rc.7-241-gea13030c (main
merged, the sampler in `gameAuthoredLeftArm`), recording `20261108_143436`,
dumps `143515440001` (approaching a site) and `143543183003` (the canister),
each with an airborne companion. Journal offset about 21.5 s again. Six
samples over three sites; the third sample of the last site at about
14:35:45.5, its Analyse at about 14:35:52.5. F9 at 14:35:42.55.

The left arm at rest: fixed. The arm wrote on 26 of 7131 calls (2479 of 5353
in the first flight), and F218/F219/F221 found nothing.

The canister: `143543183003` shows it floating beside the sampler with no
hand. F69h's line gives the FP left handAttach to the right as 40.0 cm at
rest (37.9 to 40.7 over the flight, on kOffHandMaxM), 18 to 26 cm from
14:35:41.6 to 14:35:51 (held at 25.8), then 40 again. `armed arms (F67f)`
reads `L solved 0 ... the off hand is free (F69h)` through all of it. The
veto kept the 3P left arm out of the game's canister animation.

- ruled out: the sampler as a tool whose left hand never holds anything,
  because its FP left hand comes to 18-26 cm and holds the canister after the
  third sample.

The double: `143515440001` shows it in both eyes. L00, composited, has two
radars, two top bars, two vitals panels and two "READY TO SAMPLE" rings, the
bright copy about 105 px (overview scale, about 420 native) left of the dark
one. R00 had its composite skipped and is single; R01 to R15 sit 288 px right
and about 50 px up of it. So both eyes' composited HUDs land outward, and by
a different size from the first flight's 607 px. That rules out a fixed
eye-separation error (a 0.19 m point from the other eye). The A/B switches
were not flown.

The readout in `143543183003`'s overviews looks vertically apart between the
eyes (about 60 overview px). R00 is a composite-skipped frame, so its readout
is the game's unpinned one. This is not evidence until a composited pair is
compared, and the readout lies outside the dump's centre crops.
