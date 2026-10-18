# GPU pass profile

What the hook's own rendering costs the GPU in CS2, pass by pass, beside the GPU frame it runs in.
The profile answers "which of our passes is the frame spending its time on", not "how fast is the
game": present-to-present frame time stays in `mirv_filmmaker health` ("frame timing").

## Commands

- `mirv_filmmaker gpuprofile on` starts it (off by default, no cost while off).
- `mirv_filmmaker gpuprofile` prints the table; `health` prints it too while it is on.
- `mirv_filmmaker gpuprofile reset` clears the window; `off` stops it and releases its queries.
- While `mvm_debug` runs, a `gpu.profile` line every five seconds lists the twelve costliest passes.

The table covers the last 240 resolved frames. For each pass it gives the mean milliseconds per frame
(a pass that ran in only some frames counts as 0 in the others), p95, max, its share of the GPU
frame, and how many times a frame it ran. Above it: the GPU frame (timestamps at successive
presents), the GPU time covered by at least one timed pass (nested passes count once), and the rest:
native CS2 rendering plus the hook's work inside native draws.

## What is timed

`GPU_PASS_SCOPE(ctx, "name")` ([`GpuPassProfiler.h`](../../AfxHookSource2/Filmmaker/Debug/GpuPassProfiler.h))
puts a timestamp pair around the calls that record a pass, on the immediate context or inside a
deferred command list (it resolves when the list runs). Names group by prefix:

| Prefix | Passes |
| --- | --- |
| `shadows` | The map light shadow pass: near (budgeted) lamp faces, sun cascades, static faces while they are first drawn, the lamp face refresh (posed players, props and wind-animated foliage into resident faces), sky visibility, the 3D skybox sun, the lamp shadow summary, plus glass beams, the viewmodel sun and the lighting proof. `shadows (map pass, total)` is the whole command list. |
| `lights` | Light record rebase and clusters, once per view. |
| `gi` | Probe GI: static voxelization while the map is voxelized, region shift, inject, trace, compose, reflection capture, the visible probe mask, screen-space GI. It settles after 48 stable frames and then costs nothing. |
| `casters` | The stream-output capture of players, weapons and props (re-issued native draws), and the bounds readback the face refresh uses. |
| `ao`, `sun visibility` | Owned ambient occlusion and the screen-space sun penumbra. |
| `composite` | The environment composite at BeforeUi: its shadow maps, player mask, world GI, atmosphere surfaces, props, precipitation, fog volumes, storm and final copy. `environment composite (total)` is the whole list. |
| others | Stained glass cookies, lens (depth of field, flare), contact shadows, fisheye, viewport scaler, render pass compositor, campath drawer, auto exposure. |

Work the hook adds inside native CS2 draws has no pass of its own: the derived material programs
(atmosphere and dynamic irradiance variants), the lamp shadow kernel and the light loop over our
records. Two things measure it:

- The table's "sampled whole frames" section: every lit native draw with a patched program bound,
  timed for one whole frame each second (`MaterialTiming` in
  [`EnvironmentNativeMaterial.cpp`](../../AfxHookSource2/Filmmaker/Environment/EnvironmentNativeMaterial.cpp)),
  by variant, and the costliest programs by hash. This is native shading and our additions together.
- [`profile-gpu-costs.py`](../../Automation/verify/profile-gpu-costs.py) switches each piece off in
  turn at the paused pose (flat shader instead of the patched programs, lamp shadow lookups, all
  shadows, sun shadow lookups, local lights, the light loop past one record, GI, reflections, AO,
  sky occlusion, stained glass, the composite) and times the frame against the defaults just before
  and after, in two rounds. It never moves the camera, restores every setting it changed and writes
  a JSON report to `Automation/runs/gpu-profile/`.

## Reading it

The GPU frame equals the frame time only when the GPU is the bottleneck. Another program using the
same GPU (a second game, a video) slows every pass and adds to the frame, so compare passes within
one run rather than across runs on a shared machine.
