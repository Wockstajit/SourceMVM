# Whole-map relighting performance

Measurements of [whole-map relighting](whole-map-relighting.md) and the speed changes made from
them. Each change names the `mirv_relight` switch that restores the earlier path in the same
process, so it can be timed and compared again. How the lighting works is in the main document.

## Movement profiling and runtime optimizations

Native lighting constant-buffer clones are reused by byte width, and one persistent irradiance
buffer is updated when its packing changes. Engine buffer contents are still
copied for each draw; buffer identity is not treated as proof that the contents are unchanged.
Device teardown releases the clone pool. AO, screen GI and sun visibility use optimized SM5
compilation. Probe hemispheres and the fixed PCSS disk retain their sample counts and spatial
patterns, with repeated trigonometry moved out of the per-ray/per-pixel work.

Half-resolution GI upsampling predicts neighbour depth on the receiver's projected plane.
This prevents a sloped floor from rejecting its own GI samples merely because its view depth
changes across a two-pixel footprint. Foreground/background depth rejection remains active.
The material still evaluates all six ambient-cube bands with its native normal map. This does
not add temporal accumulation or reduce the GI refresh rate.

`mirv_filmmaker health resetTiming` clears a 4,096-sample ring of Present intervals.
Subsequent health output reports mean, p50, p95, p99, maximum frame time and the number above
33.33 ms. These are whole-frame CPU wall-clock intervals, not isolated GPU pass times.
The low-frequency health command copies the ring under a lock and sorts outside it.

[`profile-relight-motion.py`](../../Automation/verify/profile-relight-motion.py) drives a
forward/reverse camera interpolation and records heartbeat-derived FPS and frame-time tails
without recording screenshots during measurement. Run in an already joined offline map:

```text
python Automation/verify/profile-relight-motion.py --routes routes.json --output result.json --seconds 10
```

The routes JSON is an array of objects with `name`, `start` and `finish`; each pose is
`[x, y, z, pitch, yaw, roll]`. Camera sensitivities, offset mode, pose and debug-log state are
preserved. The script leaves the free-camera input active; use `mirv_input end` to resume player
control. It rejects the zero player pose and scenes without meaningful draw activity so a
team-selection/menu frame rate cannot be presented as a lighting benchmark. Camera cuts and
initial region allocation settle before timing. Run captures separately; BMP writes, debug GPU
readback, simultaneous shader tests and builds invalidate a comparable performance run.

**The 30 FPS minimum is not achieved.** Shadow dimensions, filter sample counts and GI ray
counts are kept. Measurements are under the ignored `Automation/output/lighting-performance/`
directory.

Cluster assignment clips light hulls against positive camera W before projecting their
tile bounds. A light crossing the camera plane does not automatically occupy every screen
tile; conservative edge intersections retain its visible coverage. CPU and GPU regressions
cover crossing and fully behind-camera hulls. AO uses fixed octagonal horizon directions
A translated straight-edge GPU
regression covers the fixed directions.

The default remains one bounce propagation step per frame. The 30 FPS minimum remains unmet.
Extra propagation steps do not establish that shimmer is removed. Top-down captures retain
map-cast shadows.

Daylight weapon checks use the saved street finish pose, sun azimuth 0/elevation 60,
viewmodel enabled, and actual forward/backward player movement (`mirv_input end`). It records
99 lossless 1600 x 1200 BMP frames and a 24-frame full-resolution PNG review under
`Automation/output/lighting-performance/weapon/sunlight-final/`. Direct sunlight reaches the
pistol and hands; separate top-down captures show map shadows. The thin moving weapon rim
remains unresolved. Reflection-off and shadow-off diagnostics did not remove it by themselves;
Remaining GI variation and distant lighting acceptance are open.

Verification: 60/61 full native tests pass, with the existing `EnvironmentReceiverTests`
failure. The perspective-floor and background-depth GPU regressions pass, as do the
PCSS sampling math tests. Full JavaScript verification is 438/456; 18 failures remain outside
this change. The environment mock supplies its scene-key dependency (8/8 pass).

## Draw setup and shadow draws on the render thread

Measured on 2026-11-11 (RTX 5070, CS2 1.41.9.6, offline maps, fixed `mirv_input` camera).

At the Inferno CT spawn, 1920x1080 with 4x MSAA and only the map's lamps (sky and sun off), a frame
took 3.7 ms with Environment off and 11.7 ms with Dynamic lighting on. A flat shader in place of the
lit programs (`bindingcheck 1`) made that frame slower, not faster, so the GPU was not the limit.
[`SampleThreadStacks`](../../Automation/tools/sample-thread-stacks.cpp) (see
[GPU pass profile](gpu-pass-profile.md#which-functions-the-render-thread-is-in)) showed CS2's render
thread busy 97% of the time, half of it in the setup of lit draws:

| Share of the render thread | Where |
| ---: | --- |
| 23% | preparing the lights for a view (`ProofResources`): the map shadow pass 11%, building the light records 8%, the rebase and cluster command list 2.5% |
| 13% | binding a lit draw's textures (6%) and putting the engine's back (7%) |
| 7% | choosing the shared lighting constants |
| 7% | the dynamic irradiance program and its inputs, the AO view |
| 11% | re-issuing animated casters for the stream-out capture |
| 16% | inside Present |

A lit draw made about 75 device calls around the engine's one, 700 times a frame, and each view of
a frame (nine at that pose) rebuilt all 232 light records.

Four changes followed. Each keeps the picture, and each has a transient switch (default 1) that
restores the previous path in the same process:

- `mirv_relight recordreuse 0|1`. The frame's light records are built once for the world passes and
  once for the first-person pass (which scales effect lights), not for every view. They depend on
  the lights, the shadow tiles and their matrices, which one frame shares. Stained glass beams and
  their shadow maps are part of that list, so they are also prepared once.
- `mirv_relight stickydraws 0|1`. A lit draw leaves its inputs bound, and the next draw of the same
  material program takes them without a device call
  ([`RenderPixelStateLayer.h`](../../AfxHookSource2/Filmmaker/Render/RenderPixelStateLayer.h)). CS2
  submits lit draws in runs of one program, and between two draws of a run it changes only material
  textures and per-object constants. While the inputs are held, the layer keeps the engine's
  bindings beside ours: the pixel-stage `Get` calls report the engine's, a `Set` on a held slot
  updates the engine's side and leaves ours bound, and anything the lighting decision depends on
  ends the hold and restores every engine binding first: another program, vertex shader, render
  targets, blend state or viewport, another view or lighting constant buffer, a geometry, hull or
  domain shader, a command list, `ClearState`, the UI boundary and the present. A draw that copies
  engine constants (the unshared lighting or view fallback) or is being timed for the pass profile
  restores as before. So does a draw that got a "not yet this frame" answer for ambient occlusion
  or sun visibility (both wait for the depth prepass): every draw until then is set up as before,
  and the first one that can prepare them does. During a draw that reuses held inputs the layer
  steps aside, so code inside that draw's detour (the shadow caster capture, the smoke and sky
  scopes) sees our bindings, as it does in a draw set up for itself. With the engine's program
  visible there instead, the capture accepted geometry it had always rejected, and lamp shadows
  darkened a floor on Ancient. `mvm_debug` logs `render.pixelstate` every three seconds: draws set
  up, draws reused, and what ended the holds; nearly every hold ends with a program change. About
  65% of lit draws are reused at the Inferno interior pose and 39% at Ancient's. The
  layer needs its device-context detours and stays off without them (the renderer tests).
  `RenderPixelStateLayerTests` runs it on a WARP context: held bindings, reads, sets over held
  slots, every ending, a reused draw in progress and reference counts.
- `mirv_relight drawruns 0|1`. Shadow geometry is partitioned into spatial cells that lie end to
  end in the index buffer, and each visible cell was one draw call. Cells that follow one another
  now draw in one call (`DrawShadowRuns` in
  [`EnvironmentShadowSpatialMath.h`](../../AfxHookSource2/Filmmaker/Environment/EnvironmentShadowSpatialMath.h)):
  the same triangles in the same order. Wind-animated foliage is redrawn into the sun cascades and
  lamp faces every frame, so this is most of a frame's shadow draws: 54,000 draws a frame became
  16,000 at the Ancient interior pose. `EnvironmentMathTests` checks that the joined draws submit
  the same indices in the same order.
- `mirv_relight sunsummary 0|1`. The sun penumbra pass reads up to 157 shadow-map texels a pixel
  (25 for the blocker search, 4 for contact, 128 for the filter). A summary of the four sun cascade
  tiles (nearest and farthest stored depth of 4 to 2048 texel blocks, the lamp tiles'
  [`ShadowPyramidBuilder`](../../AfxHookSource2/Filmmaker/Environment/EnvironmentShadowPyramid.h),
  rebuilt for the cascades each map pass redraws: `shadows: sun summary`, about 0.13 ms) lets the
  pass return early where the answer is certain
  ([`EnvironmentSunVisibilityShaders.h`](../../AfxHookSource2/Filmmaker/Environment/EnvironmentSunVisibilityShaders.h)):
  - no stored depth in front of any tap of the search disc: lit. If something is in front, the
    search can still only count a blocker inside the cone the sun subtends, so the test repeats on
    the inner disc that the farthest such blocker's penumbra reaches;
  - a contact penumbra under a quarter texel: the four contact texels are the whole answer;
  - every texel the filter can read in front of every tap, and the contact texel covered: shadowed.

  The compare depth of every tap lies on one plane (the receiver plane at each texel centre), so its
  range over a footprint comes from the footprint's corners. A footprint a tap would be clamped out
  of at the tile edge, or one wider than a tile, takes the full path. Results are bit-identical:
  `RelightProbeGpuTests` compares 36,864 receivers a case with and without the summary (flat and
  sloped receivers, a wide and the real sun with the production search reach, a point sun, a NaN
  texel) on WARP and with `--hardware`. The summary is not offered while the first-person pass has
  its own close cascade in the atlas. At the Inferno spawn the pass fell from about 2.0 to 1.05 ms.

[`profile-lighting-switches.py`](../../Automation/verify/profile-lighting-switches.py) alternates each
switch off and on at fixed poses, two rounds of five seconds a side, and with `--capture` compares
window captures. 2560x1440, 4x MSAA, Environment at its defaults with the sky, clouds and sun on
(elevation 35, azimuth 135), spawn and one interior pose a map:

| View | Frame ms, all off / all on | All four | stickydraws | sunsummary | drawruns | recordreuse | Hook draw CPU ms off / on | Draws a frame, drawruns off / on | Pixels over 8 levels, on-off / same side |
| --- | --- | ---: | ---: | ---: | ---: | ---: | --- | --- | --- |
| de_ancient spawn | 25.8 / 24.9 | 0.88 | 0.75 | 0.11 | 0.09 | 0.00 | 11.7 / 5.9 | 42950 / 9492 | 14.12% / 14.29% |
| de_ancient interior | 26.5 / 25.9 | 0.61 | 0.43 | 0.46 | 0.06 | -0.03 | 14.8 / 5.8 | 72627 / 15891 | 0.48% / 0.45% |
| de_inferno spawn | 18.2 / 16.7 | 1.41 | 1.05 | 0.49 | 0.04 | 0.03 | 7.1 / 4.5 | 7001 / 3686 | 2.84% / 3.13% |
| de_inferno interior | 16.4 / 15.6 | 0.85 | 0.43 | 0.36 | -0.09 | 0.03 | 8.5 / 5.4 | 8471 / 3532 | 0.00% / 0.00% |
| de_mirage spawn | 13.0 / 13.0 | -0.01 | 0.01 | -0.00 | -0.05 | -0.01 | 3.6 / 2.8 | 5847 / 2692 | 0.85% / 0.86% |
| de_mirage interior | 16.7 / 16.2 | 0.52 | 0.19 | 0.33 | -0.14 | -0.01 | 5.4 / 4.0 | 9676 / 4938 | 0.00% / 0.00% |
| de_nuke spawn | 22.9 / 21.0 | 1.97 | 2.19 | 0.09 | 0.07 | 0.05 | 12.5 / 5.0 | 5353 / 4634 | 0.04% / 0.03% |
| de_nuke interior | 21.3 / 20.8 | 0.48 | 0.24 | 0.18 | -0.01 | 0.04 | 3.2 / 2.5 | 3223 / 2635 | 0.00% / 0.00% |

The frame time gain is small at this resolution because the GPU is then the limit: the render
thread waits in Present for about half of each frame. The hook's own draw cost on that thread falls
by a fifth to a half, which is what a lower resolution, a slower CPU or a lighter view gets back:
at 1920x1080 the Inferno spawn went from 15.8 to 14.6 ms with all four on (11.4 to 10.3 ms with only the
lamps lit), the Inferno interior from 11.7 to 11.1 ms and Ancient from 18.9 to 18.2 ms.
Window captures of the two sides differ no more than two captures of one side (wind-moved foliage,
particles and the frame counter differ in both).

Tried and left out: turning off engine work whose result the Environment replaces. `r_ssao`,
`lb_enable_shadow_casting` and the scene system's batching thresholds are hidden development
variables, so a console command does nothing until `mirv_cvar_unhide_all` has run. With them
unhidden, `r_ssao 0`, `lb_enable_shadow_casting 0` and `r_shadows 0` measured no gain at the
Inferno spawn (1920x1080), and `csm_max_num_cascades_override` is already 0 there. Do not lower
`sc_layer_batch_threshold_fullsort` (default 80) to look for longer runs of one program: 0 crashed
the scene system on a worker thread (`scenesystem.dll+0xa2d4f`, under `InitDrawingData`).

What the frame is still made of, at the same settings (GPU pass profile and
[`profile-gpu-costs.py`](../../Automation/verify/profile-gpu-costs.py)):

- The lit draws themselves (the engine's shading with our light loop, lamp shadow filter and bounce
  query) are 45 to 60% of the GPU frame. In the Inferno church the map's lamps cost about 8 ms of a
  17 ms frame: about 2 ms in lamp shadow lookups, 1.8 ms in the lamp shadow pass and the rest in the
  light loop.
- The opt-in `mirv_relight giscale 0.5` measured 2.5 ms faster in the church and about 1 ms at the
  spawn. It stays opt-in for the differences described under Runtime bounce.
- Inferno's stained glass added 59 beam lights (292 records instead of 233, ten mask words instead
  of eight): about 2 ms at the spawn, where no window is in view. Their shadow maps were cached; the
  cost was in the light loop. The beams are now off unless `mirv_relight stainedglass` asks for them.
- Sun shadows cost about 5 ms at the spawn before the summary (cascades, the penumbra pass and the
  lookups), clouds about 2.2 ms (`composite: fog volumes`).
- A quarter of the lit draws still take the full setup, all of them at a program change.

## Light lists, lamp face refresh and the engine's exposure

Measured on 2026-11-12 (RTX 5070, CS2 after that day's update, offline maps, no bots, fixed
`mirv_input` camera, 2560x1440).

At this resolution the GPU is the limit: [`SampleThreadStacks`](../../Automation/tools/sample-thread-stacks.cpp)
found CS2's busiest thread about half busy and mostly waiting, and the frame time equals the GPU
frame. Draw setup on the render thread therefore does not change the frame rate here, and this
pass looked for GPU time only. At the Inferno spawn by day the map's lamps cost 3.4 ms of a 14 ms
frame (`locallights 0`): about 2.1 ms in the lit draws and the rest in keeping their shadow faces
current.

Five changes, each with a transient switch (default 1) that restores the previous path in the same
process:

- `mirv_relight engineexposure 0|1`. The visible reach needs the exposure CS2's tonemapper applies,
  and under CS2's own adaptation it was unknown, so no light was ever cut (`reach` did nothing
  unless an exposure was pinned). CS2 carries that exposure in the per-view constants of every lit
  draw: `b1[18].x`, with its reciprocal in `.y`. Forcing `mat_tonemap_force_scale` to 0.5, 1 and 2
  made `.x` follow exactly, and with the scale back at 0 it read 1.1 on Inferno and 1.7 on Ancient
  ([`find-engine-exposure.py`](../../Automation/verify/find-engine-exposure.py) repeats this with
  `mirv_relight dumpview`). The hook copies that row to a staging buffer once a frame and reads it
  a frame or two later without waiting (`NativeExposureScale` in
  [`EnvironmentNativeMaterial.cpp`](../../AfxHookSource2/Filmmaker/Environment/EnvironmentNativeMaterial.cpp)).
  It counts only while the two lanes are reciprocal, so a moved layout reads as unknown and cuts
  nothing. A pinned exposure and the hook's own adaptation still come first.
  `mirv_relight engineexposure` prints the value being read.
- `mirv_relight fineslices 0|1`. The light lists used CS2's 64 depth slices over the whole map,
  100 to 150 units each, and a light's interval grows by a slice at both ends. The shipped loop
  only multiplies depth by `cb3[23].x` and clamps to `cb3[23].y`, so with the owned list builder
  (`clusters 3`) the slices are 16 units deep and as many as the map needs, up to 1,024
  (`MakeClusterGrid` in [`RelightClusterMath.h`](../../AfxHookSource2/Filmmaker/Relight/RelightClusterMath.h)).
  The masks stay conservative: only the rounding changed.
- `mirv_relight litbounds 0|1`. A lamp of the static tier has six faces of rigid map depth
  (`mapStaticBase`). Nothing it lights lies behind that depth, because moving casters only bring a
  face's depth nearer. Once the tier is complete, one compute pass takes the farthest depth of
  each 16-texel block of every face, a staging copy brings them to the CPU, and each lamp gets the
  box around what that depth lets it reach, with the neighbouring blocks, twice the receiver bias
  and 16 units added ([`RelightLitBoundsMath.h`](../../AfxHookSource2/Filmmaker/Relight/RelightLitBoundsMath.h)).
  The list builder intersects a cube record's range box with it. Budgeted lamps (their 1024-texel
  faces can see through gaps the 128-texel ones close), face records, panel lights and any shadow
  reach keep the range sphere. A player standing behind a wall inside the larger character bias
  (`CharacterLocalBias`) and outside the box no longer takes that lamp's light through the wall.
- `mirv_relight tilerefresh 0|1`. A refresh batch gave each of its sixteen lamp faces a depth
  target and a transfer texture. Now the batch shares one depth target of 4 x 4 tiles: a
  depth-only triangle loads each face's static depth into its tile, the casters are drawn with the
  tile as viewport and scissor, and one copy takes the target out before the tiles go to the atlas.
- `mirv_relight movingactors 0|1`. A cached lamp face with a captured caster in it refreshed every
  frame (up to `actorshadows`), and the capture takes parked props every frame too: at the Inferno
  spawn, with nobody on the map, 287 visible faces qualified and 64 faces were refreshed a frame.
  A block of triangles whose bounds are exactly last frame's has not moved, and only a caster that
  moved earns the every-frame refresh now. The other faces keep their place in the rotation, so
  that view refreshes 32 faces a frame. A caster that animates inside unchanged bounds refreshes in
  the rotation, not every frame.

`RelightClusterMathTests` cover the slice count, that every depth and every point a lamp lights
stays inside its lists with fine slices and with lit bounds, and the box against per-texel depth:
60,000 receivers that random face depth can light all lie inside it. `RelightClusterGpuTests` still
require the GPU masks to equal the CPU bounds.

[`profile-lighting-switches.py`](../../Automation/verify/profile-lighting-switches.py) alternates
each switch off and on, two rounds of four seconds a side. Milliseconds a frame saved; the last
column is all five off against all five on:

| View | engineexposure | fineslices | litbounds | tilerefresh | movingactors | Frame ms, all off / all on | fps |
| --- | ---: | ---: | ---: | ---: | ---: | --- | --- |
| de_ancient T spawn, day | 2.35 | 0.50 | 0.11 | 0.06 | 0.29 | 26.49 / 22.48 | 37.7 / 44.5 |
| de_ancient lamp corridor, night | 1.25 | 0.15 | 0.20 | 0.20 | 0.73 | 22.44 / 19.73 | 44.6 / 50.7 |
| de_inferno CT spawn, day | -0.12 | 0.12 | 0.06 | 0.03 | 0.02 | 16.64 / 16.39 | 60.1 / 61.0 |
| de_inferno lamp doorway, night | -0.42 | 0.19 | 0.05 | 0.12 | 0.27 | 15.31 / 14.43 | 65.3 / 69.3 |
| de_mirage T spawn, day | 0.08 | -0.00 | -0.02 | 0.09 | -0.03 | 13.86 / 13.66 | 72.2 / 73.2 |
| de_mirage CT room, night | 0.25 | 0.10 | 0.23 | 0.05 | -0.02 | 11.68 / 10.62 | 85.6 / 94.2 |
| de_nuke T spawn, day | 0.02 | 0.22 | -0.01 | 0.03 | -0.01 | 14.35 / 14.10 | 69.7 / 70.9 |

Differences under about 0.1 ms are within the spread between rounds. The gain is where a map has
many dim lamps with long authored ranges (Ancient's candles); on Inferno the reach cut nothing
measurable. The five together save more than
their sum in several rows: a shorter reach leaves the slices and boxes more to remove. The
moving-caster column is the case without players; with players moving near lamps their faces
refresh every frame as before.

Window captures of the two sides (all five) differ no more than two captures of one side: at the
Ancient lamp corridor at night 0.016% and 0.029% of pixels were more than eight levels apart
between the sides against 0.038% and 0.032% within a side, and at the T spawn by day 6.5% and 7.8%
against 9.8% and 7.9% (leaf shadows keep moving there).

Tried and left out: joining shadow cell runs across rejected cells of up to 96 indices. Foliage
cells are larger than that, so it joined nothing.
