# Interior lighting verification

Lighting captures must come from inside the playable building. A camera in a wall,
above the ceiling, below the floor or outside the map is rejected. Collision bounds
alone are not enough, because they also contain outdoor space.

## Launch and readiness

Build and install both Source 2 components using [the build instructions](../../CLAUDE.md).
Launch `Automation/launch/launch-cs2-netcon.ps1` as the signed-in Windows user, with
`-insecure`; builds, tests and netcon can remain sandboxed. Close CS2 with `quit`.

1. Stop `mvm_debug` before changing maps.
2. Load `map <name>` and wait for both the environment's current map and engine
   `status` reporting `@ Current : game`. The hook can publish its map while the
   client is still in `levelload`, compiling shaders.
3. Repeat `jointeam 3 1` until `getpos` reports a spawned, nonzero player position.
4. Run `exec dev`, let its round restart complete, then enable noclip and start
   `mvm_debug` before reproducing the lighting issue.

Running `exec dev` during `levelload` can crash in `server.dll` while resetting an
unassigned player. Wait for the game client and a spawned player before running it.
This readiness step does not establish that every possible server crash is avoided.

## Camera preflight and capture

[`interior-camera.py`](../../Automation/verify/interior-camera.py) reads the native
`MVMOCC01` collision cache selected by the current map manifest. It requires:

- Eye position strictly within collision bounds.
- A walkable, upward-facing floor 64–80 units beneath the eye.
- A downward-facing ceiling within 384 units, with at least 20 units of head clearance.
- Floor and ceiling coverage at all four corners of the player footprint.
- Building walls in at least six of eight horizontal directions within 512 units.
- At least 20 units of horizontal clearance around the eye and lower player body.

The check is a geometric preflight. Glass, thin visual details, doors, overhangs and
collision omissions still require visual inspection. A capture remains
`visualReview: pending` until its pixels have been inspected.

```powershell
python Automation/verify/interior-camera.py cs_office --eye 1552.2 560 -94
python Automation/verify/interior-camera.py cs_office --plan Automation/output/office-poses.json
```

The planner searches beside authored lights and places the eye 66 units above
the floor. It also requires floor and roof support 64 units into every
surrounding direction. It skips lights for which no checked interior pose exists.
`setpos` places the player's feet, while `getpos` reports the eye in these test
sessions. Subtract 64 from the planned eye Z when sending `setpos`; compare the
actual `getpos` eye with the plan and run preflight again before recording.
The sweep and audit also check position and angles before and after each
capture, rejecting movement or a turned view.

Record using engine-native `mirv_streams`, through the shared `Capture-Frame`
helper. Desktop-window capture can select a sizing popup instead of the game,
and a sandbox desktop can report no visible game window. Neither is valid evidence.

## Map sweep and authored-light audit

[`verify-interior-lighting.ps1`](../../Automation/verify/verify-interior-lighting.ps1)
loads Office first, then Cache, Anubis, Inferno, Mirage, Dust2, Nuke, Ancient,
Train, Vertigo, Overpass, Boulder, Fachwerk, Shelter and Italy. The actual map
names for the latter additions are `de_boulder`, `de_fachwerk`, `cs_shelter`,
and `cs_italy`.

```powershell
& Automation/verify/verify-interior-lighting.ps1
& Automation/verify/verify-interior-lighting.ps1 -Maps cs_office,de_cache -Rooms 3 -PlanLimit 24
& Automation/verify/verify-interior-lighting.ps1 -OutputDir Automation/output/interior-lighting-loop -PlanLimit 40 -Resume
& Automation/verify/audit-map-lights.ps1 -Map cs_office -Skip 7 -Limit 3 -ToggleShadows -CompareBaked
```

The sweep samples two separated interior poses per map by default. Each records
live relighting, GI-off and native/baked lighting from the same actual camera.
This is representative interior coverage, not an exhaustive audit of every room
or light. Increase `-PlanLimit` or use the authored-light audit for broader coverage.
`-Resume` retains maps with the requested number of completed captures and
restarts an incomplete map. Visual review is still required for retained captures.

The audit groups lights within eight units into a stop, selects a collision-checked
camera, and optionally compares shadows and baked lighting. It saves `index.json`
after each stop, preserving completed evidence if a later operation fails. The
HTML revisit command uses feet Z; the stored `camera` value remains eye Z.

Output lives under ignored `Automation/output/`. Preserve `results.json`, the
pose plan (including `physicsDigest`), inspected screenshots and the matching
`mvm_debug` log together when retaining regression evidence. Record rejected poses
separately and never count them as lighting failures or successes.

## GI regression and current findings

The screen compute query and injected material `WorldQuery` use the same separable
quadratic B-spline over three neighboring probes per axis. At a probe-row plane,
neighboring support remains nonzero; when the nearest-probe index changes, the
outgoing row reaches zero weight continuously. The continuous kernel avoids periodic
discontinuities with nonuniform light.
Receiver-to-probe distances and visibility still use the exact receiver location.
Blocked cages stay dark; weak validity still fades continuously.

The 27 visibility queries execute in one shader loop body. Unrolling them caused
expensive native shader compilation on map reload. Wider support increases GPU
query work over an eight-corner query; performance and
thin-wall leakage require inspection alongside brightness.

`RelightProbeGpuTests` covers a dead row, three intersecting dead rows with one
visible corner, a completely blocked cage, and continuity with nonuniform colors. `RelightIrradianceTests` executes
the same cases through the injected bytecode and validates all 28 shipped world
programs on hardware. The dense real-geometry wall scan rejects dips below 85%
of their local mean when the optional Inferno cache fixture is available.

Verification: all 66 native tests and all 454 JS tests pass, plus 28/28 shipped world material programs on hardware.

Office findings remain scoped to the inspected view:

| View | Finding |
| --- | --- |
| Painted wall, eye `(60, -70, -94)`, angles `(25, 180)` | Floor 66 units below, ceiling 62 above; smooth wall base in the quadratic-kernel capture. Bright trim seams remain. |
| Reception, eye `(1552.2, 560, -94)` | Enclosed, collision-checked camera. Periodic floor banding is absent with the continuous kernel. Bright base-trim seams and ceiling-edge points remain; disabling sunlight removes them, while disabling GI, reflections, softness or receiver bias does not. |
| Side Hall, eye `(690.9, 411.9, -94)` | Inspected wall bases are smooth with GI enabled. Thin bright seams remain at some wall joins. This view does not prove the marked wall is fixed. |
| Eye `(1587.6, 631.7, -74.2)` | Rejected by the wall-clearance check; excluded from acceptance evidence. |
| Lobby eye above ceiling | Rejected; excluded from acceptance evidence. |

Keep ceiling dots, thin-wall bleed, wall-join leaks and authored audit stop 008
open until the corresponding checked view and discriminating toggles establish
the cause and verify the correction. A successful build or uniform test cage
does not close a live lighting issue.

## Fifteen-map review

Every listed map was loaded and captured. This table records representative
findings from the sweep, not a map-wide lighting pass. Capture rejection
requires a replacement building-interior view before that map has accepted
interior coverage. Results and rejected views are retained separately in each
run's `visual-review.json`.

| Map | Inspected coverage / unresolved finding |
| --- | --- |
| Office | Enclosed service room and hall; ceiling-edge marks and floor bands. The painted wall is rechecked separately. |
| Cache | Enclosed pipe room; severe overbrightness also present with GI off. Entrance-facing view rejected. |
| Anubis | Enclosed gated corridor accepted; second dark view lacks enough spatial context and was rejected. |
| Inferno | Covered arch and scaffold exterior views rejected; replacement enclosed room required. |
| Mirage | Enclosed construction room and couch room; broad dark bands in construction room. |
| Dust2 | Entrance-facing views rejected; replacement enclosed room required. |
| Nuke | Enclosed ramp and utility corridor; dark floor under ceiling panels. |
| Ancient | Enclosed altar room accepted; open-roof ruin view rejected. |
| Train | Two enclosed corridor views; floor and upper-wall bands need isolation. |
| Vertigo | Inward-facing construction interior accepted; exterior-edge view rejected. |
| Overpass | Enclosed underground utility room and tiled hall; floor patches and field seams. |
| Boulder | Enclosed vaulted cellar and cellar entry; strong directional shafts need source isolation. |
| Fachwerk | Covered timber passage and street-facing exit rejected; replacement enclosed room required. |
| Shelter | Enclosed storage room and hallway; floor/wall bands and doorway sunlight need isolation. |
| Italy | Enclosed stone storage room confirmed using the baked view; severe underlighting. Tree courtyard rejected. |

Evidence folders: `Automation/output/interior-lighting-matrix` and
`Automation/output/interior-lighting-deep`. Kernel verification is stored
separately from these findings.

## Loop-kernel sweep and reviewed route

The loop-kernel sweep loads all fifteen maps and captures two candidate
poses per map. Visual inspection rejects exterior-facing views in Anubis,
Mirage, Dust2 and Vertigo and both covered street views in Fachwerk. The remaining
views give accepted building-interior coverage for fourteen maps. Fachwerk's
accepted view at eye `(93.219, -163.423, 162)`, angles `(12, 180)`, is an enclosed
living room with verified floor, ceiling and walls. Its covered gallery alternative
is rejected. Each of the fifteen maps has at least one accepted interior
view; this does not establish a map-wide lighting pass.

The repeatable camera route is
[`environment-interior-poses.json`](../../Automation/fixtures/environment-interior-poses.json).
The competitive environment verifier uses it by default, checks its visual approval
and collision digest, and revalidates the actual camera. A changed collision digest
requires regenerating and visually reviewing the route. Evidence references point
to ignored local output folders; the pose and digest are the committed fixture.

Metadata replacement can reset relighting during a map transition.

Representative findings:

| Maps | Result / open finding |
| --- | --- |
| Office, Inferno, Dust2, Ancient | Inspected wall bases lack periodic GI slashes. Office trim-edge sunlight seams remain. |
| Cache | Enclosed warehouse views remain severely overbright, including GI-off comparison. |
| Anubis | Enclosed stone stair room accepted; dark water/background artifact through the arch remains. |
| Mirage | Enclosed construction corridor accepted; upper-wall lighting fields need isolation. |
| Nuke | Enclosed ramp and utility corridor accepted; utility floor remains very dark. |
| Train, Vertigo | Roofed corridor / inward construction room accepted; broad lighting changes remain. |
| Overpass | Enclosed utility room and tiled hall accepted; dark floor patches remain. |
| Boulder, Fachwerk | Vaulted cellar / enclosed living room accepted; strong directional shafts and shadows need isolation. |
| Shelter | Storage and hallway floors; wall shadow bands and doorway triangle remain. |
| Italy | Both stone stair/storage views accepted using baked context; severe underlighting remains. |

Evidence: `Automation/output/interior-lighting-loop/visual-review.json`,
`Automation/output/fachwerk-verified-interiors/visual-review.json`, and
`Automation/output/office-source-isolation/`. The `r_multisample_antialiasing`
console toggle is an unknown command and is not MSAA evidence.

### Office authored stop 008

The planner's preference for 64-unit floor/roof support rejects this compact
service room. A separately inspected pose is supplied explicitly:

```powershell
& Automation/verify/audit-map-lights.ps1 -Map cs_office -Skip 7 -Limit 1 `
  -PosePlan Automation/fixtures/office-stop008-interior.json -ToggleShadows -CompareBaked
```

The plan must match the current collision digest, and actual position/angles and
player footprint are still checked. The audit preserves authored numbering after
`-Skip`, so this stop remains `008-barn`. Its room is enclosed. The audit disables both
systems for the baked phase and records actual phase state. The confirmed audit in
`Automation/output/office-stop008-confirmed` finds mean RGB 9.22 in live lighting
versus 68.05 in native lighting from the same checked interior camera. Turning
all local shadows off adds broad light across the room (mean absolute RGB change
115.79); it does not isolate this fixture and cannot establish a self-shadow cause.
This room's brightness
correction remains open.


## Sun contact and MSAA receiver

Sunlight reconstructs world position at the selected
MSAA depth sample's actual offset, including the far sample used for silhouette
validation. Opaque back-facing receiver planes block sunlight. Front-facing
receiver-plane comparisons retain their true grazing slope instead of clamping
the cosine to 0.15. When the physical contact penumbra is smaller than a shadow
texel, the filter retains covered contact texels and smoothly transitions to the
soft disk as the penumbra grows. This avoids borrowing sunlight from a nearby
opening merely because the minimum PCF disk crosses a trim edge.

GPU regressions cover both depth orders on an analytic MSAA plane, back-facing
opaque receivers, an opaque subtexel corner beside an opening, mixed near/distant
blockers, fully covered ambiguous MSAA edges, differently lit surfaces, clear sky,
and five lit receiver slopes including grazing angles. The full native suite passes 66/66, and the JS
suite passes 454/454.

Evidence: `Automation/output/office-contact-final`. Checked Office comparisons are in
`Automation/output/office-covered-msaa`.
The painted wall's continuous white base-trim seam is absent in the inspected view; periodic GI
slashes remain absent. Reception's repeated ceiling-edge points and dotted
base-trim segment are absent in the inspected view. One isolated, weaker trim
pixel remains in the painted wall's sunlight comparison; this is not a map-wide
thin-edge lighting pass. All comparison variants retain the same validated
interior position and angles and verify their requested relighting/GI state.

The remaining painted-wall pixels come from averaging nearby and distant
blockers in the same sun cone. The contact footprint caps the penumbra using
its closest covered blocker.
Ambiguous opaque pixels retain a shared zero only if every MSAA sample is
independently shadowed using its own reconstructed position and geometric normal.
A clear sample or differently lit receiver retains the native fallback. This
adds per-sample work on ambiguous edges; performance remains a verification item.

The interior-only sweep can reuse the visually accepted route rather than
selecting new candidates:

```powershell
& Automation/verify/verify-interior-lighting.ps1 -UseReviewedRoute -Rooms 1 `
  -OutputDir Automation/output/interior-lighting-confirmed
```

The route includes one accepted building interior per map and must match the
current collision digest. Recorded pitch and yaw are preserved. Geometry and
actual pose checks still run before and after every capture. A successful route
capture establishes inspected interior coverage, not a map-wide lighting pass.


Capture transport retries a temporarily unavailable listener only before sending
commands. Recording keeps its connection open until the engine acknowledges
start/end, which holds during shader compilation.
An incomplete take is rejected rather than counted as a lighting result. Resume
replaces the incomplete map's comparison set.


## Relighting state at capture

Camera validity alone cannot establish a lighting comparison. The interior sweep
sets the requested relighting phase after map metadata and the camera are ready,
then verifies map identity, loaded lights, enabled state and GI before and after
each capture. Live requires enabled/GI = 1/1, GI-off requires 1/0, and baked
requires enabled = 0. The actual states and collision digest are saved in
`results.json`. Unexpected state rejects the capture. The
`interior-lighting-final` captures are camera evidence only; lighting acceptance uses
`interior-lighting-confirmed`.


The sweep binds the environment to the current map and waits for matching authored
lighting in the running game, not just cache files on disk. A previous map's scene
binding or light metadata rejects capture. Resume retains only complete phase
states against the current collision digest. One-camera preflight builds geometry
bins only for its full ray-support envelope; AABBs intersecting that envelope are
kept even when all triangle vertices are outside it. This reduces memory for large
maps without weakening the floor, roof, wall or player-footprint checks. Eight
Python tests include equivalent focused/full checks and spanning triangles.


## World shadows during map preparation

With the environment enabled on Boulder and every shadow switch on, `mirv_relight status`
can still name Overpass and report it disabled while the runtime map-data load has not
published Boulder geometry or authored lighting. A ready disk cache
alone does not establish runtime readiness. Reloading geometry rebuilds the cache
and publishes 234 Boulder lights. The checked cellar camera shows fixture
lighting and shadows after the reload. Evidence is in
`Automation/output/boulder-shadow-readiness` (`before`, `native`, `reloaded`).
The sweep's debug log does not capture the background-load failure's loader exception.

Map lighting requires matching current-map authored metadata and ready geometry before
the native lighting takeover or replacement-material lighting can run. Pending,
failed, and previous-map data retain native lighting and shadows. The environment
status also reports runtime map-data readiness and its loader message. Five
regression checks cover missing geometry, missing/previous-map metadata, a ready
map, and no active map. The sweep reports loader failure directly instead of
waiting on a stale light list.


## Confirmed interior matrix

`Automation/output/interior-lighting-confirmed` contains all 15 distinct maps,
one reviewed building interior each, and 45 live/GI-off/native comparison frames.
Every comparison verifies actual pose before/after, current-map metadata,
environment binding, requested enabled/GI state, and collision digest. All
15 camera views passed geometry checks and visual review. Capture replies discard console backlog and wait for
fresh camera/recording acknowledgements.

These are inspected interiors, not a claim that all map lighting is corrected.
Remaining observations are recorded in `results.json` and `visual-review.json`:

| Map | Inspected interior observation |
| --- | --- |
| cs_office | Enclosed office hall; smooth floor field, small ceiling-edge highlights remain; darker than native. |
| de_cache | Roofed warehouse; geometry shadows visible, broad overbright fill remains. |
| de_anubis | Enclosed stone stair chamber; warm fixture response visible, dark sky through the arch remains. |
| de_inferno | Enclosed store room with covered ceiling; sunlight shadows visible, broad underlighting remains. |
| de_mirage | Enclosed construction corridor; broad upper-wall light fields remain. |
| de_dust2 | Inside roofed storage room and fully behind entrance; strongly underlit versus baked. |
| de_nuke | Inside roofed utility/ramp room; cool dark floor remains. |
| de_ancient | Inside enclosed altar room. Wall bases smooth in this view; broad dark floor regions remain. |
| de_train | Inside roofed glazed corridor with full floor/ceiling support; rear corridor remains dark. |
| de_vertigo | Inside roofed construction room facing inward; exterior-edge candidates excluded. |
| de_overpass | Enclosed storage/service room; very dark live floor and lamps compared with native. |
| de_boulder | Enclosed barrel cellar; warm fixture response and stair/barrel shadows visible with matching map data. |
| de_fachwerk | Enclosed living room; fixture shadows and sun/geometry shadows visible, broad sunlight overbrightness remains. |
| cs_shelter | Enclosed roofed storage; fixture floor lighting and occluded corners visible; darker than native. |
| cs_italy | Enclosed stone stairs/storage, confirmed in native view; severe underlighting remains. |

Readiness build: 66/66 native tests, 454/454 JavaScript tests, and 8/8
interior-camera Python tests pass.


## Boulder wall coverage

Official community maps keep custom wall materials separately in
`game/csgo_community_addons/<map>/<map>_dir.vpk`; their map package contains world
geometry but not these materials. Extraction mounts the selected map's assets,
tracks the directory package and numbered archives for cache freshness, and resolves
referenced skybox packages through the mounted resources. Extractor version 2 rebuilds
incomplete caches. Missing caster materials produce warnings.

Complete Boulder coverage also requires the larger shared visual-payload limits and
rigid-material batch limits. The rebuilt cache has 20,141,332 render triangles in 571
batches, including 82 cutout batches, one stained-glass batch and the 3D skybox, with no
extraction warnings. Opaque walls remain actual render geometry; collision hulls are
not substituted for them.

Verified camera: eye `(-240,1440,146)`, angles `(25,15,0)`, sun elevation 35 degrees and
azimuth 23 degrees. The camera has a floor 66 units below, roof 179.781 units above,
full supported footprint, six enclosed directions and no wall within 24 units. The
camera remains at that interior position for the doorway view `(20,170,0)`.

Rigid casters intercept four chapel rays at the same distances as the wall collision: 148.535,
216.040 and 169.754 units (the last distance occurs twice). Live captures
show the checkerboard floor and curtain continuously shaded, with no bright holes.
The doorway sun-on/sun-off comparison retains sunlight and the long solid building
shadow in the courtyard while the chapel stays shaded.

Evidence is under `Automation/output/boulder-wall-fixed/`: checked interior, sun-off
control, restored final state and doorway comparisons. Runtime logs confirm current
map data, 234 local lights, 82 cutout batches and all 16,473,981 rigid triangles. The
final state restores sun scale 1, all cascades, shadows and GI enabled, and the
interior view.

Map and blob access to the particle-shadow shader cache, used on engine shader-creation workers,
is protected by a mutex; eight concurrent cold shader observers all receive valid capture tags in the GPU test.
Affected map, geometry, particle and animated-caster native tests pass; Inferno passes all 43 parity checks
against its previous extraction. Broader brightness differences from the
15-map inspection above remain separate from this wall-coverage fix.

## Boulder ground patches

The stationary dark patches on the courtyard gravel were sun self-shadowing.
From a checked interior doorway camera, they persisted with local lights, GI, AO
and reflections disabled, and disappeared when shadow rendering was disabled.

The production PCSS pass caps the receiver-plane tolerance at half a texel, still limited by the
requested normalized bias. This keeps curved facets within the bilinear footprint from shadowing their own surface.

The GPU regression reproduces the self-shadowing on a convex receiver, verifies
the receiver remains lit, and retains a separate narrow blocker.
Existing sloped planes, contact corners and mixed or covered MSAA edges pass.

Live evidence: `Automation/output/boulder-static-patches/`. Eye
`(-1240,1280,114)`, angles `(18,45,0)`, is fully inside the red-tiled room, looking
through its doorway. Sun-direction comparisons retain moving building shadows.
The chapel camera `(-240,1440,146)`, angles `(25,15,0)`,
remains shaded at sun elevation 35 and azimuth 23.
