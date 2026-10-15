# Surviving a CS2 update: world and effects

**Read this first when CS2 has updated and something on the Lighting, Atmosphere or
Weather & Sky surfaces stopped working.**

The repo-wide runbook is `CLAUDE.md`. This page covers only the
controls listed in [`README.md`](README.md) and the renderer checks below.
A successful schema report does not confirm renderer compatibility after a game update.

---

## 1. The one command

```
mirv_filmmaker world health
```

It prints, for every schema class and field these features depend on:

- the class name and field name **as queried**,
- the offset the live game resolved it to (or `MISS`),
- which features are consequently `LIVE` or `DEAD`, and for each dead one, the exact field
  that killed it.

Check `mirv_filmmaker health` too. A `MISS` means the field did not resolve; inspect
the live class/module and field name before changing code. Resolved fields do not prove that
hooks, shader layouts, attachments or rendering still work.

Environment VFX fields resolve through `SchemaSystem.cpp`, including inferno burning positions,
active-flame flags/lifetime and Molotov pin state. After an update, verify native smoke shader
replacement, a real shot versus empty clicks, standing/crouching muzzle attachments, two active
fires, world weapon shadows and stained-glass wall occlusion. Capture `mvm_debug` evidence and
compare fixed-camera frames; reference offset dumps alone cannot validate these paths.

The same report goes to the `mvm_debug` log (`world.health` lines), so you can read it
later instead of copying it from the console.

---

## 2. What "broken" looks like, by layer

These features rely on three kinds of dependency. Each fails differently, and
knowing the layer tells you how much work the fix is.

### Layer 1: schema fields (nearly everything here)

Resolved **by name at runtime** in `Filmmaker/World/WorldSchema.cpp`. This is the resilient
layer. If Valve moves a field, nothing happens, because nothing hardcodes an offset.

It breaks only when Valve **renames or deletes** a class or field.

- **Symptom:** one feature goes dead, everything else is fine. `world health` names the field.
- **Fix:** find the new name, change the string in `WorldSchema.cpp`, rebuild. Usually one line.
- **Finding the new name:** dump the live schema. `SchemaSystem.cpp::dumpEconSchemaToFile()`
  already writes classes and fields to `%TEMP%\hlae_schema_econ.txt` on every launch; the
  world classes are added to the same dump. Search it for the nearest surviving name.
  `Misc/cs2-offsets/CS2-OFFSETS-main/client_dll.cs` is a useful cross-check once it has been
  regenerated. Use it **for names only**, and never take an offset from it.

### Layer 2: the scene-draw hook (world, sun and cloud modulation)

`WorldModulation` reads through the existing `CBaseSceneObjectDesc::DrawArray` detour installed
at `SceneSystem.cpp:849` via RTTI vtable lookup, not a byte pattern.

- **Symptom if the vtable moves:** modulation silently does nothing, or (worse) writes into the
  wrong struct. A change to the `CBaseSceneData` colour offset produces garbage rather than a clean failure.
- **Guard:** `WorldModulation` validates the scene-data stride and colour-field position against
  a known-good shape before it writes anything, and refuses to modulate (reporting why) if the
  shape does not match. It fails **closed**.
- **Fix:** re-derive the `CBaseSceneData` layout and update the struct.
  `mirv_filmmaker world modulate stride <n>` arms `WorldModulation_ProbeBatch`
  (`Filmmaker/World/WorldModulation.cpp`) for the next `n` multi-element batches and reports, on
  the `world.mod` `mvm_debug` channel, which candidate stride reads back as real pointers. That
  probe finds the array stride. `... world modulate matprobe <n>` does the same for material colour
  parameters.

### Layer 3: convars

Some features turn out to be driven by a convar rather than by memory at all. Night Mode is the
first: `mat_tonemap_force_scale` (see [night-mode.md](night-mode.md) 3.3).

This is the **most** resilient layer. A convar has to be renamed or removed to break, and both
are rare and immediately obvious.

- **Symptom:** the control does nothing; running the convar name in console reports unknown.
- **Fix:** `find <keyword>` in the CS2 console to locate the replacement, change the one
  constant in the feature module (`kScaleConvar` in `NightMode.cpp`), rebuild.
- **Caveat that costs an hour if forgotten:** most of these are cheat-flagged. A feature must
  issue `mirv_cvar_unlock_sv_cheats` + `sv_cheats 1` before its first write, or the convar
  silently refuses to change and looks identical to a rename.

Prefer this layer when it exists. For Night Mode, the convar changes the image, and the memory
writes in the reference research do not.

### Layer 4: byte patterns

There should be none for these features. Every mechanism here was chosen to avoid patterns.
A future change that adds one must:

- be anchored to a convar or string xref, with the anchor recorded in the feature's doc,
- be registered in `Automation/verify/verify-cs2-signatures.py`,
- document its module, anchor and failure behavior in the maintained feature contract,
- fail closed with a named reason.

---

## 3. Update-day checklist

1. Launch a demo. Run `mirv_filmmaker health`. It shows whether the tool's core
   version-coupled machinery came up at all. If that is broken, fix it first, because nothing
   here will work without it.
2. Run `mirv_filmmaker world health`. Note every `MISS`.
3. For each miss, find the renamed field in the schema dump and update the one string in
   `WorldSchema.cpp`.
4. Rebuild, re-run `world health`, confirm all `LIVE`.
5. **Spot-check behaviour, not just resolution.** A resolved offset does not prove the effect
   survived. In Night Mode, volume writes can land and still change nothing (see [night-mode.md](night-mode.md) 3.2). Check one feature per layer in a demo: a Time-of-Day preset (Layer 3),
   world modulation (Layer 2), a fog or smoke colour (Layer 1).
6. If `CBaseSceneData` garbage appears (wrong things tinted, or crashes on cloud draw), go
   straight to Layer 2 above.

### The screenshot check

For anything visual, the reliable test is to pause the demo, capture, change the setting,
capture again, and compare. A paused demo keeps the camera and frame identical, so any difference is
the setting and nothing else. Comparing two frames of a *playing* demo proves nothing.

---

## 4. Why this design was chosen

The research these features are built from is almost entirely **hardcoded-offset** code. Those threads go stale in public: the
`world-modulation/734508-light-rotation` thread has five separate people asking for updated
`CLightBinnerGpu` / `CSceneLightObject` structs over three months with no answer, and
`fog-night-mode/741472` posts `C_PostProcessingVolume` offsets (`0xF7C` / `0xF80`) that do
not match the current dump (`0x10BC` / `0x10C0`).

Reimplementing those techniques against **schema names** instead of offsets keeps these features
out of that rot. The trade-off is that a *rename* still breaks the feature. Renames are far rarer
and far cheaper to fix, and `world health` turns the fix into a named, one-line change instead of
a silent regression that someone notices weeks later mid-edit.

The repo-wide lesson is that the generated and name-resolved parts survive updates and the hand-typed parts break in silence. These
features are built entirely in the first category on purpose.

---

## Related

- [`README.md`](README.md): the master scope page
- `CLAUDE.md`: repository build and runtime diagnosis guidance
- `CLAUDE.md`: how to diagnose per-frame behaviour
