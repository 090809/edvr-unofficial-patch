# VR on foot: doubled HUD with the Genetic Sampler

## Status

- **State: OPEN.** One report, from a user on v0.22.0-rc.7 in VR with native
  on foot: "Double vision hud when using genetic scanner". Issue #200.
- **The screenshot shows the whole HUD twice in one eye.** The sampler is in
  hand in SAMPLE MODE. The scene and the tool are single. Every HUD element
  has a second copy of the same size, offset by one vector: about 200 px left
  and 115 px down in the 1362 x 904 image. That covers the radar, the top info
  panel, suit energy, the vitals, the green OFF circle, and the sampler's ring
  with its three bars. The copies differ by draw. The radar, ring, OFF circle
  and suit energy are bright in the upper copy, with only a dark disc or bar
  in the lower. The top panel's labels are bright in the lower copy and its
  values in the upper. The desktop window on this rig shows the right eye
  (the log's `desktop window` line: setting right_eye).
- **The report's log can't show it.** `edvr_gfx_20261106_115201` (build
  6AED3509, game build 334925) hit the 16 MB cap at 12:20:05, 28 minutes in,
  before the sampler was held (#199).
- **Environment:** SteamVR OpenXR, a lighthouse headset, RTX 4080, DLSS model
  K at 1234x1370 into 2468x2740 per eye, world scale 100. HUD settings: follow
  body, distance 0.19, size 1.00.
- **No EDVR flight has held the sampler** (probably
  `$humanoid_sampletool_name;`). F91 C keeps EDVR's ADS off for tools.

**Open hypotheses**, each with the line or capture that would confirm it:

- H5: in sample mode the game renders the HUD a second time (another HUD
  component or view), and EDVR's HUD placement covers only one of them. F12
  and F13 keep their state for one HRC (`g_f12Hrc`, `native_on_foot.cpp`).
  Confirmed by F116's `HRC calls` exceeding one a frame in sample mode, by a
  second `hrc=` address in the `native on foot hud part:` lines, or by the
  game's own eye frame in a dump already holding two HUDs.
- H6: EDVR splits the HUD's draws between its UI layer and the game's frame,
  and in sample mode the two halves land in different places. This fits the
  per-draw split of bright and dark between the copies. Confirmed by the
  game's frame in a dump holding one copy where the door's output holds two,
  by the `ui quality: layer:` redirect counts and `composites left in the
  scene` changing in sample mode, and by the double going away with AA off,
  which leaves no layer.

**Ruled out** by the screenshot, given that it is one eye's image:

- ruled out: H1 (one tool readout part left unpinned), because the whole HUD
  doubles, the radar and the vitals included.
- ruled out: H2 (true stereo of a screen held close to the eye), because the
  doubling is inside one eye's image.
- ruled out: H3 as first stated (one overlay ghosting in DLSS), because the
  whole HUD is copied with one offset and no trail. Its composite part is
  folded into H6.
- ruled out: H4 (a world marker without the marker fix), because the
  head-locked HUD doubles.

**Next flight (no build).** Native on foot with `log.on_foot_detail = on`, on
a planet with organics. Hold the sampler 20 s outside sample mode, then 20 s
in sample mode. Press F9 and take an eye dump while it doubles. Do the same
with AA off, then 20 s with the Energylink for comparison. Read `--health`,
then F116, the `hud part` lines, `ui quality: layer:` and `composites left`.

**Ask the reporter:** does it still double with AA off in the EDVR menu? Does
it double with the sampler in hand outside sample mode, or with other tools?

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
