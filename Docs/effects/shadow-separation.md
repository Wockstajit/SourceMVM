# Baked vs live shadow separation: reverse-engineering report

For current Environment-owned lighting and shadows, use [whole-map relighting](whole-map-relighting.md).
The native controls and their investigation results are below. Those results do not replace the
current renderer contract or its stated verification limits.

Goal: remove map-authored baked shadows while keeping live CSM.

Product: Lighting uses one Shadows master toggle
(`mirv_filmmaker world shadows`) that drives baked + live CSM together. Shader shadows
(`world customshadow`) are a separate HLSL contact pass. A real custom directional shadow map
is the active implementation path. The native resource probe is research-only until
the runtime shadow-map boundary is proven.

- Modules: `ShadowFx.cpp` (product master), `BakedShadowFx.cpp` / `DynamicShadowFx.cpp`
  (cvar implementors + aliases), `CustomShadowFx.cpp` + `ContactShadowPass.cpp`,
  `ShadowStageProbe.cpp` (research)
- Live protocol: [`sun.md`](sun.md) §3.5
- Status: native SST/+0x154 seams are not product gates. Product gate is unified engine Shadows;
  contact HLSL is opt-in for look tests.

## 0. Provenance

| | |
|---|---|
| Module | `game/bin/win64/scenesystem.dll`, image base `0x180000000` |
| Installed game | ClientVersion 2000880, VersionDate Aug 03 2026 |
| Protocol reference | ClientVersion 2000879 |

`scenesystem.dll` is dated 2026-07-29 and applies to both 2000879 and 2000880. The
addresses below apply to the captured evidence. Re-derive after any CS2 update. The
convar name strings are the durable anchor. The VAs are not.

Reproduce everything here with:

```
python Automation/tools/re_lightbinner_shadows.py --inventory
python Automation/tools/re_lightbinner_shadows.py --xref --all --json out.json
python Automation/tools/re_lightbinner_shadows.py --disasm 0x18019bcca --before 138 --after 190 --annotate
python Automation/tools/re_lightbinner_shadows.py --refs baked_shadow_index
```

A linear capstone sweep is useless on this binary. It desynchronises on the first jump table and
silently stops after ~36k instructions. The tool instead scans every byte offset for a `disp32` that would resolve to a
wanted VA and confirms each candidate by decoding backwards. ConVar globals are 0x10 bytes
(`+0x0` object, `+0x8` data pointer), so anything found at `+0x10` belongs to the *next* convar.

## 1. Who actually reads `lb_enable_baked_shadows`

String `0x1805ef638` maps to ConVar object `0x180915320`. Excluding the registration thunk and the console
dump routine, there are exactly two render-path readers:

| function | read site |
|---|---|
| `0x18019a0d0` | `0x18019aefe` |
| `0x18019b8e0` | `0x18019bcca` |

Both branches do the same thing: force a baked-shadow-index byte to `0xFF` in the shadow
descriptor being built.

```
; fn 0x18019a0d0
0x18019af1a  cmp   byte ptr [rax], bl          ; lb_enable_baked_shadows == 0 ?
0x18019af1c  je    0x18019af41                 ; -> disabled path
0x18019af1e  cmp   byte ptr [r12 + 0xe539], bl ; a SECOND, non-convar gate
0x18019af26  je    0x18019af41                 ; -> same disabled path
             ; enabled: keep authored index + two floats
0x18019af28  movzx eax,  byte ptr [rbp + 0x17a]
0x18019af2f  movss xmm1, dword ptr [rbp + 0x174]
0x18019af37  movss xmm0, dword ptr [rbp + 0x170]
             ; disabled:
0x18019af41  mov   al, 0xff                    ; index = 0xFF (none)
0x18019af43  movaps xmm1, xmm9                 ; = 0.0
0x18019af47  movaps xmm0, xmm9                 ; = 0.0
0x18019af5b  mov   byte ptr [rbp + 0x17a], al
```

So the cvar does not toggle rendering. It is an index invalidator: `0xFF` means "this light
has no baked shadowmap slice", and everything downstream falls out of that.

`[r12 + 0xE539]` and `[r13 + 0xF331]` (same role in `0x18019b8e0`) are per-binner-state bytes that
produce the identical result without touching the cvar. They are candidate hook points but are not
*finer* than the cvar.

## 2. The light struct, which is the one `LightFx` already hooks

`0x18019b8e0` opens with:

```
0x18019b904  lea r15, [rdx + 0xe0]             ; rdx = arg2 = scene light object
0x18019b90b  cmp byte ptr [r15 + 0x74], 0xff
```

`rdx + 0xE0` is the same data block `LightFx.cpp` already writes. Colour is at `+0xE4/+0xE8/+0xEC`
(= `r15+0x4/+0x8/+0xC`) and direction at `+0x184` (= `r15+0xA4`). Same object, same argument
position. Mapped so far:

| scene light object | `r15` | meaning |
|---|---|---|
| `+0xE4 / +0xE8 / +0xEC` | `+0x04/+0x08/+0x0C` | float r, g, b (already used) |
| `+0x11D` | `+0x3D` | byte flag, tested `== 1` |
| `+0x154` | `+0x74` | baked shadow index, `0xFF` = none |
| `+0x157` | `+0x77` | light type enum (branches on 0 / 1 / 2 / 3) |
| `+0x170` | `+0x90` | float, copied into descriptor `+0x264` |
| `+0x184` | `+0xA4` | normalized direction (already used) |

The engine defines a stationary or mixed light as one that has a baked shadow index:

```
0x18019b90b  cmp byte ptr [r15 + 0x74], 0xff
0x18019b924  je  0x18019b957                   ; == 0xFF -> not stationary
             ; != 0xFF -> requires lb_enable_stationary_lights, else exit at 0x18019cbf8
             ; == 0xFF -> requires lb_enable_dynamic_lights,   else exit at 0x18019cbf8
```

## 3. The per-object seam

Immediately before the cvar test in `0x18019b8e0`, the engine tries to derive a baked shadow index
from render attributes on a scene object:

```
0x18019bc70  call  0x1800850d0                 ; lazily build [rsi+0xa8]
0x18019bc80  lea   rbx, [rax + 0x110]          ; attribute table
0x18019bc75  mov   edx, 0x3d81da66             ; token A
0x18019bc8a  call  0x18004b810                 ; GetAttribute(table, token, -1)
0x18019bc8f  mov   edx, 0x69a26332             ; token B
0x18019bca4  call  0x18004b810
0x18019bca9  cmp   word ptr [rbp + 0x29c], r12w ; A invalid -> skip
0x18019bcb3  cmp   eax, -1                      ; B invalid -> skip
0x18019bcb8  mov   byte ptr [rbp + 0x28a], al   ; baked shadow index = B
0x18019bcbe  mov   byte ptr [rbp + 0x29a], 3    ; shadow kind = 3
```

`0x18004b810` is an attribute lookup: it broadcasts the 32-bit key with `pshufd`, `pcmpeqd`s it
against arrays of four dwords, walks a linked list via `[rax]` and a parent chain via `[r9+0x60]`.

The matching writer is `0x18008c956`, and it keeps debug names:

```
0x18008c991  mov dword ptr [rbp + 0xb0], 0x4a2a06bf
0x18008c99b  lea rax, [rip + 0x54fca6]         ; "baked_shadow_index"
0x18008c9a2  mov dword ptr [rbp + 0xb4], 0xffffffff
0x18008c9c7  mov dword ptr [rbp + 0xe0], 0x4ae53a21
0x18008c9d1  lea rax, [rip + 0x54fc28]         ; "baked_shadow_light_id"
0x18008ca18  mov byte  ptr [rbp + 0x77c], 0xff ; default = none
0x18008ca26  mov byte  ptr [rbp + 0x798], 0xff
```

### Hash function, confirmed

Source 2 string tokens here are MurmurHash2, lowercased, seed `0x31415926`. Verified against
the binary, not assumed:

| string | token |
|---|---|
| `baked_shadow_index` | `0x4A2A06BF` |
| `baked_shadow_light_id` | `0x4AE53A21` |
| `path_trace_light_flags` | `0xAD1DA24A` (cross-checked in `resourcecompiler.dll`) |

The two tokens used by the *light* path, `0x3D81DA66` and `0x69A26332`, do not match any string
in any of the 110 shipped CS2 DLLs (1.67M strings scanned) nor a 960-entry targeted dictionary. The
reader passes bare tokens with no debug name while the writer passes a descriptor containing one, so
these are map-authored attribute names that exist only in asset data. Their *semantics* are certain
from the code even though their spelling is not.

## 4. Gate ownership: which branch is baked, mixed, or live

| gate | render-path readers | what it really controls |
|---|---|---|
| `lb_enable_baked_shadows` | `0x18019a0d0`, `0x18019b8e0` | invalidates the per-light baked shadow index |
| `lb_enable_stationary_lights` | `0x18019a0d0`, `0x18019b8e0` | early-exits any light that has a baked index |
| `lb_enable_dynamic_lights` | `0x18019a0d0`, `0x18019b8e0` | early-exits any light without a baked index |
| `lb_precomputed_shadowmap_enable` | `0x18019b8e0`, `0x18019cc20` | precomputed shadowmap sampling |
| `lb_mixed_shadows` | `0x1801a1a00` only | shadowmap *render dispatch*, shares the function with `r_shadows` |
| `r_shadows` | `0x1801a1a00` | same dispatch; this is why it reads as a master gate |
| `lb_shadow_map_cull_empty_mixed` | `0x1801a1a00` region | "Don't render shadows for mixed shadowmaps with no dynamics objects in view" |
| `lb_csm_override_staticgeo_cascades(_value)` | `0x1801a04f1` | which cascades render static objects |
| `lb_csm_override_staticgeo_cascades_animated_verts` | `0x1801a0b09` | narrows the exclusion set (see below) |
| `sc_disable_baked_lighting` | 6 readers, all in aggregate/mesh draw paths | material lightmap gate, not a shadow gate |

That last row is the mechanical reason `sc_disable_baked_lighting` brightens decals: its readers are
`0x180040f40` (aggregate draw), `0x180052b10`, `0x18006e3b0`, `0x180072720`, `0x180078e30` (mesh
draw), `0x1800e7890`. It never touches the light binner.

`lb_mixed_shadows` having a single reader shared with `r_shadows` explains why both behave as
coarse master gates rather than a baked/live seam.

### The static-geo override, in full

The truncated help string reads in full:

> If `lb_csm_override_staticgeo_cascades`, ensure only objects without animated verts, i.e.
> `SCENEOBJECTFLAG_CAN_RENDER_INTO_SST` flag will be excluded (as opposed to all static objects).

So `_animated_verts = 1` narrows the exclusion to rigid SST-capable geometry. It excludes more
of the fence/tree class, not less. It does not help and should not be re-swept.

## 5. Shadow classes and the SST

Diagnostic strings confirm three shadowmap classes and a "baked only" flag:

```
%i shadowmaps [%i dynamic, %i mixed, %i fog-only]
   SST: EMPTY (no static geo rendered into any tiles, must be flagged as baked only)
CopySSTDepthToCSM_PS / CopySSTDepthToCSM_CopyTex
```

Static occluders decompress out of the Sparse Shadow Tree and are copied into the CSM atlas;
dynamic geometry rasterises into the same atlas afterwards. Setting
`sparseshadowtree_enable_rendering 0` does not remove static occluders. It only makes them take the
conventional rasterisation path into the cascades.

## 6. `DisableShadows_t` enumerator mapping

Resolved from the schema enumerator table in `.rdata` (name pointer followed by the value), not from
the string order, which is unordered:

| enumerator | value |
|---|---|
| `kDisableShadows_None` | 0 |
| `kDisableShadows_All` | 1 |
| `kDisableShadows_Baked` | 2 |
| `kDisableShadows_Realtime` | 3 |
| `kDisableShadows_ReallyNone` | 4 |

Type record: 1-byte enum, 5 enumerators. `SchemaSystem.h`'s comment (`2 = baked, 3 = realtime`) and
`BakedShadowFx.cpp`'s `entity` mode mapping match this table. The enumerator values are therefore
not the reason nearby props show no change. This is consistent with the plant
shadow living in the aggregate/world scene object rather than in any entity.

## 7. Tool-time model (Hammer / resourcecompiler)

Authoring keys present in the binaries:

```
Hammer/UsesBakedLighting
ResourceCompiler/BakedLighting/DisableCullingForShadows
ResourceCompiler/BakedLighting/LPVAtlas
BAKED_LIGHTING_LIGHTMAP / BAKED_LIGHTING_FROM_PROBE / BAKED_LIGHTING_VERTEX_STREAM
baked_shadow_index / baked_shadow_light_id
```

The compiler assigns each stationary light a baked shadow slice and stamps `baked_shadow_index` /
`baked_shadow_light_id` onto objects. The runtime equivalent of "unbake this object" is therefore
setting its `baked_shadow_index` attribute to `0xFF`, which is exactly what the global cvar
does to every light at once.

## 8. Ranked candidate controls

Predictions are hypotheses. None is proven.

| # | control | mechanism | predicted stairs / palm | fence / player | soft wall |
|---|---|---|---|---|---|
| 1 | per-object `baked_shadow_index = 0xFF` at `0x18008c956`, filtered by object | the only true per-occluder seam | drops | kept | unchanged |
| 2 | per-light write `0xFF` to scene light object `+0x154` via the existing `LightFx` hook | same effect as the cvar, scoped to one light | drops | kept | unchanged |
| 3 | `lb_shadow_map_cull_empty_mixed 1` | skips mixed shadowmaps with no dynamics in view | plant-only maps drop | player-containing maps kept | unchanged |
| 4 | binner state bytes `[+0xE539]` / `[+0xF331]` | equivalent to the cvar, no cvar needed | drops | kept | unchanged |
| 5 | `lb_enable_stationary_lights 0` | early-exits the whole light | drops | also loses the light's direct term | unchanged |
| 6 | `sc_disable_baked_lighting 1` | material lightmap gate | no | no | only candidate that can move it |

Candidate 2 is the cheapest real test: `LightFx` already holds this exact pointer at submit time and
already does SEH-guarded reads and writes into this block.

Candidate 1 is the only one that can beat today's behaviour, because it works per occluder instead
of per light. It needs a new hook, so it is worth doing only if candidate 2 confirms the field
semantics first.

## 9. SST→CSM copy builder (native seam candidate)

Recovered with `Automation/tools/re_sst_csm_boundary.py` on the same `scenesystem.dll`:

| | |
|---|---|
| Builder entry | VA `0x180157510`, RVA `0x157510` (unique prologue; one hit) |
| Vtable/ptr slot | `0x1805eb6b8` (no `E8` callers; no rip-relative refs to the slot) |
| Gate cvar | `sparseshadowtree_copy_to_shadow_atlas_ps` |
| Named passes | `CopySSTDepthToCSM_PS` (gate ≠ 0) / `CopySSTDepthToCSM_CopyTex` (else) |

Runtime probe: `ShadowStageProbe.cpp`, installed from `HookSceneSystem`. Default is log-only.
Suppress returns `nullptr` without scheduling the pass.

```
mvm_debug start
mirv_filmmaker world shadowstage logging on
mirv_filmmaker world shadowstage suppress on|off
```

Locked Mirage A/B (ClientVersion 2000880): the hook is installed, but
`shadowstage calls` stays at 0 across baseline / suppress / sun-yaw / baked toggle. Pixel
means for suppress vs baseline were 0.0 on stairs, palm, soft, and fence crops while
`lb_enable_baked_shadows 0` still moved stairs/palm ~+40/+49. Do not promote SST copy
suppress. The builder is not on the live Mirage plant-shadow path (or is never entered
after map load). Verifier: `Automation/verify/verify-sst-csm-boundary.ps1`.

Per-light index fallback (`LightFx`, scene light `+0x154`):

```
mirv_filmmaker world light bakedindex log on
mirv_filmmaker world light bakedindex force on|off
```

Verifier: `Automation/verify/verify-bakedindex-boundary.ps1`

With `world customshadow on`, `CustomShadowAtlas` borrows live CSM fills (even if the
Shadows master is off), copies the native `R16` cascade atlas, clears the engine atlas so
native sampling contributes nothing, then `ContactShadowPass` composites sun-space shadow
tests onto the colour buffer. Tunables remain `strength` / `reach` (softness) / `bias` /
`steps` (PCF taps). Lighting-panel controls persist in the filmmaker settings store.

### Native CSM atlas (Mirage probe)

Locked Mirage protocol + `world shadowmap probe on` + `mvm_debug`. Artifacts:
`Automation/runs/shadowmap-probe-20260806/` (see `FINDINGS.md`).

| observation | value |
|---|---|
| Atlas texture | `4352×5248`, DXGI `53` (`R16_TYPELESS`), samples `1` |
| Depth view | DSV `55` (`D16_UNORM`) |
| Sample view | PS SRV slot 6, viewFmt `56` (`R16_UNORM`) |
| Cascades | four `1280×1280` tiles at `(0,0)`, `(1280,0)`, `(2560,0)`, `(0,2048)` |
| Fill | depth-only `DrawIndexedInstanced` (`rtv=null`) with `csgo_depth_only`, plus character/weapon/foliage/cables/smoke depth variants |
| Consume | same atlas rebound as PS resource during lit scene draws |

This confirms live CSM is a real tiled sun-space depth atlas, not a screen-space overlay.
The custom directional map (CustomShadowFx) should own a separate map, using this shape as
the reference layout; do not hijack Valve PS slot 6 as the product path.

## 10. Runtime candidate verdicts

Locked Mirage protocol, ClientVersion 2000880 / `scenesystem` dated 2026-07-29.

| candidate | stairs | palm | fence | restore | verdict |
|---|---:|---:|---:|---:|---|
| `lb_enable_baked_shadows 0` (ref) | ~39.8 | ~48.9 | ~3.6 | n/a | product gate |
| SST `shadowstage suppress` | 0.0 | 0.0 | 0.0 | 0.0 | not on the live path (`calls=0`) |
| `light bakedindex force` | ~51.1 | ~26.5 | ~92.1 | 0.0 | rejected (destroys live CSM) |

Pass for a product seam still requires plant ≥ +25, fence within ±5, restore ±1.5, and yaw-live
response without nuking cascades.

## 11. Dead list

Confirmed dead by RE, do not re-sweep:

| control | mechanical reason |
|---|---|
| `sc_disable_baked_lighting` as the Baked toggle | all six readers are in mesh/aggregate draw paths; never reaches the light binner |
| `sparseshadowtree_enable_rendering` / `cascade_mask` | SST is a compression path feeding `CopySSTDepthToCSM`; disabling it falls back to rasterising the same occluders |
| `ShadowStageProbe` CopySST suppress | builder never entered on locked Mirage (`calls=0`); plant crops unchanged |
| `light bakedindex force` (`+0x154`→0xFF) | plant moves like baked OFF, but fence mean ~92; destroys live CSM and is not selective |
| `lb_csm_override_staticgeo_cascades_animated_verts` | `= 1` narrows exclusion to `CAN_RENDER_INTO_SST` rigid geo, i.e. excludes *more* fence-class geometry |
| `lb_mixed_shadows`, `r_shadows` | single shared reader `0x1801a1a00`; both are the shadowmap render dispatch, not a baked/live seam |
| `lb_enable_stationary_lights` as a product toggle | early-exits the entire light, losing its direct contribution too |
| entity `m_nSceneObjectOverrideFlags` | enum mapping matches the schema; the plant is not an entity but aggregate/world scene-object geometry |

## 12. Mirage A-site check

Offline live map, client 1.41.8.1 / build 10896; Environment relighting and the
custom-shadow overlay were disabled to isolate native behavior. The verifier captured native,
native yaw 90/180, baked OFF with live ON, baked OFF yaw 90/180 and pitch 30, both OFF,
then restored native lighting. See `Automation/verify/verify-native-sun-session.ps1`
(`-CurrentView` leaves the user's camera position and angles alone).

At A-site, disabling `lb_enable_baked_shadows` removed the palm silhouettes and most crate
cast shadows. Keeping native cascades enabled did not retain those casters. Yaw changed
surface shading, but matching movable cast shadows were not demonstrated. The state feed
reported persistent light writes while `lightRotationBuilds` remained zero. Do not interpret
`lightRotationOk` or the write count as proof that cascade direction updated.

The Environment renderer consumes only collision triangles and editor props. Increasing
resolution could not repair missing leaves. The visual-caster path below addresses that omission;
baked/native toggle combinations alone are still not a solved replacement.

## 13. Animated visual-caster proof

Mirage's merged `urban_trees_branches03_medium_animated` world-node asset exports through
the visual-caster converter (`Filmmaker/MapData/MapCasters.cpp`) into a separate `MVMCAST6` cache. It retains actual leaf triangles,
UVs, material alpha cutoff, the alpha texture, vertex-color animation weights and the eight
material sway/flutter parameters. The collision mesh remains separate: alpha cards must not
become solid physics walls or opaque ray blockers.

`EnvironmentFoliageShaders.h` reproduces the shipped `csgo_foliage` position deformation.
`EnvironmentFoliage.cpp` snapshots native game-time/wind constant buffers and the native 3D
noise texture at verified draw bindings. Reflection validates names, sizes and offsets; it
does not depend on a shader pointer address. Copies are GPU-to-GPU, without CPU readback or
writes into engine buffers. Clock/wind observation also works on non-foliage scene draws so
offscreen casters keep moving. Native clock changes invalidate the custom maps each frame.

The vertex-shader creation callback is attached for thread-safe shader metadata only. The
shared shader-bytecode cache/file tracing must not be enabled on compiler workers.

Validation: WARP stream-output comparison against the locally extracted native VS matched
4,608 vertex results exactly (`maxPositionError=0`). Time, zero wind, pause, seek, and observer
resource lifetime have native tests. Locked live Mirage captures show moving plant shadows,
including the camera aimed down with foliage offscreen; debug reports `time=1 wind=1 noise=1`
and advancing `nativeGeneration`. The verifier is
[`verify-foliage-animation.ps1`](../../Automation/verify/verify-foliage-animation.ps1).

This is a single-asset proof, not all-map coverage. Section 15 covers placed caster coverage. Native actors, doors and unsupported animation/noise variants
still require their own paths; no native skeletal-pose reconstruction is implied.

## 14. Independent Lighting → Sun controls

The Effects Lighting card separates these controls:

| Control | Changes | Does not change |
|---|---|---|
| Engine lights | RGB energy of submitted scene lights | Shadow switches and baked lighting |
| Baked lighting | The compiled lightmap, substituted per engine draw (`t1` black, `t2` flat fill) | Both native shadow switches, the engine sun, prop probes |
| Baked shadows | `lb_enable_baked_shadows` | Direct lights and material lightmaps |
| Live engine shadows | Native cascade count override | Baked-shadow switch and material lightmaps |
| Engine shadows | Both shadow switches together | Direct lights and material lightmaps |

Environment relighting temporarily owns direct lights and both native shadow switches; the
panel explains this and offers "Use engine lighting" to release them. The independent
choices are restored and saved rather than persisting the temporary forced-off state.
"Baked lighting" remains separately editable, mirrored by "Remove baked lighting" in
Environment.

The switch does not use the `sc_disable_baked_lighting` cvar. That cvar moves a Mirage
frame by only 0.044/255 while blacking some props. The switch substitutes the lightmap arrays
on each engine draw through `NativeMaterialUnbakeScope`; see
[environment-scene-editor](../features/environment-scene-editor.md) for what it does and does not
remove. Disabling lights can make their shadows invisible even with the shadow switches enabled.
Removing the lightmap cannot remove shading painted into textures, baked reflection cubemaps or
light-spill decals, and a material the replay does not cover keeps its native image. These
controls expose distinct inputs, not a claim that every map's baked contribution has been cleanly
separated.

Live Mirage CT-ticket/stairs A/B testing with
[`verify-lighting-layers.ps1`](../../Automation/verify/verify-lighting-layers.ps1) confirms the
material gate actually changes: debug records the resolved pointer, value offset and both
write directions, and Config reports the resulting value. With native direct lights disabled
and Environment relighting enabled, removing baked lighting makes the weapon and small crate
props black while the bright bands on the stone walls remain. This is therefore not a clean
unbaked/albedo rendering path, and it remains opt-in. The surviving bands alone do not prove
whether their source is a texture, another material lighting path or a decal. Separating that
per-material contribution and supplying replacement indirect light is still unresolved;
repeating the disproven light-binner experiments above does not address it.

## 15. Placed caster coverage and receiver detail

Normal preparation now visits all packaged world nodes, preserving both nested model instances
and aggregate-fragment transforms. The `MVMCAST6` cache records each material's native texture addressing
(wrap, mirror, clamp, border, mirror-once). Cutout foliage/fences retain alpha; opaque foliage
trunks and supported vertexlit/complex props cast too. Skinned entity exports, overlays and
non-shadow-casting/blended materials are not turned into opaque static blockers. Opaque
materials do not interpret their unused UV stream as valid texture coordinates.

Prepared coverage: Mirage 177 batches / 1,096,512 visual triangles; Ancient 26 / 1,626,896;
Overpass 334 / 2,290,298. These counts establish export coverage, not live approval on every
map. Mirage has live captures in `Automation/output/caster-coverage`. Overpass's manifest
explicitly reports a 4,118-triangle juniper subset without animation weights and an
11,644-triangle umbrella using animation variant 2: both currently cast in rest pose.

Animated shadow updates restore a cached rigid-depth base, then draw the current native wind
pose. They do not redraw every rigid map/prop triangle or accumulate previous leaf positions.
The sun has full-map, 2048-unit, 512-unit bridge and 128-unit close regions, each 4096 square. The
close region's 1/32-unit texels give four times the linear detail of a 512-unit region without
extra texture memory. Its 16:1 resolution jump into the 2048-unit map shows as a sharp-to-blurry
band across ground foliage shadows, so the 512-unit bridge breaks that jump into two 4:1
transitions, retaining offscreen caster depth. The bridge costs 64 MiB plus a 64 MiB
rigid-depth cache when animated foliage is present. Interior pixels use their finest region only;
boundary pixels blend adjacent maps. Compact bilinear comparisons retain hard edges.
GPU receiver tests cover bridge selection, its continuous outer blend and unavailable-map fallback;
math tests check texel size, overlapping snapped regions and preserved upstream caster depth.
The hook was checked at the same Mirage A-site camera. Nearby crate and ground foliage shadows
show fine detail in two captured wind poses; runtime
logging confirms the 512-unit bridge is bound with 0.125-unit texels. This is a local visual check,
not an all-map sign-off or a fix for the separate inspect-animation projection report.
The close center snaps to eight-unit cells (the intermediate center still uses 32), keeping nearby
hands away from the blend boundary while allowing cache reuse for small camera movements.

A read-only viewport trace measured world depth 0 to 0.95, skybox 0.95 to 1, and viewmodel 0 to 0.1.
`EnvironmentViewmodel` snapshots the native inverse projection/camera offset and pre-viewmodel
depth on the render thread. Shader-owned immutable reflection metadata avoids compiler-worker
cache races. Reconstruction only uses that projection for depth samples replaced by the native
viewmodel; MSAA sample pairs, unrelated depth targets and frame invalidation are checked.
The native inverse projection can produce camera-relative coordinates. Reconstruction subtracts
the inverse-projected perspective eye in that same native space, then anchors the result at the
actual camera. A sparse, asynchronous GPU probe at the Mirage glove view measures tens of units of
receiver/camera separation (about 17 to 31 across weapon poses), and a Mirage capture shows
foliage shadows on gloves and the knife rather than large black triangle patches. This is not an all-map visual
sign-off. A local tangent-plane fit reduces depth-derived faceting without repeatedly computing
four-neighbor normals at every filter tap.
This is a receiver correction, not a claim of native material normals or skeletal self-shadows.

Wetness does not derive material porosity from screen-space geometric-normal derivatives,
because those draw triangle seams into the film. Puddle-only coverage also uses the filtered coating
normal. `EnvironmentReceiverTests` runs the production shader's reconstruction and wetness
functions on WARP, testing native snapshot immutability, world/hand separation, translated native
eye versus actual camera anchoring, planar normal preservation, MSAA, and
invariance to geometric triangle edges when the coating normal is unchanged.

## 16. Receiver warp and missing scene-light inputs

A fixed-view Mirage comparison at receiver bias 2 versus 0.2 reduced the bent shadow contours
on the pistol and gloves. The surface sun path keeps only the light-direction offset and does not
add `normal * shadowBias`. At the default bias, rotating normals would displace lookup positions
sideways by multiple shadow texels. The per-cascade sampler
retains its half-texel normal allowance and slope-scaled depth comparison. Snow and wet-film sun
lookups follow the same policy. `EnvironmentReceiverTests` uses a sharp blocker silhouette and
opposing receiver normals with equal sun incidence: the corrected production shader preserves
visibility, while a compiled reproduction of the normal-offset lookup fails that invariant. This does not
establish that every projection, material-normal or animation artifact is solved.

### Viewmodel material contrast and visible contact occlusion

First-person samples do not use the wall-lighting luminance division, which flattens native dark
metal/grip detail before applying the new sun. Their retained native shaded colour uses a linear
response (fixed 2x fill normalization), preserving its contrast rather than estimating albedo
independently at each pixel. World receivers keep their existing behaviour.

A bounded eight-tap, 1.5-world-unit contact-occlusion estimate uses the same native viewmodel
projection and per-sample classification. It attenuates ambient/indirect light, not direct sun or
added lights. World pixels, offscreen taps and distant surfaces cannot become hand occluders;
coplanar samples remain unoccluded. GPU tests cover these boundaries, MSAA planar receivers,
material contrast and direct-versus-ambient energy. There is no new user tuning control.

This is a hybrid shading improvement, not native PBR reconstruction: depth does not supply the
authored tangent-space normals, roughness, metallic or material AO channels. The linear gain is
not a recovered physical albedo. Visible-surface AO cannot make players cast onto the floor or
recover hidden/offscreen self-shadow casters. Those inputs still require separate integration.
The hook and native render/receiver suites passed. Live Mirage captures cover idle and
inspect poses on the AK and pistol; debug output confirms native viewmodel classification and
active rendering. Camera and weapon changes during the run prevent a controlled visual
comparison or an all-weapon material-quality sign-off.

Two missing inputs must not be confused with this receiver fix:

- Animated casters: native posed-mesh capture includes submitted character/weapon
  meshes; the first-person distortion correction is described in section 17.
  Receiving a palm shadow is not proof of casting one. A first-person weapon receiver remains
  distinct from the third-person body/weapon pose needed to shadow the map. Screen-space contact
  shadows cannot replace missing offscreen or hidden casters.
- Authored map lights: custom direct lighting currently consumes up to 16 editor-created
  lights, not the compiled map light entities. Map material emission is not imported into the
  custom GI volume either. A glowing texture alone does not establish a shadow-casting light.

Read-only VRF inspection of the installed map entity lumps found:

| Map | `light_barn` | `light_omni2` | `light_rect` | `light_environment` |
|---|---:|---:|---:|---:|
| Mirage | 7 | 20 | 45 | 1 |
| Nuke | 144 | 0 | 30 | 1 |
| Overpass | 124 | 52 | 19 | 1 |

These are resource records, not a count of simultaneously active runtime lights. Preserve
entity-lump transforms, enable state, color/temperature, shape, range, brightness units and
shadow/direct/bounce modes before deciding how to convert them. For example, Nuke includes a
rectangle with `brightness=-0.738281` and `brightness_legacy=0.601562`, plus separate lumen,
candela and nit fields: blindly treating `brightness` as a linear scalar is incorrect. Some
fixtures explicitly have `castshadows="0"`. Import must avoid double-counting native direct
lighting and baked illumination; it must not silently turn every fixture into a shadow light.

The supplied UEditingTools 4.7 package contains UE assets/Blueprints, character and weapon
materials/animations and demo-parser source, but no `.umap` files or standalone renderer source
was found in this copy. Its [published workflow](https://adenex.ru/en/s2-ueditingtools/) imports
animated scenes/POVs into Unreal; the published guide is labeled 4.4, so it is not a complete
description of the local 4.7 package. Unreal then supplies its own lighting and shadow renderer.
Useful principles are full animated caster geometry and separating material emission from
light transport. Those engine implementations cannot be assumed to copy into this D3D11 hook.
See Epic's [emissive material guidance](https://dev.epicgames.com/documentation/unreal-engine/using-the-emissive-material-input-in-unreal-engine)
and [virtual shadow maps](https://dev.epicgames.com/documentation/unreal-engine/virtual-shadow-maps-in-unreal-engine).

## 17. Posed-caster capture and first-person distortion

`EnvironmentAnimatedCasters` replays verified native character/weapon vertex draws into a
GPU stream-output buffer after native skinning. It matches the complete reflected output
signature: the measured native `SV_Position` occupies register 8, not register 0. The native
projection inverse already restores world coordinates, so subtracting the camera offset again
is incorrect. The GPU fixture models camera-relative forward perspective projection and
a world-returning inverse, including a second camera translation.

Live Mirage diagnostics place sampled character vertices near the
visible actor rather than thousands of units away. Third-person standing, crouching and walking
captures show a changing actor/weapon silhouette; changing sun azimuth changes its direction.
These observations do not establish correct first-person casting. A first-person versus
third-person comparison shows a detached/incorrect held-weapon silhouette in first person. Hiding the visible viewmodel with `r_drawviewmodel 0` leaves that malformed ground
silhouette: excluding the `.1` HUD draw alone is not a solution. This test alone does not
distinguish a skeletal-pose difference from distortion applied by the vertex shader.

Inspection of the extracted native character VS identifies a concrete distortion:
`g_flFirstpersonLegsFade` with `g_bDisableFirstpersonLegsDistortion == 0` scales and shifts clip
coordinates according to vertex height and camera pitch, after the normal world projection.
Inverting those modified clip coordinates produces distorted shadow geometry. Character and
weapon VS variants also apply `g_flPlayerOcclusionAmount` / `g_flWeaponOcclusionAmount` clamps
before projection. These camera-presentation operations must not deform world shadow casters.

The shadow-only replay uses a private GPU copy of the reflected `$Globals` buffer. It
disables those presentation clamps and first-person legs distortion, and removes the weapon's
projection Z bias, without modifying the native constants or visible rendering. Scratch buffers
are reused in a bounded size-keyed cache. GPU tests verify the undistorted captured positions,
unchanged original buffer contents, and restored VS/compute bindings. Static scene changes
must refresh the retained rigid-shadow cache before animated casters are added, even when the
invalidating frame contains no animated draw.

The animated-caster, receiver, render and math suites passed after these changes. The installed
CS2 build reports `accepted-globals-scrubbed` for native character and weapon draws. Live Mirage
testing accepts the held-weapon casting correction. This acceptance is not a sign-off on all receiving-shadow cases, nor on general hidden-body and offscreen coverage.

A Mirage receiving-shadow report was checked with paired color/depth captures,
shadow enable/disable, and sun azimuth 65/75 comparisons. The native depth contains the player's
surface rather than the floor behind it. A throttled, same-frame native/receiver matrix probe
measured less than 0.000007 NDC difference for sampled character and equipped-weapon triangles
(under 0.01 screen pixel at 1600 pixels wide). The first-person probe separately classified the
viewmodel with reconstructed distances of roughly 17 to 24 world units. These checks did not establish
a misplaced body projection in that reproduction; they do not prove every viewmodel animation or
material path. The added perspective receiver regression checks that raised and floor surfaces
at the same screen location reconstruct to distinct positions and receive different shadow results.

A shadow received on an opaque body does not erase an upstream object's floor shadow. Both
objects still block those light rays; the floor receives the union of their occlusion. Body and
floor silhouettes therefore need not join in screen space. Do not implement a screen-space floor
shadow eraser to imitate interception by a raised receiver.

The required contract is camera-independent: the world shadow uses the full third-person
player and equipped weapon pose in first person, third person and free camera. First-person
hands/weapon self-shadowing needs a separate source and must not add another gun to the world
shadow. Current `.95` viewport and character/weapon shader-name gates do not prove that contract.

### Interim first-person receiver isolation

Viewmodels do not sample the same shadow depths as the world, which include the third-person body,
arms, hands, and equipped-weapon representation. Those poses need not coincide with the viewmodel,
so reusing them can produce duplicate gun and hand shadows, and a moving triangle cutoff can
flicker during viewmodel animation. The viewmodel-safe layer uses a stable,
camera-anchored head-and-torso blocker instead of any captured third-person character or weapon
geometry. First-person hands and weapons therefore receive a body/head shadow plus world/editor
geometry and native wind/alpha-aware foliage, without receiving mismatched third-person arms,
hands, or equipped weapons. The proxy follows the camera and remains available when the local
third-person body is hidden or culled.

Captured character and weapon casters both remain complete in the world/third-person actor maps,
so the viewmodel policy does not alter their shadows on the map. The proxy is intentionally a
coarse first-person blocker rather than a second animated body: it does not reproduce individual
limb silhouettes. Captured remote characters also do not currently shadow the viewmodel. Full
owner-aware filtering would be needed to combine only remote actors with the local proxy. Native
material contrast and the existing contact-occlusion treatment are unchanged.

Sun cascades and editor-light faces keep the viewmodel-safe layer (environment plus the stable
body proxy) separate from the full actor layer while first-person receivers are active. World
lookups use the nearest depth from either layer; viewmodels use only the viewmodel-safe layer. The
viewmodel cache is keyed to the proxy transform and foliage pose, not native arm animation, which
prevents inspect/reload motion from rebuilding the proxy shadow. The native viewmodel pass and
matching depth resource identify eligible receivers, not a camera-distance cutoff. Missing
receiver layers fail open instead of falling back to the duplicate body/weapon map. Allocation is
lazy and retained across first-/third-person switches: four 4096-square sun layers require 256 MiB;
the optional 24-face 1024-square editor-light array requires another 96 MiB.

GPU regressions exercise stable first-person body receipt, third-person character/weapon exclusion
with their world shadows preserved, retained rigid blockers and alpha-aware foliage, actor
disappearance while the local proxy remains, disabled lights, repeated cache requests, and the
actual `Surface` receiver selector. Full owner-aware filtering remains follow-up work.
A read-only scene probe found pawn and held-weapon handles at offset `0xC0` in sampled Mirage
scene objects, but this is only a candidate ownership link: batch/instance propagation to the
deferred D3D draw, attachment ownership, other actors, and game-update resilience are unverified.
That probe and the shared per-view shader flags are not used as a production owner filter.

Read-only native-pass probes confirm character and weapon submissions into Mirage's
`4352x5248` D16 atlas. The native scene-list evidence separates `DrawDepthOpaque` from
`DrawDepthViewModel`; the latter is not a valid world-body source. A second integration issue
is verified: borrowing native cascade fills while Environment suppresses native direct-light
energy yields no actor cascade draws. Restoring native light energy restores those draws.
Consequently, extending coverage through native shadow passes requires suitable shadow-only
submissions independently of native direct lighting; borrowing or sampling the atlas alone is
insufficient. The current replay still depends on meshes submitted to the observed world pass.

Do not generalize the accepted held-weapon correction to hidden-body/offscreen support, generic
moving props/grenades, all-map coverage, or complete material/PBR relighting. Dropped/world
weapon capture alone does not establish the held-weapon path. Live evidence is kept under
`Automation/output/animated-casters/`; generated captures and machine-local logs are not source
documentation.

## Related references

- [`sun.md`](sun.md): sun/light rotation, the locked Mirage protocol, measured A/B tables
- `Automation/verify/verify-shadow-separation.ps1`: baked vs live product gates
- `Automation/verify/verify-sst-csm-boundary.ps1`: SST copy suppress
- `Automation/verify/verify-bakedindex-boundary.ps1`: per-light `+0x154` candidate
- `Automation/tools/re_lightbinner_shadows.py` / `re_sst_csm_boundary.py`: RE tools

## 18. Environment sun cascade ladder

Environment's own sun uses four camera-centred cascades. `SunCascadeHalfExtents` in [EnvironmentShadowMath.h](../../AfxHookSource2/Filmmaker/Environment/EnvironmentShadowMath.h)
derives the whole ladder from `shadowRange` and the map radius, spaced by one constant ratio.
Only the close tier stays fixed, because first-person receivers magnify it most. Measured live on
Mirage at the default range of 3000:

| Cascade | Span | Texel size (units) |
| --- | --- | --- |
| close | 128 | 0.031 |
| detail | 366 | 0.089 |
| middle | 1048 | 0.256 |
| outer | 3000 | 0.732 |

Neighbouring tiers differ by a constant 2.86x ratio, so quality steps evenly as the camera moves.

Geometry beyond `shadowRange` does not cast a sun shadow, because the outer tier is
centred on the camera rather than covering the map. That is why the range is a user control
(256 to 8000) and why it clamps to the map radius. A value at or above the map's own size
covers the full map. The outer cascade redraws as the camera moves; its snap is 32 units, the same
as the middle tier. The frame-time cost of the dynamic outer cascade has not been measured against
a matched run. The environment pass reads 3.4 to 5.4 ms with GI on.
