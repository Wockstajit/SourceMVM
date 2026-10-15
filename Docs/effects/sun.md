# Sun

For current Environment-owned lighting and shadows, use [whole-map relighting](whole-map-relighting.md).
This document describes the native sun controls and the measured results behind them. Those results do not
replace the current renderer contract or its stated verification limits.

Sun size, brightness, glow ring, and live scene-light rotation.

- Module: `AfxHookSource2/Filmmaker/World/SunFx.{cpp,h}`
- Command: `mirv_filmmaker world sun ...`
- Status: glow ring done; scene-light rotation done; visible sky-disc fallbacks built; split shadow controls verified (CS2 2000879)

---

## 1. What works: the glow ring

Verified in-game. The A/B/A control passed.

The bright halo around the sun is a separate material,
`materials/effects/glows/sun_disc_glow_001`, drawn with `csgo_effects.vfx`, the same shader as
the clouds. Its `g_flColorBoost` ships at 129.93, which is why the halo blows out so hard.

```
mirv_filmmaker world sun glow 0.05    # halo essentially gone, clean disc remains
mirv_filmmaker world sun glow 2.5     # hotter
```

Measured on a crop around the sun, demo paused:

| comparison | mean pixel diff |
|---|---|
| glow 1.0 vs glow 1.0 (control) | 0.000 |
| glow 1.0 vs glow 0.05 | 41.5 |
| glow 1.0 vs glow 2.5 | 36.7 |

Written through `SceneSystem_FindMaterialParam`, the same material-var mechanism already proven by
the cloud tint. It multiplies the map's own value, so 1.00 leaves it untouched everywhere.

## 2. What does not work: SolarPosition, SolarRadius and SolarIrradiance

Shipped sky materials carry these, and they decode cleanly:

```
sky_de_nuke.vmat_c   SolarRadius     0.51                                 <- degrees; the real sun's angular size
                     SolarIrradiance [2.107187, 2.123156, 1.794319, 0]    <- RGB, slightly warm
                     SolarPosition   [0.422618, 0, 0.906308, 0]           <- UNIT vector; elevation 65, azimuth 0
```

`SolarPosition` norms to exactly 1.0, and `0.4226 = sin(25°)`, `0.9063 = cos(25°)`. It is
clearly a sun direction.

There are two separate problems.

a. Most sky materials do not expose them. A material only exposes the parameters it actually
*sets*. Probed at runtime (`world sun probe`, channel `world.sun`):

```
sky "materials/skybox/sky_de_mirage.vmat"          pos=no  radius=no  irradiance=no
sky "materials/skybox/sky_de_dust2.vmat"           pos=no  radius=no  irradiance=no
sky "materials/skybox/sky_cs15_daylight01_hdr.vmat" pos=no radius=no  irradiance=no
sky "materials/skybox/filmmaker/sunsky.vmat"       pos=YES radius=YES irradiance=YES
```

The last line is one of ours: `build-skybox.ps1` emits all three into every generated
skybox, which is what makes them resolvable at all.

b. Even when they resolve, writing them changes nothing on screen. Measured on our own
skybox with all three confirmed resolving:

| comparison | mean pixel diff |
|---|---|
| base vs base (control) | 0.000 |
| base vs azimuth 120 | 0.000 |
| azimuth 120 vs azimuth 240 | 0.000 |
| azimuth 120 vs size 8x | 0.000 |

Conclusion: these parameters are metadata, not a drawn object. They describe where the sun is
for lighting/atmospheric purposes; they do not composite the visible disc. On the stock map the
disc is a separate high-glow `csgo_effects.vfx` draw; the draw-time path described in
[`skybox.md`](skybox.md) §2 moves that geometry directly.

The generated-material `Solar*` block is kept: it costs nothing, it is correct metadata, and it is
the only way those parameters could ever become drivable.

## 3. Moving the sun: lighting and shadow direction

Moving the sun for real means rotating the scene light, not touching the sky. The current
build exposes the light object through the scene-submit hook, so the implementation records the
authored direction at `+0x184`, applies a rigid yaw/pitch rotation for the engine call, and restores
the authored value when the control is reset. The engine's hidden `lb_allow_shadow_rotation` CVar
is also enabled through the CVar registry (including hidden entries), so maps that enter the
shadow-update path can use the engine's own invalidation logic.

Yaw is azimuth and pitch is elevation, relative to the map's own sun
(`SunRotationMath::SunRotation`). Yaw circles the sun about world up at the same height.
Pitch raises (+) or lowers (−) it about the horizontal axis across the sun's own azimuth, so its
compass direction does not change. The resulting elevation is held inside ±89° so the sun never flips
over the zenith.`SunFx` publishes the authored sun direction as the reference (`LightFx_SetSunReference`). Until it
does, the scene light's own direction stands in. That field points toward the sun: measured on Inferno at +63°, so the rotation depends on that orientation. Each level clears the reference. The sun
disc, the sky sprite's vertices and the light direction all use this one rigid rotation.

The draw-time sun path follows this same physical direction, so the visible disc and live shadows
move together. Sky yaw/pitch can add an artistic cubemap offset without changing the authored
scene-light direction or moving the separate sun decal.

### 3.1 Published signatures do not match this build

All of them, from both the light-rotation and the lighting-modulation threads, run against
`game/bin/win64/scenesystem.dll` with `Misc/sigscan.py`:

| Pattern | From | Matches |
|---|---|---|
| `48 89 54 24 ? 55 56 41 57`: UpdateLightShadowMap | 734508 post 1 | 0 (dead) |
| `48 89 54 24 ? 53 55 41 57`: lighting modulation | 653274 (ogpolak) | 0 (dead) |
| `48 89 54 24 ? 53 41 56 41 57`: lighting, "updated" | 653274 / 658539 | 0 (dead) |
| `E8 ? ? ? ? 4C 8B 8F`: the pre-obfuscation one | 653274 | 5 (ambiguous) |
| `48 89 5C 24 ? 48 89 6C 24 ? 48 89 54 24` | 734508 post 4 | 2 (ambiguous) |
| `48 89 54 24 10 53 55 41`: lights modulation | 658539 | 1 (candidate, unverified) |

Not one usable hit.

The published offset dumps do not rescue this either. `Misc/cs2-offsets/` has a
`scenesystem_dll` dump, but it contains only the SSDS *message* schema classes
(`CSSDSMsg_ViewRender` and friends). `CAggregateSceneObject`, `CSceneLightObject` and
`CLightBinnerGpu` are internal engine classes, not networked schema types, so no schema dump will
ever carry them.

### 3.2 The anchors that are alive: use these instead

```
"lb_enable_lights"           1 hit  @ 0x5ede08   <- xref for the lighting-modulation function
"lb_allow_shadow_rotation"   1 hit  @ 0x5ee118   <- xref for the shadow/light-rotation function
"CAggregateSceneObject"      3 hits              <- RTTI present
"CSceneLightObject"          4 hits              <- RTTI present
"CLightBinnerGpu"            0 hits
```

Both convar strings survive, so both functions survive. Only their byte patterns moved. And two
of the three classes carry RTTI.

So resolve by RTTI and convar xref, not by AOB. The repo already has
`FindClassVtable()` in `Shared/binutils.h` for exactly this. It is the only approach that
currently works, and it is more update-resilient than anything published in those threads,
That is the same reason this feature set resolves schema fields by name in the first place
(see [`update-resilience.md`](update-resilience.md)).

### 3.3 Solved by reverse engineering: the light function on this build

Rather than hunt for a new AOB, the function was recovered from the convar anchor. Reproducible
with `pefile` + `capstone`; the whole chain is four steps.

1. The convar name string and its one code xref.

```
"lb_enable_lights"  @ VA 0x1805ef208
  0x18000f5b0: lea rdx, [rip + 0x5dfc51]   ; the name string
  0x18000f5b7: lea rcx, [rip + 0x905c22]   ; the ConVar object -> 0x1809151E0
  0x18000f5be: call 0x1804b0460            ; registration
```

2. Who reads that ConVar object. ConVar objects here are 0x10 bytes, so a wider search window
picks up neighbouring convars and looks like field offsets. It isn't. The real reads are at `+0x8`.

3. The enclosing function.

```
0x180199590  mov  qword ptr [rsp+0x10], rdx     ; arg2 saved -- the scene object
             push rbp / push rdi / push r14
             sub  rsp, 0x50
             mov  rdi, rdx                      ; arg2
             mov  rbp, rcx                      ; arg1
             lea  rcx, [rip+0x77bc31]           ; lb_enable_lights
             call 0x1804b0380
```

4. The colour offsets, confirmed by disassembly rather than by trusting the leak.

```
0x180199633  lea   rsi, [rdi + 0xe0]        ; rdi = arg2
0x18019963a  movss xmm0, [rsi + 4]          ; +0xE4
0x180199647  ucomiss xmm0, xmm6             ; == 0.0 ?
0x18019964e  movss xmm0, [rsi + 8]          ; +0xE8
0x18019965a  movss xmm0, [rsi + 0xc]        ; +0xEC
0x180199664  je    0x1801997f8              ; all three zero -> skip this light
```

Three consecutive floats at +0xE4, +0xE8 and +0xEC, culled when all are zero. That is exactly
the `float red, green, blue` both threads claim, independently verified here.

Result:

| | |
|---|---|
| Module | `scenesystem.dll` |
| VA (this build) | `0x180199590`, RVA `0x199590` |
| Signature (unique, 1 match) | `48 89 54 24 ? 55 57 41 56 48 83 EC ? 48 8B FA 48 8B E9 BA FF FF FF FF` |
| Args | `(rcx = a1, rdx = scene light object, r8 = a3)` |
| Colour | `float r,g,b` at `+0xE4 / +0xE8 / +0xEC` |
| Side effect | writing `0,0,0` hits the engine's own cull, so that light is skipped entirely |

Re-derive, do not trust, after a CS2 update. The signature above is from build 14174 and will
rot. The convar-anchor chain in steps 1 to 3 is the durable part and takes about a minute to re-run.

Implemented in `World/LightFx.cpp`. The detour is attached from `HookSceneSystem`
(SceneSystem.cpp), non-fatally. A dead pattern disables this one feature instead of taking the
draw hooks down. It multiplies the engine's colours rather than assigning an absolute one, so the
map keeps the relative lighting its artist authored and 1.0 needs no snapshot to restore.

#### Status: hook proven and visually verified

What is confirmed:

- The pattern resolves, the detour attaches, and the function fires constantly.
  `world light` reports 17782 light submissions seen in a couple of minutes of playback.
- The writes execute on every one of those. They are bounded by the source light's HDR peak and
  are restored immediately after the engine call, so the hook cannot compound radiance frame over
  frame.

Measured on the repaired Mirage demo at a paused tick:

| comparison | mean pixel diff |
|---|---|
| base vs base (control) | 0.000 |
| uniform tint `40 100 255` | visibly blue, no clipping |
| filtered tint `255 20 180` | visibly magenta, no clipping |
| amount `8` | reports 1.00 and remains bounded |

The implementation separates display/sRGB picker
values from linear radiance, preserves the source peak in uniform mode, and clamps the amount
before any submit-thread write. The process stayed alive through tint, mode, clamp, and reset.

The same tint state is also applied to the visible sun, not only to the submitted light:
`SolarIrradiance` colours the parameterized disc, and `g_vColorTint` colours the separate glow
material. Color only replaces hue while preserving authored peak brightness. Filter / dim
multiplies authored RGB and also scales glow boost/opacity, so dark colours and black can dim the
disc and halo instead of leaving a bright white sun behind.

### 3.5 Sun and shadow rotation (implemented on the current build)

The shipped path does not write the orthogonal partner at `+0x190`, does not save/restore across a
shadow call, and never mutates a short-lived record. It rotates only the validated normalized direction at `+0x184`, keyed by the persistent
scene-light pointer, with a per-object authored snapshot so repeated frames cannot compound the
angle. All raw reads/writes are SEH guarded.

Controls are in Effects > Lighting > Sun:

```
mirv_filmmaker world light yaw <0..360>
mirv_filmmaker world light pitch <-90..90>
mirv_filmmaker world light reset
```

The Panorama state reports `sunYaw`, `sunPitch`, `lightRotationOk`, and live write counters. On
the Nuke repair demo, a fixed camera showed the disc move horizontally for yaw −15° and downward for pitch −20°; a 180° yaw sweep moved the live fence shadow across the spawn ground. The process
remained responsive through the full sweep; no access-violation/crash record was emitted.

`mirv_filmmaker world bakedshadows` drives only `lb_enable_baked_shadows`
(Baked OFF => cvar 0). `mirv_filmmaker world dynamicshadows` drives only
`csm_max_num_cascades_override` (Live OFF => 0).

### Proven Mirage A/B (CS2 ClientVersion 2000879)

Locked protocol on the repaired Mirage match demo:

```
demo_gototick 2936
demo_pause
mirv_input camera
mirv_input position -1561.1 -2215.5 -164.3
mirv_input angles 10 -70 0
mirv_input fov 90
mirv_filmmaker world light yaw 140
mirv_filmmaker world light pitch 30
mirv_filmmaker mouse free
mvm_debug start
```

Regional pixel diffs vs both-ON baseline (crops in image pixels on 1600×1200 captures):

| state | stairs crop `650,650,950,950` | ground-palm `1000,750,1400,1050` |
|---|---|---|
| baked OFF / live ON | mean dL ~+40 | mean dL ~+50 |
| baked ON / live OFF | mean ~1–4 | mean ~0–3 |
| both OFF | matches baked OFF on these crops | matches baked OFF |
| restored both ON | ~0 | ~0 |

So the large stairs/ground plant-frond contribution is owned by `lb_enable_baked_shadows`.
Live cascade override barely moves those crops at this pose; fence/static-geo CSM is not
stripped because Baked does not touch `lb_csm_override_staticgeo_*`.

Standalone in-game verifier (does not share build/authoring with other lighting checks):

```
powershell -ExecutionPolicy Bypass -File Automation/verify/verify-shadow-separation.ps1
```

SST→CSM copy-suppress candidate (research; keep product baked gate until it passes):

```
powershell -ExecutionPolicy Bypass -File Automation/verify/verify-sst-csm-boundary.ps1
mirv_filmmaker world shadowstage suppress on|off
mirv_filmmaker world light bakedindex force on|off
```

Focused source assertions: `Automation/tests/shadow-separation-tests.mjs`.

### Rejected controls (same protocol)

| control | result |
|---|---|
| `sc_disable_baked_lighting` | no-op on plant crops (decal/lightmap path; rejected for brightening decals) |
| `lb_precomputed_shadowmap_enable` alone | no-op with Live CSM on |
| `sparseshadowtree_enable_rendering` / `cascade_mask` alone | no-op with Live CSM on |
| `lb_csm_override_staticgeo_cascades(_value)` | strips live static-geo CSM (tree/fence); must not be Baked |
| `r_shadows` | master dynamic gate; useful diagnostic, not the product Baked toggle |
| `r_directional_lightmaps` / `r_csgo_directional_lightmaps` | small lighting tint, not plant-frond removal |
| entity `m_nSceneObjectOverrideFlags` (`DisableShadows_t` 0–4) on nearby props / `worldent` | writes succeed (`offset=0x3BC`); no regional pixel change on the plant |

Upper-wall soft residual at sun yaw 140 barely moves when Baked is OFF (~0.03 mean in
`700,320,900,600`); large sun yaw changes still re-light that band. Treat the stairs/ground
frond crops as the authoritative baked-plant metric for this corner.

The binary-level account of *why* each of these controls behaves as it does. The two render-path
readers of `lb_enable_baked_shadows`, the per-light baked shadow index at scene light object
`+0x154`, and the per-object `baked_shadow_index` attribute are covered in
[`shadow-separation.md`](shadow-separation.md).

Native finer-seam candidates (SST→CSM copy suppress; per-light `+0x154` force) were A/B’d on the
locked Mirage protocol and rejected. Product Lighting control is one Shadows master
toggle (`mirv_filmmaker world shadows`) that turns off baked + live CSM together. Real custom
directional shadow maps are active work. See
[`shadow-separation.md`](shadow-separation.md).

The Sun yaw/pitch, sky yaw/pitch/mirror, Shadows master, Shader shadows toggle and tunables,
tint, amount, mode, and Lighting-tab exposure are persisted by the Filmmaker Config settings store
(`%APPDATA%\\HLAE\\filmmaker_settings.json`). They are restored
on the next startup and re-applied when the scene-light hook resolves after a map loads.

`world light probe` scans a live light object for unit-length float triples. The direction field is
`+0x184`, the light data block starts at `0xE0`, and colour sits at `0xE4`.

Sun glow ring, light colour and light intensity are unaffected by the rotation path.

### 3.4 Corroborating offset sources

Two independent threads describe the same struct under different names, both putting float
`r/g/b` at 0xE4:

- 658539: `class C_SceneLightObject { char pad[0xE4]; float r, g, b; }`
- 653274: `class CAggregateSceneObject { unsigned char pad_0[0xE4]; float red, green, blue; }`

Same object, two names, agreeing on the offset. Worth testing first once the object can be
reached, though note 653274's own follow-up reports "when changing color/intensity it doesn't
actually do anything", so treat even that as unconfirmed.

The struct paddings in the reference (`CLightBinnerGpu` PAD(57968), `CSceneLightObject` PAD(224))
are build-specific and must be re-derived, not copied.

## 4. On the modulation threads generally

`658539-modulation-world-props-lights-sun.md` gives colour modulation for world, props, lights
and sun/clouds via scene-system hooks. Useful for master-scope items 6 and 7, not for moving
the sun.

Two things about it worth remembering:

- Its `CBaseSceneData` colour offset is 0x50. Our `SceneSystem.cpp` struct uses 0x50 too.
- Its own author says the aggregate-object approach "isn't the best way of doing world
  modulation... You should rather look into material vars." That is what this module and
  `CloudFx` already do. Both are verified working, while the hook-based approaches in that
  thread are reported to crash on clouds.

## 5. Commands

```
mirv_filmmaker world sun glow <x>        # THE HALO RING - works
mirv_filmmaker world sun size <x>        # SolarRadius     - resolves on our skies, no visual effect
mirv_filmmaker world sun brightness <x>  # SolarIrradiance - ditto
mirv_filmmaker world sun azimuth <deg>   # SolarPosition   - ditto
mirv_filmmaker world sun elevation <deg> # SolarPosition   - ditto
mirv_filmmaker world sun probe           # which parameters this map actually exposes
mirv_filmmaker world sun apply | reset
```

`glow` and `glow_size` are wired to the UI. `glow` changes halo brightness; `glow_size` scales the
disc pair and any companion glow around one shared disc midpoint, so the pieces cannot drift
apart when the sun moves. Mirage's visible disc is two separate high-boost triangle draws; scaling
each about its own centroid made bloom size read as "two triangles". The Solar* commands remain
console instrumentation; they are how the §2 measurement is
reproduced, but must not be surfaced as controls while they do nothing.

The D3D path must rigidly rotate every sun vertex around the camera, and must re-read the live
vertex buffer every draw: the engine rebillboards these sprites toward the current view each
frame, so a frozen snapshot goes edge-on when the player looks around. Rotating only the centre
and translating the original plane also leaves its normal at the authored direction; after a large
yaw or pitch the plane becomes edge-on/back-facing and the disc appears to phase out. An
`mvm_debug` sweep observed the high-boost disc draw in 35,361 unique rendered frames with no
multi-frame submission gap, ruling out engine view-frustum culling for that symptom. Companion
glow pairing uses distance-to-shared-midpoint rather than AABB containment of a single triangle
centroid, because the two disc centroids sit outside the glow AABB while their midpoint does not.
Scene-system material-name scopes cannot be used here because scene submission and
D3D drawing occur on different queued threads.

Hiding the sun zeros only its visible irradiance and glow opacity/boost. It deliberately keeps
the authored `SolarRadius` valid so the sky shader does not take the camera-dependent cloud/zenith
branch that makes clouds disappear when looking straight up. The `mirv_sky sun draw` command sets
this same visibility state and does not cull the scene draw.

## Related

- [`README.md`](README.md): master status
- [`shadow-separation.md`](shadow-separation.md): baked vs live shadow RE report and candidate controls
- [`skybox.md`](skybox.md) §2: the draw-time sky and sun-disc rotation path