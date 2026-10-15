# World / prop / sun-cloud modulation

For current Environment-owned lighting and shadows, use [whole-map relighting](whole-map-relighting.md).
This document covers the native controls and their verification results. Those results do not
replace the current renderer contract or its stated verification limits.

Tints baked map geometry and dynamic scene objects without writing guessed aggregate-object
memory.

- Module: `AfxHookSource2/Filmmaker/World/WorldModulation.{cpp,h}`
- Main-thread pump: `World_RunFrame()` -> `WorldModulation_RunFrame()`
- Render hook: `SceneSystem.cpp` -> `new_DrawBaseSceneObject`
- Command: `mirv_filmmaker world modulate <world|props|sunclouds|effects> <r> <g> <b>`
- Enumeration: `mirv_filmmaker world modulate status` reports live category colors, enabled
  flags and strength as JSON. Console/NetCon transport may split the output across chunks.
- Status: **LIVE-TESTED** on the main menu and repaired Mirage demo

UwU temporarily owns the world, props and sun/cloud tint categories. It restores their original
colors and enabled flags when disabled; effects tint and global strength remain independent.
Environment autosaves, named presets and Undo capture those original values while the effect
is active. Render snapshots continue to use the live violet values. Custom-color restoration,
unrelated edits, active exit/restart and disconnect/reload were verified in CS2.

## Why both paths are necessary

Competitive maps use mostly baked lighting. Scene-light tinting works, but it cannot recolour the
light already baked into static geometry. Most static geometry also draws through
`CAggregateSceneObjectDesc`, which ignores `CBaseSceneData::color`.

So the implementation splits the work by surface:

| surface | mechanism | thread |
|---|---|---|
| static/baked world | material `g_vColorTint` | main |
| model props, foliage, players, sun/cloud layers | material `g_vColorTint` when exposed; guarded per-draw fallback otherwise | main + render |
| transient particle/effect surfaces | `CBaseSceneData::color` around `DrawArray` | render |

Material-backed draws do not also receive the per-draw tint once the exact material has been
published as owned. Streamed instances that have not exposed `g_vColorTint` remain eligible for
the guarded fallback, so a map transition cannot leave a player or prop neutral for one frame.

The Config panel's Atmosphere → World Tint intentionally sends the same colour to `world`, `props`,
and `sunclouds`. Mirage shows why that is necessary: a draw-only tint changes walls, while crates, foliage, and
character gear keep their authored tan/green. The material path reaches those layers too, while
the blue HDR sky remains a separate sun/cloud category. The white wash comes from an HDR light
write, not from bloom: disabling `r_csgo_render_post_bloom` and `r_csgo_effects_bloom` did not
change it at the same paused tick.

Panorama colour-picker bytes are sRGB. Scene-light radiance is linear HDR, while Valve's
`csgo_character.vfx`/world material declarations mark `g_vColorTint` as `SrgbRead(true)`: the
material parameter must be written in sRGB and is converted once by the shader. World modulation
therefore decodes the selected tint, composes it with an authored material tint in linear space,
then encodes the result back to sRGB. Scene-light submission keeps the decoded linear tint. This
matters most for saturated non-primary colours. Magenta `(255, 60, 224)` becomes linear
`(1.000, 0.045, 0.745)` and must not become `(1.000, 0.235, 0.878)`. Writing linear numbers into an
sRGB parameter causes a second conversion, which produces the brown/washed player gear.
ValveResourceFormat's decompiled `complex.vert.slang` and `ctm_sas_body.vmat_c` are the reference
for this boundary.

## Material restore and streaming safety

For each live world material with `g_vColorTint`, the module records:

- the material and parameter identity;
- the authored float4;
- the exact float4 last written by the feature.

When streaming creates or replaces a material object, generated `compmat_*` materials can inherit
a tinted source object's current parameter even under a new name. If that value exactly equals
any live snapshot's last applied float4, the snapshot carries that source's original authored
float4 forward; otherwise the new material is treated as engine-authored and snapshotted normally.
This exact-match rule prevents generated materials from multiplying the tint twice or restoring to
a partially tinted value without guessing from their names.

Reset restores only when the live object and parameter still match and the current value is still
the feature's applied value. A map transition can therefore never write through a stale pointer or
overwrite a newer engine-owned change.

`GetMaterials()` changes every frame during level streaming. Acting on each count change
immediately is unsafe: it crashed inside `materialsystem2.dll` while an entry was still being
constructed. The pump:

1. observes the loaded-material count without touching entries;
2. waits until it is unchanged for 750 ms;
3. applies the tint;
4. rechecks every 2 seconds for same-count replacement or refresh;
5. guards every resource-system/material call with SEH and skips a faulting entry.

Telemetry goes to the `mvm_debug` channel `world.mod.material`. It includes material counts,
applied and restored counts, and guarded faults.

## Live verification

Main-menu regression:

- scene lights in uniform and filtered modes at full amount, including an `amount 8` request;
- whole-scene world + props + sun/cloud materials: red, green, blue;
- process responsive and `mirv_filmmaker health` healthy after every group.
- repeated on the final DLL; artifacts use the
  `final-exact-menu-*` prefix.

Paused Mirage material A/B/A:

- A: neutral map;
- B: 1,152 named material parameters written by the exploratory all-category path; static Mirage
  became deep red;
- A2: all 1,152 restored;
- A/B SSIM `0.749265`; A/A2 SSIM `0.998248`.

Production repaired-demo transition:

- source: `replays/repaired/match730_003828519431596671001_0696917925_410.dem`;
- paused A/B/A screenshots across the repaired Mirage demo on the sRGB material path;
- stable frames retain the tint across map geometry, models/foliage, and sky instead of returning
  to a neutral/pastel state;
- first safe all-category apply after streaming: 781 updates / 1,163 snapshots, `faults=0`;
- three additional consecutive demo reloads kept the same PID responsive;
- exact final-DLL log `mvm_debug_20260804_212312.log`: zero crash markers, zero material-fault
  lines, zero double-tinted `compmat` writes, and 1,160 snapshots restored on reset.

Production paused A/B/A after the reload stress test:

- world-blue A/B mean absolute pixel delta `45.76`;
- world-reset A/A2 mean absolute pixel delta `0.04` (HUD timing accounts for the non-zero residual);
- uniform/filtered light captures show hue changes without white clipping; `amount 8` is
  clamped to `1.00` and the process remains responsive.
- health remained green.

Artifacts are under
`Automation/runs/world-light-regression/20260804-201721/screenshots/`.

## Rejected aggregate-memory path

Read-only probes measured the current aggregate layout as count `0x160`, data `0x168`, stride
`0x68`, with repeated `0xFFFFFFFF` fields at `+0x34/+0x38`. Millions of guarded writes ran without
a direct fault, but any non-white value made the map black. These fields behave more like a
lighting or occlusion term than a diffuse colour.

`g_aggWrite` remains off. The aggregate detour is retained only for bounded diagnostics; production
colour comes from the supported named shader parameter.

Detouring vtable slot 1 on unverified scene descriptor classes is rejected: it uses incompatible
signatures and causes repeatable launch/load crashes.
Only the two proven draw descriptors plus the separately proven aggregate diagnostic hook remain.

## Commands

```text
mirv_filmmaker world modulate world 255 0 0
mirv_filmmaker world modulate props 255 0 0
mirv_filmmaker world modulate strength 0.5
mirv_filmmaker world modulate world off
mirv_filmmaker world modulate reset
```

`world modulate matprobe <n>` is a read-only diagnostic that logs named colour parameters.

## Related

- [`README.md`](README.md): effects status and ownership
- [`sun.md`](sun.md): scene-light and baked-lighting background
- [`update-resilience.md`](update-resilience.md): post-update recovery rules
