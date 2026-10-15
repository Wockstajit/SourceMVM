# Wicked Engine lighting comparison

**Comparison and evidence.** This page compares the CS2 lighting path with Wicked Engine's DDGI and
HBAO implementations and records inspected revisions and measured outcomes. Current behavior and
remaining verification gaps are in [whole-map relighting](whole-map-relighting.md). Items described
as proposals are not implemented.

Code inspection against Wicked Engine commit
[`df44c3db4c4927492bc9c791eac715d98d7ed091`](https://github.com/turanszkij/WickedEngine/tree/df44c3db4c4927492bc9c791eac715d98d7ed091).
Offline checks support this comparison. Live CS2 checks of Inferno's church and top-down
shadows are recorded below.
This is not a claim of Wicked DDGI parity or complete visual acceptance.

The main quality gap is geometric visibility and material sampling, rather than insufficient
smoothing. Increasing bounce or AO cannot recover visibility that the current representation
has lost. Our implementation is voxel-traced probe GI; it is not a port of Wicked DDGI or Lumen.

## Lighting and colour transport

| Area | Wicked reference | Current CS2 path | Consequence / next change |
| --- | --- | --- | --- |
| Ray intersections | Triangle intersections through hardware ray queries or the software BVH; loads the hit surface material. | Static geometry rasterized into 20-unit voxels; animated actors receive GI but do not enter indirect occupancy. | Thin blockers and detailed surface colours are lost. Idle poses would otherwise cause large visibility changes near the camera, so actors stay out of indirect occupancy. A DX11 software BVH is a plausible alternative; hardware ray queries require a different graphics path. |
| Surface colour | Uses hit-point albedo and actual emissive colour. | Map triangles/batches carry average linear material albedo. | Coloured lights do bounce, but a painted or stained surface loses texture detail. Capture UV/material sampling at the hit, or improve voxel material sampling first. |
| Sky | Samples directional static/dynamic sky on a real miss. | Six-band probe transport stores scalar sky visibility, composed with live sky colour. Rays stop after eight probe spacings (320 units). | A locally unobstructed ray is not proof it reaches the sky. Check the remaining path against whole-map geometry before assigning sky visibility. |
| Recursive bounce | Previous irradiance contributes with a 0.95 conservation factor; user GI boost is applied separately when shading. | Feedback uses a fixed 0.95 gain; the user boost applies only to displayed bounce. This prevents a high slider value from amplifying every recursive step. |
| Colour changes | Maintains RGB mean/variance history. | The driver hashes authored RGB lighting inputs. | Actual edits replace history at updated probes and double background work for 24 frames. Nearby and visible receivers refresh every frame. Steady updates replace deterministic world-anchored samples immediately, avoiding stale colour history during movement. Nearby atlas changes are regression-tested within four updates. |
| Global tint | Incident light colour reaches direct lighting and reflected transport. | Global world controls tint sun, lamp and sky inputs in both Environment-owned and standalone Relight modes. Lamp-only hue edits invalidate GI, even with a black sun and sky. |
| Numerical range | Uses float accumulation for irradiance/depth and limits storage to finite representable values. | Float accumulation, half-float history, and a component-wise radiance ceiling of 64 during injection. | The ceiling can change bright colour ratios. Replace it only alongside finite-range tests and consistent radiometric units; blindly copying `/PI` would be incorrect for our current cosine-weighted integration. |

Primary sources: [DDGI ray tracing](https://github.com/turanszkij/WickedEngine/blob/df44c3db4c4927492bc9c791eac715d98d7ed091/WickedEngine/shaders/ddgi_raytraceCS.hlsl),
[DDGI update](https://github.com/turanszkij/WickedEngine/blob/df44c3db4c4927492bc9c791eac715d98d7ed091/WickedEngine/shaders/ddgi_updateCS.hlsl),
[lighting composition](https://github.com/turanszkij/WickedEngine/blob/df44c3db4c4927492bc9c791eac715d98d7ed091/WickedEngine/shaders/lightingHF.hlsli).
Local implementation: [probe shaders](../../AfxHookSource2/Filmmaker/Relight/RelightProbeGiShaders.h),
[probe driver](../../AfxHookSource2/Filmmaker/Relight/RelightProbeGi.cpp),
[probe settings](../../AfxHookSource2/Filmmaker/Relight/RelightProbeMath.h).

## Probe visibility, leaks and black spots

Wicked stores a shared relocated position for each probe, directional depth moments in a
16-by-16 octahedral map, and colour in spherical harmonics. Its receiver query combines eight
probes with trilinear, surface-facing and Chebyshev visibility weights. The actual relocated
position is used for tracing and receiver visibility.

We store six broad hemisphere distance moments. All six bands share one relocated
origin, selected from adjacent open voxels within half a probe spacing. The signed axis and
fractional displacement occupy the fourth distance channel. An origin change discards old
radiance and moments; a probe with no open origin remains invalid in every band.

Native receivers, recursive feedback and the screen cache blend the three signed directional
visibility estimates continuously with squared direction weights. They also use Wicked's wrapped
surface-facing weight to reduce contributions from probes behind the receiver. Visibility
still uses cubed Chebyshev weights; entirely blocked or uninitialized cages return zero.
The native bytecode regression covers both Z directions and receivers at relocated origins,
including the degenerate direction where an equal angular mixture is required.

Composition filters between actual relocated origins, rather than nominal cells that can lie
inside geometry. It samples the connecting segment at half-voxel steps and excludes neighbours
across blockers. Production GPU regressions distinguish two probes relocated into the same
open space from probes relocated to opposite sides of a wall.

The sixteen directions per hemisphere are fixed by a world-cell hash. Camera scrolling
preserves that hash. This removes temporal sampling shimmer, but does not increase angular
resolution; stationary spatial sampling error remains a limitation. Further quality work must
refine directional visibility and geometry, rather than merely increase smoothing or AO.
Do not copy Wicked's minimum weight floor alone: normalizing blocked probes can admit leaks.

Diffuse opacity uses exact voxel-cell
traversal. A wall must block a ray that enters its cell, but must not block a parallel ray
in the adjacent empty cell; the production GPU test exercises both. Geometry backfaces
still contribute blocking distance, while their unavailable outgoing radiance is excluded
from the hemisphere average. Native and screen receivers both use a 0.35-probe normal offset.
Whole-probe backface rejection is not used because it creates missing-light gaps
under roofs. Neither this offset nor backface handling replaces Wicked's triangle visibility.

Source: [DDGI layout and receiver query](https://github.com/turanszkij/WickedEngine/blob/df44c3db4c4927492bc9c791eac715d98d7ed091/WickedEngine/shaders/ShaderInterop_DDGI.h).
Our [native receiver query](../../AfxHookSource2/Filmmaker/Relight/RelightIrradianceMath.h)
and [GPU regressions](../../Automation/tests/relight-probe-gpu-tests.cpp) cover the current representation.

The static voxel grid dimensions are uploaded to the shader constants, so `RegionVoxel` accepts
static cells and the runtime can bounce their material colour and use them as blockers. The driver
test exercises a real map triangle and verifies red reflected light with sky fill disabled.

GI light records compare their actual contents before reuse. Camera-dependent shadow
budgets can change a light's atlas tile without changing authored settings or the record count.
Changed records update a retained mutable GPU buffer; unchanged records reuse it. A production
driver regression moves the same record from blocked depth to lit depth while retaining the
authored key and count, and checks that the reflected red illumination follows the reassignment.

## AO and enclosed rooms

Wicked applies AO to indirect diffuse/specular, separately from direct light. It offers a
separable HBAO implementation with groupshared depth caching and incremental horizon
integration, plus an MSAO pipeline with depth preparation, multiple scales and bilateral
upsampling. These are separate from DDGI visibility.

Our contact AO uses a 32-unit world radius, eight screen-space directions with six samples
each, and a 5-by-5 bilateral filter. It accumulates newly blocked horizon intervals
with exponential distance falloff and applies a strength exponent, following Wicked's HBAO
integration principle. Position/depth preparation resolves MSAA and unprojects once per AO texel; each horizon invocation
reads cached positions instead of up to 53 depth reconstructions. A separate RGBA32F position texture costs about 8 MiB at half of 1920-by-1080 resolution.
This is not Wicked's separable groupshared implementation. Screen-space contact AO cannot
determine whether an entire room is closed, because it only sees nearby screen depth.
Whole-map sky depth maps and probe visibility are responsible for that distinction.

Setting contact AO to zero leaves separately enabled sky occlusion active.
An invalid main-view inverse still disables both, preserving the neutral fallback. WARP tests
exercise sealed/open sky with AO disabled, fractional sky strength, ordinary/reversed depth,
and single-sample/4x MSAA depth, stronger corner shading, immediate blocker removal and neutral
output for missing depth. With contact AO disabled, the horizon sample loop is skipped while
sky visibility remains independent. AO describes blocked indirect light; directional/local
shadow maps supply self-shadowing from direct sources.

The sky-map normal offset and depth bias still scale with whole-map texel size. On a large map
that offset can cross thin walls. Probe GI also suppresses the screen-space sky term while its
own sky visibility is bound. Stronger contact AO is not a substitute for fixing either path.
Keep local contact occlusion independent of distant sky blockage and test coloured bounce in
a room that has no direct view of the sky.

Sources: [HBAO](https://github.com/turanszkij/WickedEngine/blob/df44c3db4c4927492bc9c791eac715d98d7ed091/WickedEngine/shaders/hbaoCS.hlsl),
[AO orchestration](https://github.com/turanszkij/WickedEngine/blob/df44c3db4c4927492bc9c791eac715d98d7ed091/WickedEngine/wiRenderer.cpp),
[lighting composition](https://github.com/turanszkij/WickedEngine/blob/df44c3db4c4927492bc9c791eac715d98d7ed091/WickedEngine/shaders/lightingHF.hlsli).
Our [AO driver](../../AfxHookSource2/Filmmaker/Environment/EnvironmentAmbientOcclusion.cpp)
and [AO shader](../../AfxHookSource2/Filmmaker/Environment/EnvironmentAmbientOcclusionShaders.h).

## Shadow softness and stained glass

Wicked's pinned shadow helper enables a disk sample pattern. Dithering and PCSS blocker search
are optional defines that are **disabled in this reference file**; its default shadow-map path
is not automatically a contact-hardening solution. It clamps samples to each tile and samples
a separate transparent shadow atlas containing RGB transmission and depth.

Our recognized local-light shader permutations use five depth reads for blocker search and
a fixed 32-tap Vogel comparison disk for owned fixture shadows. The source extent and relative
blocker/receiver depths determine the penumbra; lamp angle and softness retain their artistic
multiplier. Zero footprint performs one comparison. Unknown permutations retain the native
kernel; recognized records without owned source metadata retain fixed-footprint filtering.
The atlas guard still caps the radius. Cached 128-square faces reserve 12 texels per edge;
nearby 1024-square faces reserve 96, giving both tiers the same angular filter margin. This
trades nearby central resolution for a wider usable penumbra. The owned sun path has separate
blocker-search filtering. A single depth projection remains an approximation for a tube or
extended area source; this does not implement multiple source visibility samples.
Audit supported permutations and clamp behaviour before claiming a slider fixes every light.
The church close-up reproduces the hard, stepped edges at zero softness. Increasing source
angle and softness visibly smooths them. The top-down comparison retains the same building and
tree shadow coverage. This does not establish acceptance for every shader permutation or angle.

Stained glass currently stays native opaque geometry. A separate coloured sun-cookie beam
passes through the window and is stopped by exported walls, floors and static props; that
cookie also enters GI injection. This does not make the pane itself transparent, nor does it
implement diffuse sky transmission. Self illumination is suppressed during takeover. Export
uses the stained-glass self-illumination artwork as transmission colour multiplied by incident
light, with a base-colour fallback. It must not be an always-on light source.

A full glass solution needs a verified transmission draw/depth path and coloured shadow
transmission, with direct sun, sky and bounced incident lighting handled distinctly. Test
sun off, all lights off, sky only, a blocked exterior window and an interior lamp. Projected
sun beams should disappear without sun; a pane can remain visible under real indirect light.

Source: [shadow helper and transparent shadow atlas](https://github.com/turanszkij/WickedEngine/blob/df44c3db4c4927492bc9c791eac715d98d7ed091/WickedEngine/shaders/shadowHF.hlsli).
Our [local shadow patch](../../AfxHookSource2/Filmmaker/Environment/EnvironmentNativeShadowMath.h),
[glass pass](../../AfxHookSource2/Filmmaker/Relight/RelightStainedGlass.cpp)
and [map caster extractor](../../AfxHookSource2/Filmmaker/MapData/MapCasters.cpp).

## Work scheduling and optimizations

Wicked allocates rays from per-probe inconsistency, reduces off-frustum budgets to 10%, keeps
at least four rays, caps at 512, and uses the full cap on the first frame. It prepares indirect
dispatch and profiles allocation, tracing and update stages. Off-screen probes retain work
because they can illuminate visible surfaces.

Our camera region has 64 cubed probes. The background updates one sixteenth each frame with 96 rays per updated
probe: **1,572,864 voxel rays per frame**, each with up to 64 march iterations. These are
calculated work counts, not GPU timings or a comparison with Wicked ray costs. Injection visits
the 128-cubed region; composition visits all six probe bands. Brick light lists already avoid
testing every light at every voxel, static voxels are cached, and camera movement scrolls history.

Radiance and direction mip generation is skipped when reflections are disabled.
Diffuse probe rays read level zero; the reflection cone tracer still needs those mips and retains
them.

Cached fixture faces refresh at most 32 faces per preview frame, prioritizing age,
visible receiver ranges and camera proximity. Sixteen slots are reserved for the oldest faces
so distant lights cannot starve. Existing cached depth is retained until refreshed; whole-map
static shadows are never removed because they are outside the view. Sun cascades and the
high-resolution local tier still update normally. Movie exports refresh every resident face.
Preview shadows of moving objects in the cached tier can lag while waiting for their turn.
Scheduling tests cover the cap, uniqueness, background progress and small sets. This requires
nearby moving-caster and top-down acceptance, especially where many light ranges overlap.

Preview caster-bound readback uses `D3D11_MAP_FLAG_DO_NOT_WAIT`.
If this frame's result is unavailable, bounds remain incomplete and culling conservatively
includes every submitted caster. It never uses an old pose to omit current shadows. Exports
still wait for exact current bounds. Incomplete bounds increase shadow draw work, so this avoids a CPU stall
without establishing an overall FPS improvement.

`mvm_debug` logs `relight.probe.gi.gpu` approximately every three seconds with
setup, injection, tracing, composition and reflection GPU milliseconds. Results
are polled asynchronously with `D3D11_ASYNC_GETDATA_DONOTFLUSH`; the renderer does not wait for
them. Map-load voxelization and region/history movement are outside this stage sample.
`environment.relight.shadow.spatial` reports resident/updated faces and the effective budget.
The live results below show that the reported FPS problem remains.

Next performance work should use those GPU timings to select validity/change-based update queues
and adaptive ray budgets. Preserve off-screen transport and reserve work for recently exposed
or changed probes. Avoid blindly increasing probe resolution or adopting a 16-by-16 depth map
at every existing probe: 64 cubed probes times 18 squared bordered texels is about 85 million
depth texels, before history and colour storage. Evaluate fewer probes, smaller cascades or
packed directional moments before committing that memory.

Sources: [ray allocation](https://github.com/turanszkij/WickedEngine/blob/df44c3db4c4927492bc9c791eac715d98d7ed091/WickedEngine/shaders/ddgi_rayallocationCS.hlsl)
and [DDGI orchestration](https://github.com/turanszkij/WickedEngine/blob/df44c3db4c4927492bc9c791eac715d98d7ed091/WickedEngine/wiRenderer.cpp).

## Verification and remaining acceptance

This pass adds production GPU regressions for display boost independence, equal-luminance
colour changes and relocated visibility, global-colour tests, shadow scheduling tests,
AO/sky independence tests, and a UI regression for fractional/zero bounce.
The Lighting page exposes bounce intensity from 0 to 4. Zero removes displayed reflected
light while retaining the GI visibility field; `mirv_relight gi 0` remains a separate diagnostic
that disables the GI implementation.

The automated suites run offline. Live checks cover a subset of the original acceptance list;
glass transparency/colour, exhaustive fly-through black spots and leaks,
and moving-caster scheduling remain unfinished.

The release build succeeded. The full native suite passed 60 of 61 tests; the baseline
`EnvironmentReceiverTests` failure remains at the viewmodel material-contrast assertion.
The full JavaScript suite passed 430 of 454 tests, with 24 failures.
The affected GI, irradiance, AO, environment UI and map-pack regressions passed, as did the
documentation integrity check. Logs are local ignored outputs under
`Automation/output/lighting-live/wicked-{offline-build,native-tests,ui-tests}.log`.
Colour, AO and scheduling checks use `wicked-performance-{build,native-tests,ui-tests}.log`
in the same directory.

## Live checks on Inferno

CS2 launched under the signed-in Windows account through the repository launcher without a
Steam SDK error. The hook was staged with both install components. Debug logs and engine
frame captures are under `Automation/output/lighting-live/wicked-runtime{,-after,-final}`.
The runtime debug log is written as `mvm_debug_<timestamp>.log` under `%APPDATA%\HLAE\debuglogs\`.

| Check | Observed result |
| --- | --- |
| Global red tint | Church walls, floors and lamp illumination receive red. This verifies incident tint, not sufficient indirect colour transport. |
| AO 0 versus 2 | Higher strength visibly darkens floor contacts and corners while retaining direct lighting. Whole-room visibility remains a separate GI/shadow responsibility. |
| Source angle / softness | Increasing source angle and softness smooths the stepped hard edges on the close church wall; the 16-tap filter was inspected. |
| Top-down shadows | A fixed pose retains roof, wall and cypress shadow coverage. White areas outside rendered map surfaces are present in the captures. |
| Stained glass with illumination disabled | With standalone Relight, intensity/sunscale/indirect/bounce/reflections zero, the panes are dark and the wall receives no projected beam. Native lamp emission remains visible. |
| Stained glass under normal lighting | The pane artwork stays dark/opaque. Transparency, incident-lit artwork and broad coloured transmission are rejected. |

FPS samples use render-pump counter differences over approximately eight seconds, 1600-by-1200,
the same church pose, and `mvm_debug` stopped. No WARP suite ran during these samples.
Environment is disabled during the standalone Relight comparison; disabling only Relight while
Environment is active is not an engine-only baseline. Shadow-size commands forward to Environment,
so the test explicitly disables it again before measuring standalone mode.

| Configuration | Measured FPS |
| --- | ---: |
| Both lighting modes disabled, engine lighting | 347.75 |
| Full standalone Relight, 16-tap filter, two runs | 24.47 / 24.72 |
| GI disabled, 16-tap filter | 27.84 |

The 16-tap filter halves filter fetches but does not materially solve FPS.
Disabling shadows produced 58.42 FPS; the one-record diagnostic produced
60.23 FPS, but also disables GI and changes the light workload. Neither is a quality-preserving
performance result. GI timestamp samples were approximately 2.6–4.5 ms in the church. The next
profiling work must separate shadow-map preparation, native material shading and binding work.
Do not treat the GI timer as the total renderer cost.

Debug instrumentation measures native material draws and execution of the map-light
shadow command list with asynchronous GPU timestamps. Full church samples covering approximately
205–206 material passes take about 16–17 ms; map shadow execution takes about 3–4 ms and GI about
3 ms. Partial material samples must not be compared with complete samples. Disabling GI or setting
the source angle to zero does not remove the measured material cost. Temporarily disabling shadows
reduces the complete material sample to about 4 ms, but removes required lighting visibility.
The live status reports `shadows=1`.

Conservative cluster bounds recognize perspective light-face/cone matrices and project their
five-vertex light-volume hull instead of the entire eight-corner range box. Unknown and orthographic
matrices retain the box fallback. Dense receiver tests cover all six cube faces and a rotated face;
the production GPU tests verify CPU/GPU mask parity. Paired measurements at the same church pose (30.69 and 31.05 FPS) do not establish a meaningful
performance improvement.
Church and top-down captures retain map shadows.

The Config panel also discards its stored Panorama context before teardown when the current HUD
has disappeared or changed. Repeated open/close cycles, test-harness stop/restart, disconnect and clean quit pass
with the current hook. The captured Lighting page shows Sun size, Light source angle and Softness
together. Logs and captures are under
`Automation/output/lighting-live/wicked-runtime-{final,profile,cone}`.

The release build and staging succeeded; native tests passed 60/61 with the viewmodel-contrast failure. The audited soft-shadow bytecode validates
on a real D3D11 WARP device. The final native log is
`Automation/output/lighting-live/wicked-runtime-after/final-native-tests.log`.

## GI artifact and resolution

The facade checkerboard is isolated to the GI path: it remains with contact AO and reflections
disabled, vanishes when GI is disabled, and a uniform-irradiance diagnostic also removes it while
probe visibility is retained. AO strength is not a repair for it. The shared-origin and
continuous-visibility handling leaves no large checkerboard on the captured facade, and the floor
and interior captures show no hard grid or diagonal seams.
Faint circular patches on the wine-sign wall remain under investigation; this is not complete
artifact acceptance. Top-down captures retain the roof, wall and cypress shadows.

The static-grid upload produces visible red indirect illumination on the church wall
and floor with direct sun and sky fill disabled when bounce changes from zero to four. Captures
are in `Automation/output/lighting-live/wicked-runtime-static-grid`, named
`de_inferno-actual-red-bounce-{zero,four}.png`.

`mirv_relight giscale 0.5` evaluates all six irradiance bands once per half-resolution depth
pixel. Native material queries use four depth-compatible neighbours, retaining native normal
maps, and fall back to world probes at uncovered or incompatible depths. `giscale 1` evaluates
world probes directly. Invalid depth is cleared each frame. The cache accepts CS2's main-view
0-to-0.95 depth range and excludes the viewmodel range. GPU tests exercise invalidation,
uniform irradiance, depth edges, fallback and surface-facing colour selection.

Live logs confirm an 800-by-600 cache for the 1600-by-1200 view. Two paired facade samples with
debug logging off measure 30.29/30.25 FPS at full resolution and 30.49/30.67 FPS at half
resolution. This does not establish a meaningful overall performance gain; native material
light/shadow work remains a larger measured cost. No shadows were disabled for these samples.
The binding helper also batches adjacent native resource gets/sets while preserving gaps and
nested restoration; its GPU state regression passes, but its FPS benefit is unverified.

A stationary interior comparison eight seconds apart with world-stable probe rays changes by
0.019 RGB levels on average, with 0.013% of pixels changing by more than eight levels. This is
one fixed-camera sample, not a guarantee against flicker during camera movement. Relevant
captures and logs are under `Automation/output/lighting-live/wicked-runtime-{screen-gi,filtered-screen}`.

The accepted build additionally guards degenerate normals in the screen receiver to avoid
NaNs. Surface-facing GPU checks use opposing red/blue probes. The near-origin native regression
checks the scalar threshold packed in the fourth DXBC component; the
query preserves uniform light at relocated probe origins.

Per-receiver short visibility segments through the existing 20-unit voxel grid are not used.
Further visibility refinement needs finer directional/geometry data.

The full native suite passes 60/61 with only the pre-existing
`EnvironmentReceiverTests` material-contrast failure. The full JavaScript suite passes
430/454, with 24 failing tests. The authored-sky key test
checks that sky intensity/hue edits accelerate convergence, while displayed bounce gain does
not. Logs are `Automation/output/lighting-live/wicked-runtime-facing/accepted-native-tests.log`
and `Automation/output/lighting-live/wicked-runtime-facing/full-ui-tests.log`.


## Shared composition and transmitted artwork

The composition pass traces the six neighbour connections once per probe and reuses the
visibility for all six irradiance bands. It retains the same valid-centre double weight,
invalid-centre filling, relocated origins and boundary handling. GPU regressions compare all
bands at interior and boundary probes with the original stencil, including invalid centres.
At the same Inferno facade, native GPU timestamps measure composition at about 0.048 ms. This is not a large overall FPS improvement. Probe tracing
still costs about 2.24 ms. Captures in `Automation/output/lighting-live/wicked-compose-after`
retain map and tree shadows in the top-down view and no large checkerboard on the ground.

Converter v15 uses the stained-glass material's exported self-illumination artwork as a
transmission colour, with a base-colour fallback. It does not export the authored emission gain
or restore always-on native emission. The live church comparison shows saturated blue, red and yellow
artwork on the opposite wall with sun elevation 25 and azimuth -90; disabling transmission or
setting sun scale to zero removes the projection. Captures are under
`Automation/output/lighting-live/wicked-glass-artwork`. The packaged map data takes precedence
over the user cache, and the debug log confirms that the packaged data loaded.
The opaque native pane still requires a separate transparency/render-order solution, and diffuse
sky transmission is unfinished. Neither is claimed fixed by changing the beam texture.

The full native suite remains 60/61 (the existing `EnvironmentReceiverTests` contrast failure).
The JavaScript suite is 432/456, with 24 failing tests; two export regressions pass. Logs are `Automation/output/lighting-live/wicked-compose-after`.


Finite orthographic glass-cookie records receive conservative screen-tile and depth-slice
bounds. The bounds invert only verified
orthogonal affine projections, expand by one world unit and one tile/slice, and retain the
unbounded fallback for malformed matrices or screen tiles crossed by the camera plane. The sun
has no cookie and remains map-wide. CPU tests independently sample beam interiors; GPU tests
cover straight/oblique beams, camera crossings and unsupported viewport fallback.

Two eight-second samples at the same facade measure 34.27 and 34.58 FPS. This is a fixed-view
measurement, not a map-wide
performance guarantee. Church and top-down captures preserve the coloured projection and map
shadows. Comparing the church projection region against `clusters 0` (the all-lights reference)
gives a mean absolute difference of 0.523 RGB levels, a 99th percentile of one level and no
pixels differing by more than eight levels. Evidence is in
`Automation/output/lighting-live/wicked-beam-clusters`.

Scheduling each probe hemisphere as an independent GPU thread is not used: measured tracing
increased from approximately 2.24 ms to at least 2.34 ms. The full build/native-suite log is in `Automation/output/lighting-live/wicked-beam-clusters`.


## Sampling and glass-material checks

The retained probe update uses 16 cosine samples per hemisphere and updates four slices. The ray budget remains 1,572,864 per frame and texture allocation
is unchanged. A complete volume sweep takes 16 frames. History coefficients are 0.1536 (steady) and 0.84
(authored edits). The GPU test checks the analytic unoccluded
sky integral, first-sweep publication, live hue changes, scrolling and shadow-tile reassignment.
The +Z hemisphere cosine integral error is about 0.000435.

Standalone Relight starts with skylight contribution 0.3. Explicit zero and Environment-authored ambient values
remain respected. This does not add transmission through opaque stained-glass panes.

At the same Inferno ground/facade poses, the retained pass traces in approximately 2.2 ms;
one eight-second facade sample measured 33.73 FPS. This is not evidence of an overall speed increase. Faint spatial GI patches remain visible.
Matching captures and settings are in
`Automation/output/lighting-live/wicked-angular-visibility/quadrature/`.

Two approaches are not used: a 4-by-4 octahedral distance field using the existing 48 rays
added wall-edge dark spots and extra memory/work, and a shared ray rotation across probes did
not resolve the patches. The six-band visibility and world-stable ray hash are retained.

A prototype using the installed `csgo_glass.vfx` shader and Valve's breakable-glass material
example compiled successfully, but the map loads its packaged opaque material, confirmed by the
original self-illumination parameters in the runtime log, including through the head-priority
override mount. Evidence is in `Automation/output/lighting-live/wicked-angular-visibility/glass-head`.
Pane transparency and diffuse coloured sky transmission still require a material/render-order
integration; they are not implemented.

Full verification: 60/61 native tests, with the existing `EnvironmentReceiverTests` failure;
432/456 JavaScript tests, with 24 failures. Logs are
`Automation/output/lighting-live/wicked-angular-visibility/full-native-tests.log` and
`Automation/output/lighting-live/wicked-angular-visibility/full-ui-tests.log`.


## Movement and lighting-edit response

The scheduler keeps the 16-ray-per-band quadrature. The background
phase follows world cells. A 16-cubed neighbourhood
refreshes each frame, and newly exposed scrolling probes are traced immediately. Unknown
probes and enclosed invalid probes use separate states, so enclosed cells do not retrace as
new cells every frame. Compact dispatches handle background and nearby work; only region
changes scan the whole grid for exposed probes. A camera cut initializes the full field and
may be more expensive than a normal frame.

Authored edits replace colour at updated probes and increase background work from four
to eight slices for 24 frames. The nearby box responds on the next update; the background
completes a sweep in eight frames during this interval. This does not make recursive bounce
instantaneous or establish an overall FPS improvement.

The sun PCSS filter always tests the central blocker and retains a minimum PCF footprint
when its sparse blocker search finds nothing. The disk orientation is fixed in light space, so world surfaces are not resampled as they move
across the screen.
A GPU test verifies the thin central blocker and the corresponding unobstructed case.

Live Inferno captures cover forward/backward movement through the alley, the barrel corner,
a red-to-blue sun edit, and a top-down view with building and tree shadows. They are saved in
`Automation/output/lighting-live/moving-gi-final/`. Tracing measured roughly 3.0–3.2 ms in the alley captures. These are not paired FPS results.
Broad checkerboard holes are absent from the inspected captures; faint spatial GI variation
and exhaustive distance/angle acceptance remain open.

The decorative brick wall also has actual openings in its render mesh. Double-sided casting
cannot fill missing triangles. Closed, localized backing geometry is the appropriate map
repair, while retaining doors, windows and intended gaps. Scene boxes currently
cast direct shadows but are not part of static probe voxelization, so claiming that they
repair diffuse sky/GI leakage would be incorrect.
## Probe execution optimization

VRF supplies map
geometry, materials and authored light definitions; our voxel/probe transport is a runtime
implementation, not the CS2 light-bake solver.

Wicked's [ray allocation shader](https://github.com/turanszkij/WickedEngine/blob/master/WickedEngine/shaders/ddgi_rayallocationCS.hlsl)
assigns work from probe inconsistency and retains reduced off-frustum work. Epic's
[Lumen performance guide](https://dev.epicgames.com/documentation/en-us/unreal-engine/lumen-performance-guide-for-unreal-engine)
describes update budgets and the quality cost of reducing probe and integration resolution.
Those are useful scheduling principles; neither makes our coarse voxel visibility equivalent
to triangle intersections or Lumen's scene representation.

The scheduler keeps 16 rays per hemisphere, six hemispheres, probe/voxel spacing, reflections,
authored fill lights and prioritized visible refresh. Current depth records the next frame's
receiver list; transport, composition and reflection normalization publish together before
materials draw, avoiding changes to the lighting field during native material consumption.
Each compute lane traces one
hemisphere. Background, nearby, exposed and compact
visible dispatches all cover the six bands. Native receiver queries use a point sampler
because the query already gathers and weights individual cage corners. Reflection cone
sampling retains linear filtering and mips. Debug camera luminance reuses a staging buffer
and polls with `DO_NOT_WAIT`; it does not wait for current-frame GPU work.

The current scheduler measurably reduces both background/nearby and visible tracing in the dense
Inferno street.

Matched 12-second forward/reverse runs, offline Inferno at 1600 by 1200, CT, no bots,
same street eye poses and scene settings, recording disabled:

| Metric | Value |
| --- | ---: |
| Average FPS, debug enabled | 37.06 |
| Mean frame time | 26.933 ms |
| 95th percentile frame time | 39.972 ms |
| Lowest sampled window FPS | 23.79 |
| Typical background/nearby trace | 1.33-1.35 ms |
| Typical visible trace | 0.81-0.99 ms |

A run with debug disabled averaged 36.66 FPS, with a 23.79 FPS minimum
sampled window and 39.739 ms 95th percentile frame time. These are local
measurements, not a map-wide 30 FPS minimum. Native material rendering, shadow maps and draw
hook CPU work remain substantial. Initialization and newly exposed regions can still spike.

Stationary native captures, continuous forward/reverse motion and a live red/blue sun edit
were inspected on the staged DLL. The stationary wall crops remain nearly static; railing
and room variation remains. The captures do not establish that the scheduler by itself fixes shimmer. Twenty-unit voxels and forty-unit probes still
lose thin blockers and directional visibility; dark pockets and full visual acceptance remain
open. Increasing ambient globally would conceal those errors and wash out enclosed rooms.

All six Relight native tests pass. GPU checks cover all six compact-dispatch hemispheres,
visible receiver dependencies, scrolling, newly exposed cells, lighting edits and the actual
production point-sampler binding. Native irradiance tests exercise the shipped bytecode
queries with that sampler. Ignored local evidence is in
`Automation/output/lighting-optimization/`: `baseline.json`, `parallel-ct.json`,
`parallel-clean.json`, `temporal-comparison.json`, `parallel-motion/`, and `parallel-edit/`.

## Fixture and shadow filtering

The decoder reads nested authored cone angles and analytic projector fades, uses the
native quaternion basis, and represents tube sources with their authored local +Y segment.
Rect/barn sizes are half extents; positive `size_params.z` is reciprocal projection distance.
Their expanding frusta drive direct shading, GI injection, shadow projection and light
binning. A shaped projector submits one shadow face/record.
Fractional barn roundness is retained as a continuous value.
The decode follows [VRF's SceneLight](https://github.com/ValveResourceFormat/ValveResourceFormat/blob/master/Renderer/Renderer/SceneEnvironment/SceneLight.cs).
Owned local shadow filtering uses the blocker-depth/source-size calculation described in
[NVIDIA's PCSS integration guide](https://developer.download.nvidia.com/assets/gamedev/docs/PCSS_Integration.pdf),
with a small fixed sample budget and bounded atlas reach. This remains a central-projection
approximation, not full area-source visibility or Unreal's area-light implementation.

High-resolution nearby shadow slots are stable across distance-ranking crossings and admit
at most one replacement per frame when a challenger is substantially closer. Cached preview
refresh covers 32 faces per frame; oldest-face reservations and export correctness are
retained. Selection and tile lookup are prepared once per frame, and repeated native buffer
descriptor reads are skipped on the material binding fast path.

Matched 12-second street runs use the same offline Inferno route, 1600 by 1200 resolution,
scene preset, CT and no bots, with recording disabled:

| Metric | Value |
| --- | ---: |
| Average FPS, debug enabled | 37.81 |
| Mean frame time | 26.424 ms |
| 95th percentile frame time | 35.011 ms |
| Lowest sampled window FPS | 27.67 |

A debug-disabled run averaged 37.15 FPS, with a 26.20 FPS minimum sampled window
and 35.424 ms p95. These are local measurements, not a map-wide 30 FPS minimum. Draw-hook
CPU time remains substantial; these changes do not establish a major reduction in that cost.

Native recordings cover fixtures 175, 31, 157, 164 and 149 at distances 64, 24 and 8 units,
plus continuous approaches and retreats at 175, 157 and 149. Runtime logs confirm the
nested authored values, including the 32.3/80.1-degree cone. The kitchen tube still shows
hard triangular shadow wedges. Raising softness to four did not establish a meaningful
fix; disabling shadows removes those wedges but also removes room occlusion. The exact
reported square-before-fade artifact was not reliably reproduced, and remains open.
Coarse voxel/probe visibility, dark pockets, thin railing variation and source-housing
visibility still require better representation rather than a global ambient increase.

All seven selected native lighting/environment tests pass. Native shader checks verify the
owned source load and five raw 2D depth fetches, and the patched shipped program renders on
hardware across the tested HDR formats and single-sample/4x MSAA targets. Ignored evidence
is in `Automation/output/lighting-optimization/`: `priority-before.json`,
`priority-wide-after.json`, `priority-wide-clean.json`, `wide-approach-*`, and `wide-control-*`.
The fractional-roundness build was staged, captured at fixtures 24 and 175, and
rechecked on the same street route: 38.49 FPS, 35.610 ms p95 and a 27.72 FPS minimum sampled
window. Evidence is in `priority-final-confirm.json` and `final-approach-*`. Test settings:
baseline player pose and preset, GI/shadows enabled, recording stopped.

## Stationary flicker correction

Held-camera stability depends on two rules: posed actors and world weapons stay out of the coarse
20-unit indirect cells, and recursive shadow-fed solves are held while the camera and authored
lighting are unchanged.

Indirect occupancy uses the static map. The solver retains a settled irradiance, visibility
and reflection snapshot after 48 unchanged frames. Camera movement, region changes, light edits,
shadow-record reassignment and display controls invalidate it. Current depth collects priorities
for the next solve; all material draws consume the same published field. Actors receive GI and
keep direct shadows, but their geometry does not block or colour indirect transport, and
animated caster changes do not update held indirect lighting or reflections.

Two 120-frame native preview bursts at the Inferno camera, with normal game time,
the pistol visible, GI/reflections/sun shadows/AO enabled and diagnostics off, confirmed
this behaviour. Sampling 48 frames per burst, the static patches measured:

| Patch | p95 RGB range |
|---|---:|
| Red-building window | 0 |
| Gun interior | 1 |
| Upper marked shutter/wall | 1 |
| Left marked ground | 0 |
| Right marked wall | 1 |

These are fixed-pixel interior measurements, not motion-compensated weapon-wide statistics.
Moving weapon edges, foliage, direct shadows and clouds remain visible in the heatmaps.
Runtime logs confirmed reuse at 48 settled frames, active solving during a forward/reverse
camera route and successful bounce-strength edits. GPU regressions cover ignored actor occupancy,
immutable fields during receiver collection, cache reuse and lighting/display invalidation.
The staged DLL was relaunched and verified in offline CS2. Initial convergence and voxel aliasing
during movement remain limitations; this targets the stationary flicker and does not
claim triangle-level transport or a replacement of the whole lighting architecture.

## Denoiser integration requirements

[NVIDIA NRD](https://github.com/NVIDIA-RTX/NRD) supports D3D11 and offers REBLUR/RELAX lighting
and SIGMA shadow denoisers. It requires guide data such as view depth, normals, roughness and
motion vectors, together with the expected radiance/hit-distance encoding. Our current D3D11
path does not expose verified object motion vectors for first-person weapons. Camera-only
reprojection would not correctly follow the moving viewmodel. NRD is therefore a candidate
for a deliberate integration, not an enabled feature or a remedy that can simply be toggled.
Review its [SDK license](https://github.com/NVIDIA-RTX/NRD/blob/master/LICENSE.txt) before redistribution.
A lighting denoiser must operate on owned lighting before native material shading/tonemapping;
blurring the final game frame would also blur textures and the weapon.

## Apartment regression correction

Omni emission is a full sphere, so the fan lamp's ceiling illumination and moving blade
shadows are retained; spot cones and
rect/barn projections remain directed. Separate front/back voxel radiance also prevents
exterior sky from entering the sealed room through an excluded backface sample.

Adjacent cube-shadow faces overlap and use smooth
weights normalized across the three cube axes. Two faces sum to one at an edge and three
sum to one at a corner. The atlas mapping compensates for the wider culling projection,
and the filter guard keeps taps inside their own tile.

Matching native captures verify both wall lamps and moving ceiling shadows. The ceiling
RGB temporal range is p95=64 while the static-wall control remains
p95=0, max=1. The wall-lamp face boundary's mean luminance jump is
-0.72 code values. Authored light position, colour and intensity are retained, with GI,
reflections and shadows enabled.

The [saved fixtures and metrics](../../Automation/fixtures/relight-regressions/README.md)
include the [lamp comparison](images/lighting-lamp-before-after.png) and
[fan comparison](images/lighting-fan-before-after.png). An uncaptured eight-second
apartment run averaged 42.57 FPS with 25.64 ms p95 and no sampled frames above 33 ms.
This local result does not establish a map-wide minimum or full baked-lighting parity.

## Hallway and doorway

Inferno's stair fill (118) and window projector (115) have zero authored edge softness.
A minimum 15 percent analytic feather softens the fill boundary; stronger authored
softness remains. Projector filtering uses the decoded physical source radius instead
of the full emitter dimension. Matching captures show the [stair fill fading](images/lighting-fill-feather-before-after.png)
and [window crossbars](images/lighting-window-before-after.png). The distant stair
view was also checked with the fill enabled.

Nearby visible faces refresh every frame, with a
128-unit allowance outside the light range and at least 16 oldest background faces retained.
The [consecutive-frame differences](images/lighting-doorway-shadow-cadence.png) exclude
the fan mesh. Runtime logs confirmed zero frame age for these nearby faces.

Overlapping cube-shadow faces contribute to GI with injection weights that sum to one at seams,
matching native direct-shadow blending.
Reflection captures retain their point during travel below 40 units and blend both radiance
and ambient normalization with 80 percent history. Lighting/display edits and large cuts
bypass that history. A matching 20-unit hallway step kept the capture normalization at
0.0641 without resampling a different point.

A fixed-camera test moved the physical player from eye `[1265.808228, -150.775146, 324]`
to `[1290, -82, 324]`. The [player's window shadow](images/lighting-window-player-motion.png)
changed locally while three other wall/ceiling regions retained p95 RGB change of zero;
their mean luminance changed by less than 0.001 code value. This verifies that test without
claiming that every view or lighting transition is covered.

An uncaptured eight-second matching hallway walk averaged 36.95 FPS, with a p95 frame time of
37.502 ms. The quality correction does not establish an overall frame-rate change. Five targeted native suites passed, including production HLSL cube
energy and reflection capture tests. [Camera fixtures, controls and measurements](../../Automation/fixtures/relight-regressions/README.md)
are retained. The reflection is still one coarse voxel cube; accurately reflected geometry
and local parallax require further work.
