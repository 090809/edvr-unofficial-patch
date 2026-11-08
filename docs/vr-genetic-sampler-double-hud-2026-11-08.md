# VR on foot: doubled HUD with the Genetic Sampler

## Status

- **State: OPEN, no evidence yet.** One report, from a user on v0.22.0-rc.7 in
  VR with native on foot: "Double vision hud when using genetic scanner".
  Issue #200.
- **The report's log can't show it.** `edvr_gfx_20261106_115201` (v0.22.0-rc.7,
  build 6AED3509, game build 334925) hit the 16 MB cap at 12:20:05, 28 minutes
  into a session that ran until 14:46. Before the cap the user held fists, the
  Energylink, the Arc Cutter, a laser SMG and a kinetic pistol, never the
  sampler. No aim press was made on a tool (`ADS item gate (F91)` IDLE). Why
  the log fills so fast on foot: issue #199.
- **Environment:** SteamVR OpenXR, a lighthouse headset, RTX 4080, DLSS model
  K at 1234x1370 into 2468x2740 per eye, world scale 100,
  `native_on_foot_ads_sight_eye = both`.
- **What EDVR knows about the sampler: nothing measured.** No flight in the
  native on-foot arc (`design-native-onfoot-vr-2026-10-13.md`) has held it,
  and its item name appears nowhere in `src\`, `tools\` or `docs\`. It is
  probably `$humanoid_sampletool_name;`; confirm from Status.json. F90's part
  table has one generic tool readout row (mesh 2 x 1, scale 0.094), named in
  the log but never scaled or moved. F91 C keeps every EDVR ADS feature off
  for a tool, so an aim press with the sampler gets only the game's behaviour.
- **Related:** a tester's weapon report of 2026-10-27, "HUD doubled at ADS",
  had an unproven lead: the optic composite VS 025B4B9FF54622ED /
  PS 46F92DC71BF8DFA5 left in the DLSS scene. The same VS is in this log's
  `composites left in the scene` line.

**Open hypotheses**, each with the existing line that would confirm it:

- H1: the sampler's display is a weapon UI part that F85b doesn't pin, so it
  is drawn from one camera in both eyes. Confirmed by `weapon UI pin (F85b)`
  refusals or not-found counts rising while it is held, and an F91 role line
  reading `NO TABLE PART` for it, with an F141 `readout anchor` line.
- H2: the game's aim mode brings the sampler's screen close to the eye, and
  F91 C leaves it in true stereo, without the single-image sight F106 gives a
  weapon. The game's aim FOV reaching the eye projections is a variant.
  Confirmed by `ADS item gate (F91): refused` for the sampler at the moment it
  doubles, and by the double appearing only while aiming.
- H3: the sampler's overlay is an interface composite left in the DLSS scene.
  That would be a temporal double that trails with motion, not a stereo one.
  Confirmed by a new VS/PS pair in `composites left in the scene` with the
  sampler up, and by the double going away with AA off.
- H4: the sampler's world marker (the sample point or the colony range ring)
  is a marker draw the F62c/F64 marker fix doesn't patch. Confirmed by the
  `marker fix` applied share falling, and its `outside` count rising, with the
  sampler out.

**Next flight (no build: every signature is an existing line).** Native on
foot with `log.on_foot_detail = on`, on a planet with organics. Hold the
sampler 30 s without aiming, then aim at a plant 30 s and take a sample. Press
F9 where it doubles and take an eye dump of the double. Repeat the 30 s with
AA off. Read `edvr_log.py --health`, then `--grep` for F85b, F91, F141,
`marker fix`, `composites left` and `ADS item gate`.

**Ask the reporter:** which part doubles (the tool's own screen, a ring or
marker in the world, or HUD text); whether it doubles while only holding the
sampler or only while aiming it; and whether the double trails when they turn.

**Ruled out:** none yet.

## Journal

### 2026-11-08: the report and its log

The bundle (`edvr-logs-20261106-202242`) holds the graphics log, the runtime
log (`edvr_openxr_20261106_115203_977_9852`, which ends 13:45:21), the
breadcrumbs, both inis and the game's graphics settings. `--health` covers
11:52:02 to 12:20:05. After the cap the log has only `mem:` lines, so nothing
on foot after 12:20 is recorded. The breadcrumbs carry heartbeats only, no
items.

For the Energylink, which the user did hold, F141 reads its readout parts as
`neither weapon- nor avatar-anchored`, and F91 lists several parts, the
Energylink's among them, as `NO TABLE PART`. The user didn't report a double
with it or with the Arc Cutter. If H1 holds for the sampler, these lines are the
model of what its own lines would show.
