# The d3d11 proxy's handle to Windows' d3d11.dll (2026-11-07)

## Status

- **Question:** why does a Windows rig with EDHM ahead of EDVR log `0 of 96 sampled vtable entries are inside
  Windows' d3d11.dll`, then `draws=0 copies=0` and a flat stand-down (`no-known-output-copy`), when the same build
  under Proton logs 96 of 96 and LiveCopy?
- **Report (2026-11-07, a contributor, passed on by the maintainer):** v0.22.0-rc.7, build `6AED3509` (the tag
  resolves to `eedd5d04` here; `6AED3509` is in no ref of this repository). Chain edworld, EDHM, EDVR: edloader is
  the game folder's `d3d11.dll`, EDVR sits in `edloader\plugins`, `real_dll` names edloader's `d3d11.dll`. The
  player's rig is an RX 6800 on Windows.
- **Proven, from the report's log lines and the source:** `g_systemModule` was not System32's `d3d11.dll`. `0 of 96`
  is counted against that handle's image range; `d3d11.dll backs 96 of the 96` comes from the module owning the
  entries once that handle is excluded (`vtableDominantOtherModule`); both print only if the two differ. Slot 12
  (DrawIndexed) is `C:\Windows\system32\d3d11.dll+0x12DC30` in the same log.
- **Not shown:** which module the handle was (nothing logged it); that a hook on `LoadLibraryExW` returned it
  (`system_d3d11.h` documents 3Dmigoto and EDHM with `load_library_redirect = 2` doing so; this rig's EDHM
  settings were not seen); that `draws=0` is reclaim leaving Windows' re-points alone (below); why Proton resolved the
  right module.
- **Mechanism, read from the source:** `loaderPhase` takes the handle from `LoadLibraryW(<System32>\d3d11.dll)`
  in DllMain and trusts it. The probe counts against it; `systemD3D11Module()` hands it to
  `setImplementationModule` (`vscreen.cpp`, `exposure_fix.cpp`), and reclaim adopts a re-point with no traffic
  evidence only when the new entry lies in that module (`vtable_hook.cpp`, `ownerRepoint`). Wrong handle: InPlace,
  and a runtime that re-points the patched slots before the first frame (issue #21) is left alone, so no EDVR hook
  runs.
- **Fix, on branch `claude/windows-d3d11-module-resolution-61f112`:** `chooseSystemD3D11` (`system_d3d11.h`) checks
  the handle against the module's own file path and, when it is not System32's, finds the System32 module by a lookup of
  its full path or by walking the module list. `probeSystemModule` (`d3d11_proxy.cpp`) calls it at the first
  probe, once a device exists, and writes one `system d3d11.dll:` line saying which route was taken and what the
  DllMain handle was. Rig `system_module_test`. Not flown; not run on the reporting rig.
- **To read after a flight on that rig:** the `system d3d11.dll:` line (names the substitute, if there was one);
  `context hook mode:` (expect LiveCopy, 96 of 96); no `no longer holds EDVR's hook` lines; draws above 0. If the
  line says the handle was Windows' own, this fix is not the cause of `draws=0` and the `no longer holds` lines
  are the next read.
- **Not folded in:** the loader phase still uses the unchecked handle for the export fallback in
  `chainThroughOtherProxy` and for `g_systemCreateDevice` and `g_systemCreateDeviceAndSwapChain` (the re-entrancy
  route). `advanced.context_hook_mode` is not on the flat allow-list (`flat_profile_keys.h`), so flat reads it as
  `off`; a one-off test build adds the key to that list. No temporary key was added.
- **Ruled out:** the loader alone answering a full path with a same-named module mapped first, because rig case S3
  got System32's module back with a decoy `d3d11.dll` already mapped (Windows 11 10.0.26200); a hook is needed.

## Journal

### 2026-11-07: the check, and the rig

Read: `d3d11_proxy.cpp` (`loaderPhase`, `contextHookModeFor`), `proxy.cpp` (`loadRealModule`), `vtable_hook.cpp`
(`vtableEntriesInModule`, `vtableDominantOtherModule`, reclaim), `system_d3d11.h`, the flat allow-list.

Changed: `system_d3d11.h` (`mappedD3D11Modules`, `walkMappedD3D11AtPath`, `chooseSystemD3D11`;
`mappedD3D11Paths` is now built on the first), `d3d11_proxy.cpp` (`probeSystemModule`; the probe and
`systemD3D11Module()` read it), `device_hook.h` (comment), `build.bat` (the rig's block),
`tools\system_module_test`.

The rig stages a copy of `version.dll` named `d3d11.dll`, maps it first, then System32's. Run alone, on the fixed
header: S0 to S7 pass. Run on a scratch header that keeps the old behaviour (trust the handle as it came): S2, S5
and S6 fail, so the rig fails without the fix. It does not hook `LoadLibraryExW`; it stands in for the substitute
with a decoy handle.

Validation: `build.bat --wait-lock` on the working tree over `4aeb712e` (describe
`v0.22.0-rc.7-53-g4aeb712e-dirty`): all gates passed, 348.8 s, `system_module_test` and `flat_temporal_test`
among them. The first full build failed `flat_temporal_test`: a source-text pin held the old `g_systemModule`
argument of `vtableDominantOtherModule` in `d3d11_proxy.cpp`. It now holds `systemModule`, and two more pins hold the
probe to `probeSystemModule()` and the entry count to that module. Not done: a flight, a run on the reporting rig, a
`mutants.py` for the new rig, `build.bat --dll-only`.
