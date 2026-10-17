# Render passes (multipass capture)

A render job records one output per **pass**. A pass is an output **type** combined with a **visibility group**, chosen independently:

| Type | Output |
| --- | --- |
| Beauty | the colour image (groups that fill the frame) |
| Key | the isolated subject in its own colour over the background colour, a green screen (Players only, Gun only) |
| Depth | linear grayscale camera depth (distance along the view axis): white at *near*, black at *far* (world units, default 8 / 4096) |
| Normal | view-space surface normal reconstructed from depth, RGB = n·0.5+0.5 (+X right = red, +Y up = green, +Z toward the camera = blue) |

| Visible elements | What is drawn |
| --- | --- |
| Everything | the scene as the game draws it |
| World + Players | everything except the first-person weapon and its effects |
| World only | the map: no players, player gear, weapons or viewmodel |
| Players only | players and their gear over the background colour |
| Gun only | the first-person weapon and everything it causes (muzzle flash, first-person particles) over the background colour |

Depth and Normal exist for every group. The colour pass is **Beauty** for the groups that fill the frame (Everything, World + players, World only). It is **Key** for the isolated groups (Players only, Gun only), which need a background to key out. That makes 15 passes in total (`PassAllowed`).

The take folder holds one folder per type. Each video pass is one file in its type folder, named for its group: `beauty\everything.mp4`, `beauty\world.mp4`, `beauty\world_players.mp4`, `depth\players.mp4`, `key\gun.mp4`. An image sequence keeps each group's frames in their own folder inside the type (`beauty\world\frame_000001.png`). `manifest.json` lists each stream's `folder` (and `file` for videos). The model and its rules live in [`RenderPassPlan.h`](../../AfxHookSource2/Filmmaker/Render/RenderPassPlan.h), and [`render-pass-plan-tests.cpp`](../../Automation/tests/render-pass-plan-tests.cpp) unit-tests them.

## Synchronisation: one simulation state per output frame

Every pass of output frame *N* is rendered inside the **same engine frame**. The recorder renders the normal view once (the *Main* render). It then re-runs the engine's client render output for each extra render before the engine advances ([`RenderServiceHooks.cpp`](../../AfxHookSource2/RenderServiceHooks.cpp), HLAE's extra-pass loop). The simulation advances exactly once per output frame (`host_framerate` = the recording FPS), so every pass reads the identical client state: player transforms and animation, ragdolls, particles, effects, camera and camera shake. Nothing is advanced, rewound or re-timed between passes, and the demo clock is never driven by `timescale`.

Measured: an extra render with no changes is pixel-identical to the Main render of the same frame (0.00 % of pixels differ). Consecutive frames differ by 0.4 to 1.6 %.

## Physical renders

Passes that need the same scene share one render (`RenderKey`):

| Render | Serves | How |
| --- | --- | --- |
| Main | every *Everything* pass | the normal view the user sees; always rendered |
| WorldOnly | World only; the depth reference of Players only | viewmodel off + the World Only draw filter, active for that render only. Rendered before NoGun, output-less when only Players only needs it |
| NoGun | World + Players, Gun only, Players only | `r_drawviewmodel` is written false for that render only (direct convar write with its change callback) and restored after |

A job never needs more than three renders per output frame, whatever it records. Progress counts output frames, and the workspace shows the renders per frame alongside.

**Gun only** has no render of its own. Its subject is the first-person viewmodel (gun and arms) in Main, found by depth: CS2 draws the viewmodel at raw depth 0.1 or less, so that band is the gun whatever its colour. On top of that comes the per-pixel difference between Main and NoGun of the same frame, which adds what the viewmodel causes outside the band (muzzle flash, first-person particles). Colour and depth come from Main. The difference only works because both renders share one simulation state. The difference alone keyed out every part of the gun as bright as the background behind it: grey sleeve and barrel highlights over grey smoke came out key green (2026-10-17).

**Which render is being drawn** is one engine-thread value (`AfxStreams_CurrentPassRenderKey`). It is set when a render begins, before its scene update. It is cleared only after the extra-pass loop has waited for that render to finish (`RenderServiceHooks.cpp` waits for each render before switching). Every draw-level decision (World Only's player drop, Players Only's repaint, the chroma clear, the foliage and indirect-draw skips) reads it.

Two other timing sources are unreliable (measured). The render-thread begin/end queues land late relative to a render's scene draws: Players Only loses its players on 13 of 20 frames behind a World Only render. The in-stream pass ordinal lags a render behind: World Only can draw with no filter, and Players Only with World Only's. Ending one render and priming the next before waiting for the first also leaks state between them.

Isolation renders change **nothing outside their own draws**. They never touch global state while a job runs: no convars other than the per-render viewmodel switch, no sky material, no material pokes. The Main render, the view the user sees, therefore stays the full beauty scene.

## The pass compositor

Every pass other than a direct Beauty capture goes through [`RenderPassCompositor`](../../AfxHookSource2/RenderPassCompositor.cpp) and [`afx_render_pass_ps_5_0.fxc`](../../Shaders/afx_render_pass_ps_5_0.fxc). It runs at the BeforeUi boundary of its render, where that render's colour and depth are both bound:

- Depth is the engine's raw depth (sample 0 of the 8× MSAA buffer), unprojected exactly like the Environment renderer. It is normalised into the **world viewport's depth range** and multiplied by the frame's inverse world-to-clip. CS2's raw depth does not follow one projection. The world is drawn into the viewport range, and the first-person viewmodel into its own band below it (raw ≈ 0.094 for a pistol on cs_office, with the world at 0.87 to 0.93). The sky clears to the far end, and the 3D skybox uses what is left. Decoding with the bare projection's `m[2][2]`/`m[2][3]` pair ignores the viewport range and collapses the whole room into 56 to 88 units (measured). Depth passes write the distance along the view axis.
- Normals come from that distance at the pixel and its neighbours. The code takes the neighbour on the same surface on each axis, so silhouettes do not smear. Depth alone yields only each triangle's face normal, which looks faceted on characters. So the face normals of nearby pixels on the same smooth surface (near the pixel's tangent plane, within ~45 degrees) are averaged over a 9x9 px window. The result approximates the interpolated vertex normals the game shades with. Creases and silhouettes stay sharp. Texture normal-map detail is not available, because CS2 exposes no normal buffer.
- Players only uses the NoGun render. Subject = pixels **closer than the World Only render** of the same frame (by more than max(1.5 units, 0.4%)). Everything at world depth is background (props, wires, signs, decals, lighting). So the Beauty plate's background is exactly the key colour, and Key / Depth / Normal show the players alone.

  No material is swapped in a job. Weapons lying on the ground are dropped by World Only, so they count as subject.

  The live workspace view does the same. While a Players only pass is shown, the render loop adds one output-less World Only render per frame, and the next frame is masked against it (identical while paused, one frame behind during playback).
- Gun only: subject = Main's viewmodel depth band (raw depth 0.1 or less), plus where Main and NoGun differ (smoothstep 0.03 to 0.08). Colour and depth come from Main. The live view has one render, so it shows the band alone (no muzzle flash).

The compositor applies the background colour (`render pass key`), so any colour works.

**Blood.** Blood that the Players only render cannot skip (drawn from command lists the engine recorded before the pass armed) is tinted into the key. The **Blood** toggle removes it at the source instead. Off sets `violence_hblood 0` and `violence_ablood 0` for the whole job. The state guard restores them afterwards, so no blood spawns in any pass, Beauty included. Blood decals already on walls before the job stay.

## Controls

The Render workspace's **Passes** tab has a type × visibility grid, the background colour, the depth range, and a live view of one enabled pass (Gun only shows the gun and arms live; the muzzle flash needs the job's second render). Below that are the game HUD (Beauty · Everything only), the camera sidecar, and blood.

```
mirv_filmmaker render pass                                   status + help
mirv_filmmaker render pass add|remove|toggle <beauty|depth|normal> <all|worldplayers|world|players|gun>
mirv_filmmaker render pass clear
mirv_filmmaker render pass key <r> <g> <b>                   0-255
mirv_filmmaker render pass depth <near> <far>                world units
mirv_filmmaker render pass keytolerance <0..1>               players-only background match
mirv_filmmaker render pass preview <type> <elements> | off   live view in the workspace
mirv_filmmaker render pass hud none|game|full
mirv_filmmaker render pass cameradata 0|1
mirv_filmmaker render pass blood 0|1                         0: no blood during the job
```

**Live view.** The Passes tab lists exactly the passes the job renders (`Normal · World only`, `Depth · Players only`, ...). It shows the chosen one on the camera view while the workspace is open. The view uses the visibility group through the same scene draw policies a job uses. For Depth, Normal and Key, it runs the same pass compositor (`AfxRenderPass::ComposeInto`) over the finished scene before the UI, with the current depth range and key colour. Turning a pass off drops it from the view. A running job stands the live view down.

**HUD none** hides the Panorama HUD and also sets `crosshair 0` for the job. CS2's crosshair is not a Panorama panel, so it would otherwise stay in the plate. The job's state guard restores the viewer's `crosshair` value on every outcome. Game and Full keep the crosshair as part of the HUD.

EXR output (pre-tonemap HDR) records only the Beauty + Everything plate. `render pass depthdebug 1` records the raw depth sample in Depth passes (r + g/255 + b/65025), which helps diagnose depth decoding.

## Known limits

- Normals are reconstructed from depth, so they are geometric. Normal maps and bump detail are not in them. Alpha-to-coverage surfaces (glass) take the nearest covered MSAA sample. That makes them deterministic, but solid in Depth and Normal.
- Depth passes are 8-bit gray. Choose *near* / *far* to fit the shot (indoor cs_office reads well at 8 / 1024).
- Players only keeps world effects drawn over players (muzzle flashes, tracers, smoke). Gun only includes everything the first-person weapon causes, including its muzzle-flash light on the floor, and a few pixels of window reflections that differ between the two renders.
- The HDR EXR path, the HUD modes and accumulation motion blur apply to Beauty + Everything only.

## Verification

[`verify-render-multipass.ps1`](../../Automation/verify/verify-render-multipass.ps1) renders a short range with the chosen passes and checks the folders, frame counts and manifest. Then [`render_pass_sync.py`](../../Automation/verify/render_pass_sync.py) scores every pass against Beauty at frame offsets −1, 0 and +1. Offset 0 must win on every moving frame.
