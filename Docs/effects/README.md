# World and effects: current contracts and status

The current implementation lives in the [Environment workspace](../features/environment-scene-editor.md)
and the [whole-map relighting contract](whole-map-relighting.md). Those contracts separate implemented
behavior, live evidence and remaining limitations. The numbered ledger below records each
item's method and status.

Read [`update-resilience.md`](update-resilience.md) first if CS2 has just updated.

Environment provides runtime authored-map lighting, sun/local shadows, volumetrics, native
smoke relighting and event-driven muzzle/explosion/fire lights. Multiple fires share four moving
shadow slots fairly; dry clicks do not emit muzzle light. Opaque weapon shadow capture is active.
Transparent particles are not blanket shadow casters, and casing shadows still need visual proof.
See the relighting contract for the latest checks and their limits.

---

## 1. Native-control status at a glance

Status values: **DONE** (verified in-game) · **PARTIAL** (some of it works, see notes) ·
**BLOCKED** (proven not to work by the stated method) · **TODO** (not started).

| # | Item | Method actually used | Risk | Status |
|---|---|---|---|---|
| 1 | Night mode | **`mat_tonemap_force_scale` convar** | Low | **DONE** |
| 2 | Smoke colour / no-smoke | `C_SmokeGrenadeProjectile::m_vSmokeColor` via the existing setter hook | Low | **PARTIAL** |
| 3 | Ragdoll gravity | `ragdoll_gravity_scale` + friends (convars; offline, so no convar hook) | Low | **DONE** |
| 4 | Skybox + clouds UI | Existing `mirv_sky` wired to Panorama + import pipeline + `CloudFx` + live draw rotation | Low | **DONE** |
| 5 | Fog | `C_GradientFog` schema first; shader-param hook as fallback | Medium | **TODO** |
| 6 | World modulation | `g_vColorTint` for world, models/foliage, and sky + `DrawArray` for transient effects | Medium | **DONE** |
| 7 | Sun / cloud colour modulation | Same hook, `"sun"` / `"cloud"` material split | Medium | **PARTIAL** |
| 8 | Particle colour modulation | `particles.dll` render hook | Medium | **TODO** |
| 9 | Weather particles (snow/rain) | `CacheParticleEffect` + control-point origin driving | High | **TODO** |
| 10 | Sun movement / light rotation | Scene-light direction + sprite/SolarPosition fallbacks + independent CSM/static-shadow controls | Highest | **PARTIAL** |

---

## 2. Detail, per item

### 1. Night mode: DONE

Driven entirely by one convar, `mat_tonemap_force_scale`: 0 is off, 0.10 is night, 0.35 is dusk,
1.0 is neutral. It also drives the Time-of-Day preset row.

> **The reference method does not work.** Writing `C_PostProcessingVolume::m_flMinExposure` /
> `m_flMaxExposure` (what every reference port does) **changes nothing**. The writes land and the
> probe shows the volume rewritten every frame, but the image is identical. Mirage has one volume,
> and the spectator camera reports as inside *none* of them, so the tonemapper never consults it.
> `mat_tonemap_force_min/max` do not affect the image.
> Full measurement: [`night-mode.md`](night-mode.md).

Because it is convar-only, this is the most update-resilient item here.

### 2. Smoke colour: PARTIAL

| Piece | Status |
|---|---|
| Colour at spawn | **DONE**: hooks the client's own smoke-property setter |
| Recolour smoke already on screen (`reapply`) | **DOES NOT WORK**: user-tested and parked |
| Keep the colour as it fades (fade-to-grey) | **UNSOLVED**: write disabled |
| Swatches + custom colour picker UI | **DONE** |
| "No smoke" / suppression | **TODO**: not built |

CS2 smoke is a 32³ RGBA **voxel volume** whose colour is baked in at creation. Voxels regenerated
during dissipation come back stock-coloured. Re-tinting the buffer is the intended approach, and the
volume is located correctly at runtime. The write is not yet safe: a committed region is a whole
heap segment, not one allocation, so a `VirtualQuery` bound does not give the size of this one. The write is hard-disabled behind `kVoxelWriteProven = false`. To finish it, find
the buffer's true allocation size by reversing its allocator. Details: [`smoke.md`](smoke.md) §2.1a.

### 3. Ragdoll gravity and physics: DONE

Convar-driven (`ragdoll_gravity_scale`, `ragdoll_friction_scale`, `cl_ragdoll_limit`,
`cl_player_ragdolls_collide`, `ragdoll_lru_min_age`), plus `reset` / `cleanup`. User-tested.

Moon Gravity requires an atomic recovery save before activation. Recovery preserves full float
precision, rejects malformed/out-of-range files and retains invalid evidence. Inspect
`mirv_filmmaker egg status` and `mvm_debug` for recovery/write outcomes. Live failure, cancellation, disconnect and restart checks cover this path. Cheat-state and
crash-recovery work remains open.

> **Open bug:** the RAGDOLLS tab will not switch by console command (Panorama tab hydration).
> Deferred by the user. Clicking it works.

### 4. Skybox and clouds: DONE

| Piece | Status |
|---|---|
| Skybox picker (tile grid, YOURS / CS2 sub-tabs) | **DONE** |
| Import any image or HDRI as a skybox | **DONE**: `.hdr .exr .png .jpg .tga .bmp .pfm` |
| "Create custom skybox" button + file picker, in-game | **DONE** |
| Hot-swap, no map reload | **DONE** |
| Right-click → delete a custom skybox | **DONE**: user-tested |
| Thumbnails, custom **and** stock | **DONE**: 12/14 stock decode; 2 fall back to letter tiles |
| Cloud density / size / drift / brightness | **DONE** |
| Cloud colour + sky tint, with custom colour pickers | **DONE** |
| **Live sky rotation / tilt / mirror** | **DONE**: draw-time shader matrix plus sun-disc vertex override |

> **`mirv_sky` matches the sky entity by both class names.** Matching only `GetClientClassName()=="C_EnvSky"`
> finds zero entities, because the sky entity reports `className "env_sky"` / `clientClassName "C_BaseModelEntity"`.

> **The entity transform is not the render path.** The shipped solution intercepts the live sky draw,
> rotates `sky_pc_50_vs` `g_matSkyWsToOs`, and separately rotates Nuke's high-glow sun-disc draw.
> Full reverse engineering and fixed-camera evidence are in [`skybox.md`](skybox.md) §2.

Everything else is in [`skybox.md`](skybox.md).

### 5. Fog: TODO

> **Check the map's entities first:** Mirage has **no `C_GradientFog` entity**. It
> uses `env_cubemap_fog`. So "schema first" needs `C_EnvCubemapFog` as the primary target, with
> `C_GradientFog` as the secondary for maps that have one. Check which entities a map has before
> assuming.

### 6. World modulation: DONE

Static and baked geometry, props, foliage and character materials use the loaded material's named
`g_vColorTint` (the parameter is written in sRGB as Valve's shader contract requires). Materials
that are not yet published by the streaming scan, plus transient effects, use the guarded
`CBaseSceneObjectDesc::DrawArray` fallback. The material pump waits for the streamed-material
count to settle before touching entries, restores authored values by live object identity, and
reasserts after map loads. Verified with menu RGB, paused repaired-demo A/B/A restoration, and
scene-light uniform/filtered captures. See
[`world-modulation.md`](world-modulation.md).

### 7. Sun, cloud and scene-light colour modulation: DONE

Cloud colour is DONE and does not use the draw hook. Cloud materials use `csgo_effects.vfx`
and expose `g_vColorTint`, so the `mirv_sky clouds color` command writes it directly. It tints
whatever materials the current map loaded whose name contains
`"clouds"`, with no per-map table.

Scene-light colour is DONE. `world light` detours the current build's light-submit function,
temporarily applies a bounded linear tint, and restores the authored HDR values around the engine
call. Uniform mode preserves the source light's peak; filtered mode multiplies channels. The
amount is clamped to `0..1`, so oversized requests such as `8` cannot produce a blown-out glow.
The live repaired-demo test shows both modes changing the scene without highlight clipping and
surviving reset.

### 8–9. Particle colour and weather particles: TODO

Not started. Sun movement/light rotation and the baked-shadow switch are complete as
item 10 above; see [`sun.md`](sun.md) §3.5 for the reverse-engineered offsets, controls, and
fixed-tick visual proof.

---

## 3. Measured dead ends: do not re-attempt without reading these

Each of these looks correct in memory.

| What | Why it fails |
|---|---|
| `C_PostProcessingVolume` exposure for night mode | Writes land; the tonemapper never consults the volume |
| `mat_tonemap_force_min` / `_max` | Not read by the renderer on this build |
| Per-frame `m_vSmokeColor` write to recolour deployed smoke | Colour is baked into the voxel volume at creation |
| Voxel re-tint with a `VirtualQuery` size bound | A committed region is not one allocation. Causes CS2 crashes |
| `env_sky` `m_angRotation` / `m_nodeToWorld` for sky rotation | Writes persist; the renderer doesn't use that transform |
| Matching entities by `GetClientClassName()` alone | Most report a generic base class. Match `GetClassName()` too |
| A 2D texture for `g_tSkyTexture` | Needs a cubemap; you get the magenta error texture |
| DXT1 for a Panorama UI texture | Renders washed to near-white. Valve uses `RGBA8888` + `NO_LOD` |
| sRGB output colour space as a "tone map" for HDR | It's a transfer curve; HDR clips to white. Tone-map first |
| Aggregate instance `+0x34/+0x38` as diffuse tint | Stable writes, but any non-white value makes the map black |
| Unverified descriptor vtable-slot-1 detours | Incompatible signatures; repeatable launch/load crashes |

## 4. The verification standard

A feature is not done until, **at a fixed camera and a paused tick**:

1. It builds clean.
2. `mirv_filmmaker world health` reports its fields resolved.
3. The visible effect is confirmed by screenshot, toggled **on and off**, with off restoring the
   original look.
4. Its UI round-trips: console → panel and panel → console agree.

**Run an A/B/A control.** Capture the off state, the on state, then the off state again, and
require the two off frames to be *identical* before believing the middle one. A single
before/after pair can produce a false positive: a frame taken while a texture was still
streaming in looked exactly like a working effect. Compare numerically on a cropped region, not by
eye.

## 5. Out of scope

- **Smoke voxel occlusion**: wallhack machinery.
- **Anything online.** Offline demo/movie tooling only.
- **`ragdoll_gravity_scale` convar-getter hooking**: that thread avoids VAC detection. Offline, we
  just set the convar.

## Related

- [`skybox.md`](skybox.md) · [`night-mode.md`](night-mode.md) · [`smoke.md`](smoke.md) ·
  [`ragdoll-physics.md`](ragdoll-physics.md) · [`sun.md`](sun.md) ·
  [`world-modulation.md`](world-modulation.md) · [`shadow-separation.md`](shadow-separation.md)
- [`whole-map-relighting.md`](whole-map-relighting.md): relighting a map from its own authored
  lights through CS2's forward material shaders (console-only, direct light only)
- [`lighting-owned-proof.md`](lighting-owned-proof.md) ·
  [`lighting-replacement-evidence.md`](lighting-replacement-evidence.md): the single-fixture
  experiment and why it was rejected
- [`sky-map-data.md`](sky-map-data.md): per-map sky/lighting data extraction
- [`update-resilience.md`](update-resilience.md): CS2 update recovery for these features
- [Repository build and diagnosis guidance](../../CLAUDE.md)
