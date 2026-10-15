# Smoke colour

For live density, wispiness, noise, cloud spread and rendered voxel size/count controls,
see [native smoke parameters](smoke-parameters.md).

## Environment smoke illumination

With Environment relighting enabled, native smoke receives the current sun, local map/VFX
lights, world shadows and live probe irradiance inside its ray marcher. Density, transparency
and volume detail remain native.
Unsupported shader layouts retain the native program. Disabling Environment restores native
lighting. See [whole-map relighting](whole-map-relighting.md) for implementation and limits.

The sections below cover the separate `mirv_colors` smoke tint feature and its spawn,
reapply and fade behavior. Relighting does not fix those tint limitations.

Recolours CS2's volumetric smoke grenades by throwing team.

- Backend: `AfxHookSource2/MirvColors.cpp`, `mirv_colors smokes t|ct`
- UI: CONFIG → EFFECTS → Atmosphere (`Panorama/ConfigHudAtmosphereTabJs.h`)
- State mirror: `MirvColors_GetSmokeColorName()`
- Status: SHIPPED

---

## 1. Existing backend

The smoke colour backend is an existing, working, well-placed implementation. The UI adds a
surface and a read accessor so the panel can mirror it.

This matters as a precedent. Earlier research describes walking the entity list
and writing `C_SmokeGrenadeProjectile::m_vSmokeColor` every frame. That approach would duplicate
the hook and write at a worse time (see §2).

Check what the codebase already does before porting a reference thread.

## 2. Mechanism

`MirvColors.cpp` detours the client's own smoke-property setter
(`new_setSmokeProps`, pattern-resolved in `getAddresses()`), reads the projectile's team, and
writes `m_vSmokeColor` before calling the original.

The timing matters. CS2 smoke is a volumetric voxel effect, not a particle system, and its
colour is baked when the smoke effect is created. A per-frame entity write (the reference
approach) wastes work and races against creation. Hooking the setter puts the write where the
game sets the value.

The result users see: an already-deployed smoke keeps the colour it was created with.

### 2.1 The colour is baked at spawn

The `SmokeFx` module re-writes `m_vSmokeColor` on every live smoke projectile every main-thread
frame (the approach that earlier research describes). On a paused frame it reports
`projectiles: 1` with colours set, and the write succeeds.

The deployed smoke does not change colour. Switching the colour while smoke is on screen has no
effect. The new colour appears only on the next smoke thrown.

Conclusion: CS2's volumetric smoke reads its colour once, when the voxel volume is created.
Writing the entity field afterwards cannot change it. The per-frame write is kept because it costs
nothing and keeps the field correct when a volume spawns.

### 2.1a Fade-to-grey limitation (unsolved)

Symptom: a smoke set to magenta renders magenta while dense, then washes back toward the
game's own grey as it thins out.
The entity field is fine. The `mvm_debug` probe shows `m_vSmokeColor` holding our value for the
entire lifetime (`pre=(255.0,0.0,0.0)` on every sample, never reverted).

Cause: CS2 renders smoke from a 32×32×32 RGBA voxel volume. Each voxel's colour is baked when
voxels are generated, and voxels regenerated during dissipation come back in the stock colour.

The fix is disabled. Re-tinting the voxel buffer each frame (rewriting rgb and preserving alpha
so density still fades) is the intended approach, and the volume can be located at runtime. The
write is not yet safe, because the allocation size of the buffer is unknown.

Neither a fixed `32³ × 4 = 131072` byte size nor a bound from `VirtualQuery` is safe. A committed
region is typically a whole heap segment holding many separate allocations, so the remaining
region size says nothing about whether this allocation is that large.

`__try` does not protect against this. Overrunning into adjacent valid committed memory does not
fault. It corrupts the heap, and the game can fail later somewhere unrelated.

Current state: `TintVoxels` is hard-disabled behind `kVoxelWriteProven = false`, and
`world smoke voxel on` refuses with an explanation. The volume-location code is active because it
works and validates correctly. Only the write is off.

To finish this properly, establish the buffer's true allocation size by reversing whatever
allocates it. Do not infer it from the voxel-count assumption.
Until then, this stays off.

### 2.2 The fix that works: re-trigger, don't re-colour

Since the volume cannot be recoloured, `SmokeFx_Reapply()` restarts it instead. It writes the
colour, then rewinds `m_nSmokeEffectTickBegin` to the current demo tick. The client treats the
volume as newly spawned and rebuilds it with the colour already set.

It deliberately does not clear `m_bDidSmokeEffect`. The reference thread reports a crash from
writing that field (607639, post 12) and reports the tick field as safe, so the tick field is
the lever used.

The cost: the smoke restarts its lifetime, so it lingers longer than it otherwise would. For a
filmmaking tool, that is a fair trade, and the UI tooltip says so.

Surfaced as "Apply to smoke already on screen" in the Atmosphere tab and
`mirv_filmmaker world smoke reapply` on the console.

## 3. Verification

The backend is pattern-based, so it is checked with live playback.

1. `mirv_colors smokes t red` / `ct blue`: set, and read back correctly.
2. Demo fast-forwarded while polling `mirv_listentities` for `smokegrenade_projectile`.
3. On a hit, camera moved directly above the smoke's origin looking straight down
   (`spec_goto <x> <y> <z+420> 89 0`) and the demo paused.

Result: clearly red volumetric smoke. The backend works on the current build.

Framing tip worth reusing: top-down is the reliable way to photograph a world effect from a
blind camera placement. Straight down over the origin is almost never occluded.

The entity class name `smokegrenade_projectile` appears for both `GetClassName()` and the debug
name, with `C_BaseGrenade` as the client class. This is an instance of the
entity-class-vs-client-class distinction recorded in `World/WorldEntities.h`.

## 4. UI

CONFIG → EFFECTS → Atmosphere, card page `SMOKE`:

- Two rows, Terrorist and Counter-Terrorist, each a grid of nine 30×24 swatches
  (`default`, white, red, green, blue, yellow, cyan, magenta, black) matching exactly the names
  the backend command accepts.
- Selection shows as an accent border. The fill is the colour being chosen, so it must stay
  accurate.
- `default` has no colour of its own, so it is drawn as a neutral chip with a dash glyph.
- A `Reset both to default` button, and the "already-deployed smoke keeps its colour" note.

The panel uses swatches rather than RGB sliders. The backend takes named colours, so three
sliders would mean three command round-trips per change. Team-coded smoke also needs only a
handful of saturated hues.

State round-trips via `st.smokeT` and `st.smokeCT`, sourced from `MirvColors_GetSmokeColorName()`.
A colour set numerically over the console (`mirv_colors smokes t 255 0 0`) highlights no swatch.
That is correct, because no swatch matches it.

## 5. Known gaps

- Per-team only. There is no per-grenade or single-colour-for-all control. Per-team is the
  backend's model. Changing it means changing `MirvColors.cpp`, which is out of scope here.
- Molotov and inferno colour is outside this feature. `C_Inferno` has no colour field. Adding one
  requires particle modulation, which is not yet documented separately.
- Not verified inside a render job. Only live playback was tested.
- The `new_setSmokeProps` hook is pattern-based, so it is the one part that can break on an
  update. It calls `ErrorBox` on a failed scan, so a break shows up loudly.

## Related

- [`README.md`](README.md): master scope
- [`update-resilience.md`](update-resilience.md): CS2 update recovery
- [`night-mode.md`](night-mode.md): the other half of the Atmosphere/Lighting look
