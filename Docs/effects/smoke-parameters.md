# Native CS2 smoke parameters

## Feasibility and evidence

Installed CS2 `1.41.8.8`, build `10924`.
The running game was an insecure local Inferno session. Health reported all
version-coupled subsystems resolved. Appearance controls and live rendered-volume
spread, voxel size and grid count are implemented, staged and verified in-game.

Density, wispiness, noise and motion can be exposed through the smoke renderer.
Valve ships named material parameters and compiled shader variants that read them.
The volume controls scale and resample the visible cloud. CS2's physical simulation,
recorded voxel frames, collision and bullet/HE interactions keep their native data.
Increasing the rendered grid count does not create additional simulated detail.

Evidence comes from the installed `client.dll`, `server.dll`, materials in
`csgo/pak01_dir.vpk`, and shaders in `csgo/shaders_pc_dir.vpk`. Assets were read with
Source2Viewer CLI; native code was disassembled with Capstone and DXBC with `fxc`.
The shader resource version is 72. The compressed blocks contained 392 pixel and
296 compute programs; their DXBC boundaries and lengths were validated, and selected
programs were disassembled. Raw evidence is generated locally under `build/smoke-re/`.

Binary SHA-256 identities for reproducing this analysis:

- `client.dll`: `d7db25d48f1d10c5e0b0296e20ed803426eb9509da41760daeda39dd35ba89b9`
- `server.dll`: `098d4ddd57e2fbe9a73623a2bf68ebaff86f7b6342ddb3d5a0f69cd6335b31cc`

These addresses and layouts describe this binary snapshot, not update-stable contracts.

## Using the controls

Open CONFIG > EFFECTS > ATMOSPHERE > LOOK. Changing a slider enables
Custom appearance. Switching it off restores native rendering while keeping the
slider values; Reset appearance restores the shipped defaults and switches it off.
Settings are saved with the existing settings store and Effects carry-over preference.
For cell size, grid count and cloud scale, open ATMOSPHERE > VOLUME.
The card's sub-tabs are SMOKE (colour), LOOK, VOLUME and WORLD TINT.
Its Custom smoke toggle controls the same override as Custom appearance.
Reset appearance, Reset smoke, the Atmosphere card reset and the bottom Reset Effects
button all restore defaults and switch the override off.

```text
mirv_filmmaker world smoke appearance
mirv_filmmaker world smoke appearance density 0.2
mirv_filmmaker world smoke appearance wispy 0.8
mirv_filmmaker world smoke appearance spread 0.5
mirv_filmmaker world smoke appearance voxel_size 80
mirv_filmmaker world smoke appearance resolution 24
mirv_filmmaker world smoke appearance off
mirv_filmmaker world smoke appearance reset
```

The first command reports values, supported shader counts and applied draws/dispatches.
Each parameter command below accepts a finite number and clamps it to its range.

| Parameter | Default | Command range | Meaning |
| --- | ---: | --- | --- |
| `density` | 0.85 | 0-20 | Volume opacity/coverage; zero removes volume rendering. |
| `wispy` | 0 | 0-1 | Wisp deformation. |
| `speed` | 1.5 | 0-10 | Noise animation speed; zero stops this noise motion. |
| `scale_low` | 3 | 0.05-20 | Coarse noise frequency. |
| `scale_high` | 7 | 0.05-20 | Fine noise frequency. |
| `strength_low` | 0.5 | 0-1 | Coarse layer amount. |
| `strength_high` | 0.35 | 0-1 | Fine layer amount. |
| `noise` | 0.8 | 0-20 | Overall distortion; the UI offers 0-3. |
| `boil` | -0.3 | -1 to 1 | Boiling deformation. |
| `ray_step` | 6 | 0.5-20 | Rendering sample spacing; smaller values cost more GPU time. |
| `spread` | 1 | 0.25-2 | Visible cloud scale around its centre. |
| `resolution` | 32 | 4-64 | Rounded rendered cells per side. |
| `voxel_size` | 20 units | Positive finite input | Sets the linked grid count; resulting size is limited by spread and resolution. |

Noise scale has a positive floor to avoid native divisions by zero. Ray step is
rendering sample spacing, not voxel size. Volume controls use
`voxel_size = 640 * spread / resolution` and `cell_count = resolution³`.
Changing spread keeps the count and changes cell size; changing voxel size keeps
spread and adjusts the rounded grid count. The displayed count is the full rendered
grid capacity, not a count of occupied smoke cells. Appearance changes preserve recorded voxel frames and smoke gameplay.
Enlarged rendering additionally follows exported static map collision geometry.

## Appearance parameters

Both `materials/dev/smoke_volume.vmat` and `materials/dev/smoke_volume_cs.vmat`
contain the following values. Shader metadata exposes the authoring ranges below;
these are not visually verified safe slider limits. Material values can differ
from shader defaults.

| Control | Material parameter | Shipped material value | Metadata range |
| --- | --- | ---: | --- |
| Density | `g_flDensity` | 0.85 | 0-20 |
| Wispiness | `g_flWispy` | 0 | 0-1 |
| Noise animation speed | `g_flNoiseSpeed` | 1.5 | 0-10 |
| Coarse noise scale | `g_flNoiseScaleLow` | 3 | 0-20 |
| Fine noise scale | `g_flNoiseScaleHigh` | 7 | 0-20 |
| Coarse noise strength | `g_flNoiseStrengthLow` | 0.5 | 0-4 |
| Fine noise strength | `g_flNoiseStrengthHigh` | 0.35 | 0-4 |
| Overall noise strength | `g_flSmokeNoiseStrength` | 0.8 | 0-20 |
| Boiling deformation | `g_flBoilStrength` | -0.3 | -1 to 1 |
| Ray march step length | `g_flRayStepLength` | 6 | 0.1-20 |

Additional shipped controls include noise offset/power/min/max, volume normal blend,
noise normal strengths, ambient/direct/rim lighting, interior darkening, AO strength
and distance, shadow quality and interpolation steps, and an extra-sample toggle.
The noise asset is a volume texture derived from `materials/dev/noise/worley_perlin_z0000.tga`.

### Parameters in compiled shader variants

Metadata's material constant layout is 144 bytes. Its scalar offsets are expressed
in four-byte units; disassembly confirms these example byte offsets in CB0:

| Parameter | Byte offset | DXBC operand |
| --- | ---: | --- |
| `g_flSmokeNoiseStrength` | 8 | `cb0[0].z` |
| `g_flDensity` | 12 | `cb0[0].w` |
| `g_flNoiseSpeed` | 16 | `cb0[1].x` |
| `g_flNoiseScaleLow` | 44 | `cb0[2].w` |
| `g_flNoiseScaleHigh` | 48 | `cb0[3].x` |
| `g_flRayStepLength` | 80 | `cb0[5].x` |
| `g_flBoilStrength` | 84 | `cb0[5].y` |
| `g_flWispy` | 128 | `cb0[8].x` |

For example, extracted pixel program 200 multiplies sampled density by `cb0[0].w`
and uses `cb0[8].x` in noise/deformation calculations. Compute program 200 also
reads `cb0[8].x`. Pixel program 390 has the CB3[1990]/CB5[156] declarations associated
with the project's existing native smoke lighting recognition, and reads the material
appearance parameters too. So the shader reads these as real inputs, not unused names.

Other variants, including the earliest extracted programs, read only the mixed-resolution
scalar from this buffer and fold appearance values into instructions. Therefore a blanket
CB0 write is insufficient for all variants. Implementation must verify the selected program,
and either select a compatible parameterized variant or patch the relevant computation.
The numeric program indices above refer only to the extraction order in this snapshot.

## Hidden console variables

The following inventory came from the running game's `find smoke` after
`mirv_cvar_unhide_all`. That command changes command/variable visibility flags for the
session; it does not change smoke tuning values. Writing `cl_smoke_edge_feather` reads back
correctly. This verifies the setter, not the appearance of a modified smoke.

| Variable | Observed value | Evidence/limitation |
| --- | ---: | --- |
| `cl_smoke_edge_feather` | 21 | Client analytical density code divides its edge distance by this value. Keep positive. |
| `cl_smoke_torus_ring_radius` | 61 | Client analytical density/bounds path reads it. Actual voxel spread is unverified. |
| `cl_smoke_torus_ring_subradius` | 88 | Same analytical path. Actual voxel spread is unverified. |
| `cl_smoke_origin_height` | 68 | Read by that client path. |
| `cl_smoke_lower_speed` | 1 | Client smoke update code reads it and multiplies it by frame time. Visual interpretation still needs a controlled test. |
| `cl_smoke_volumeprop` | true | Selects a branch in the analytical density path. |
| `cl_smoke_volume_growth` | 1 | Registered; no direct simulation/render read found by this scan. |
| `smoke_use_noise_texture` | true | Registered; shader variants also have `D_SMOKE_USE_NOISE_TEXTURE`, but the scan did not establish a live connection. |
| `smoke_param1` … `smoke_param5` | 6.26, 8.27, 0.13, 0, 0 | Registered on client and server; this scan found registration/destruction references, not direct simulation/render reads. Do not assign invented meanings. |
| `smoke_volume_lod_ratio_change` | 0.6 | Registered client variable; visual effect not tested. |

Rendering switches also include `r_csgo_smoke_shadow`, `r_csgo_smoke_fullres_pass`,
`r_csgo_smoke_fullres_enhance`, `r_csgo_smoke_avoid_flat`, and upsampling/depth controls.
`cl_use_prompt_smoke_density` and `inferno_smoke_volume_density` are thresholds for
other behavior, not controls for the rendered cloud's density.

The xref scan checks direct RIP-relative references to the ConVar global and its
data pointer. Absence of those references is not proof that an indirect reader cannot exist.

## Voxel size and spread

No smoke voxel-size or resolution ConVar appeared in the live unhidden inventory.
The installed client allocation function at RVA `0x7D4790` constructs a shared
`SmokeFluidA%dx%dx%d` texture with fixed dimensions 542 × 32 × 32. The packed
dimension constants are visible near RVA `0x7D49EF`; the native ray march also contains
32-cell addressing and `1/32` constants. This supports the existing 32-cell volume
research, but the GPU resource is an atlas, not a standalone per-grenade RGBA allocation.

That function also allocates a CPU state block of `0x10B010` bytes and initializes
several distinct subregions. This does not establish the size or ownership of the
separate pointer the disabled voxel-tint code treats as RGBA. Leave
`kVoxelWriteProven = false`; see [the fade-to-grey limitation](smoke.md#21a-fade-to-grey-limitation-unsolved).

Two different changes have different scopes:

- Rendered scale and shape (implemented): scale render bounds and density coordinates,
  then continuously interpolate neighbouring logical cell centres. The rendered grid
  ranges from 64 to 262,144 cells. A 32-cell grid preserves native sampling; higher
  counts resample the native field without creating new simulation detail. Coarse
  settings look softer rather than exposing hard individual cubes.
  Enlarged clouds use a 64-cell collision lookup generated from exported static map
  geometry. Neighbour links are ray-tested, and reachable cells follow paths through
  openings rather than a straight stretch through walls. Moving doors and props are
  not included in this geometry snapshot. Native gameplay and bullet/HE data remain
  unchanged.
- Simulation cell size, resolution or spread: reverse the server's `buildSmokeVolume`
  and `SmokeVolume::BuildSmokeSimulation` paths, voxel serialization/decompression,
  and matching client upload/atlas addressing. Demo smoke includes recorded voxel-frame
  data; tuning a local server generator does not automatically rebuild recorded frames.

Increasing grid resolution is not a one-constant patch. Allocation, strides, packing,
shader addressing and CPU/network data must agree. Halving cell width over the same
volume means eight times as many cells, before accounting for rendering work.

## Implementation and validation

[SmokeAppearanceMath.h](../../AfxHookSource2/Filmmaker/World/SmokeAppearanceMath.h)
recognizes parameterized SM5 pixel/compute programs by their material/view/volume ABI
and reads of all ten appearance controls. It redirects those scalar reads to an
independent CB11. The volume patch also recognizes native smoke mask vertex/pixel
and depth pixel programs. It scales paired dynamic volume bounds around their centre,
adjusts centre-relative world/grid conversions, and interpolates neighbouring logical
cell samples in the native packed atlas. All four recorded density frames retain their
packing and blending. The ray candidate mask is rebuilt from the scaled boxes, and
native stencil/scissor coverage does not cut enlarged rays off at the native cloud bounds.
The native full-viewport geometry and scene-depth tests remain in use.
CB11 holds 176 bytes including the spread and grid constants.
[SmokeAppearance.cpp](../../AfxHookSource2/Filmmaker/World/SmokeAppearance.cpp)
creates replacement shaders during engine shader creation/precache. Enable and slider
changes do not compile shaders. Draw/dispatch scopes bind the replacement and constants,
then restore the exact original shaders, CB11, warp texture/sampler, stencil state and
reference, and scissor rectangles, including the mask vertex shader.
CB0 and native volume resources stay owned
by the engine. Appearance binds after Environment's lighting scope, so compatible native
relighting replacements can also carry the appearance override.

[SmokeVolumeMath.h](../../AfxHookSource2/Filmmaker/World/SmokeVolumeMath.h) builds
collision fields on a worker from an immutable map mesh. Native GPU volume constants
are captured read-only at Map/Unmap and UpdateSubresource; native heap allocations are
never rewritten. The worker prepares a packed RGBA lookup at t119/s13, separate from
Environment lighting resources. Slider changes never wait on that worker. While a
new map/source/spread result prepares, the renderer keeps the requested custom spread
and appearance. Collision readiness is tracked per atlas slot: completed grenades keep
their field while another grenade prepares, and a reused slot cannot sample the old
source's field. Completed collision atlases and settings publish at frame boundaries
so opacity and colour passes use the same snapshot. Visibility-table reordering, camera culling and sub-unit origin rounding
do not invalidate existing fields. Newly seen or changed slots use custom resampling
until their static collision field completes, so they can briefly lack map conformity.
Voxel-size changes reuse the field. Smoke overrides apply to every recognized world
pass, including passes rendered after the UI boundary. Verified GPU volume constants
are captured even while the toggle is off, and full/partial resource copies preserve
their metadata instead of dropping it on engine buffer rotation.
Reachability fades density frames while lighting retains its intensity, avoiding the
black fringe produced by fading both.

The native `g_flDensity` input is a colour-coverage threshold, not a complete opacity
multiplier. Directly lowering it produced black patches in live CS2. Below the shipped
0.85 threshold, the override retains that threshold and scales both native per-sample
alpha and extra-opacity contributions by `density / 0.85`, before moment/colour compositing.
Both ray-march branches must match; partial matches stay native. Zero then removes the
volume without divisions by zero. Defaults preserve the native appearance.
CS2's separate inside-smoke screen overlay still uses native density: an inside-cloud
camera can retain haze even at zero. This control does not override that overlay.

Unknown layouts, occupied CB11, dynamic shader linkage and folded-constant
variants stay native. Supported program counts report recognized/prepared variants;
they do not promise that every graphics-quality path uses one. The disabled voxel writer
remains disabled.

Validation on this installed CS2 build:

- Built the release hook and staged both required runtime/hook components.
- D3D11 WARP tests cover pixel, moment and compute paths; parameter updates, finite
  bounds, unrelated/malformed programs, occupied CB11 and exact native shader/buffer
  restoration. A native-like two-branch GPU fixture checks premultiplied colour/opacity
  at density 0.85, 0.05 and zero.
- D3D11 accepted patched extracted pixel programs 200, 290, 350 and 390, compute
  programs 200 and 250, both actual live pixel shaders (45 redirected reads each),
  and native mask vertex, mask pixel and depth pixel programs.
- Native-shaped GPU fixtures verify unchanged default sampling, half/double spread,
  coarse/fine grids and reset restoration for pixel, moment and compute paths.
  Command regression checks include Panorama's trailing `quiet` argument.
- Native Inferno captures verify thin/zero density, visible wisp/noise changes, all ten
  applied values in `mvm_debug`, reset/off restoration, slow playback and the in-game UI.
  Environment on/off was exercised with the appearance override.
- An existing Inferno match demo was played through its first smoke event, paused at
  tick 7802, then played onward and sought back through the event. Thin-density/reset
  captures show transparency and restoration while paused; density 0.2 and wispiness
  0.7 persisted through playback and the seek. The inside-cloud zero test identified
  the separate native overlay limitation above.
- Saved density 0.2 and wispiness 0.7 survived a full CS2 process restart.
  Reset then restored the disabled state and all ten defaults in saved settings.
- Live verification used the pixel path. No native compute dispatch occurred in that
  session; compute execution is covered by WARP, not claimed as live CS2 proof.
- Volume UI checks covered actual drags and clicks on Reset smoke, Atmosphere
  Reset and Reset Effects. Enlarged shadow bounds do not prove expanded visible
  coverage: native stencil and candidate masks still constrain the visible cloud.
- The release build is tested on an existing Inferno smoke at spread 0.5,
  1, 1.5 and 2. The 1×/2× captures show visibly different cloud coverage. Live voxel-size
  commands selected 160-unit cells (8³) and 20-unit cells (64³) at spread 2 without hard
  voxel edges. Reset reported off with all defaults and restored the active native cloud.
  Growth completion and stencil bypass were confirmed in `mvm_debug`.
- WARP checks additionally cover continuous sampling across cell boundaries,
  per-ray candidate rejection, collision fading of density without dimming lighting,
  thin-wall rejection, paths through a doorway, packed-atlas guards, and exact native
  stencil/reference and scissor restoration.
- Environment lighting was tested with enlarged and native smoke. Its existing split
  between lit and shaded areas remains visible at both 1× and 1.5×. This work does not
  change that lighting behaviour. Native compute execution remains unverified live.

Generated captures and traces are under `build/smoke-re/`; density/UI evidence is in
`thin-fixed/` and `final-persistence/`, UI/reset evidence in `volume-final/`, and
visible growth, voxel sampling and runtime reset evidence in `smooth-growth/`.
A full matrix of quality presets, overlapping grenades, moving geometry and bullet/HE
holes remains outside this validation. Native simulation spread and allocation
resolution remain separate from these rendering controls.
