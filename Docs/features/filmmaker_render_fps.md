# Recording FPS semantics (CS2 + Deadlock)

This is the fixed decision for Source MVM beauty capture in both games.

| Control | Meaning |
| --- | --- |
| **Recording / Frame rate** | **Output and capture FPS**: file mux rate and `host_framerate` |
| **Timeline speed dial** | **Preview only**: ignored for render math |

During a job:

1. Set `host_framerate` to the chosen FPS (clamped).
2. Force `demo_timescale` to `1` so a leftover dial cannot stretch the clock.
3. Mux / encode at the same FPS.

Choose the FPS and start the render. The dial does not need to be set for a take. Playing the file at 1× shows real-time motion at that frame rate. For example, 600 fps gives 600 samples per demo second.

Shared clamp: [`RenderCaptureMath.h`](../../AfxHookSource2/Filmmaker/Render/RenderCaptureMath.h).  
CS2 driver: [`RenderSessionDriver.cpp`](../../AfxHookSource2/Filmmaker/Render/RenderSessionDriver.cpp).  
Deadlock: [`DeadlockRender.cpp`](../../AfxHookSource2/Filmmaker/Deadlock/DeadlockRender.cpp).  
Tests: [`render-capture-math-tests.cpp`](../../Automation/tests/render-capture-math-tests.cpp).

## Render speed

A render job plays the demo at `host_framerate` = the output fps, so every output frame is a full
engine frame plus the hook's own work. The time per output frame is therefore the frame time the
game has at that moment, not a fixed encode cost. Measured on de_ancient at 2560x1351 with 4x MSAA
(2026-10-18, 3-second clips at 60, 120 and 300 fps):

| Custom lighting | Live frame | Rendered output frame | Fixed cost per job |
| --- | ---: | ---: | ---: |
| off | about 6 ms | about 10 ms | about 5 s |
| on (molotov scene) | about 37 ms | about 36 ms | about 6 s |

With the custom lighting on, rendering is exactly as fast as the live view: the GPU is the limit and
the encoder never waits for it. The fixed cost is the deterministic pre-roll (a seek, a one-tick-per-
frame approach to the start tick, a settle), which is what makes two takes of one range land on the
same ticks; it is not shortened for short clips.

- The GPU encoder (NVENC) rendered identical times at its p3, p5 and p7 presets, so it always runs
  p7, its best quality. The Speed row (Best / Balanced / Fastest) only shows when the job encodes
  on the CPU, where the x264 presets change the time per frame several-fold.
- H.264 4:2:0 needs even frame sizes. An odd game window (2560x1351) made libx264 refuse every frame
  and the job failed; both encoders now crop an odd edge by one pixel instead.
- A recording no longer forces the engine's `r_wait_on_present`, which made the CPU wait for the GPU
  every frame. The capture maps each frame's staging copy one Present later, and D3D11 makes that Map
  wait for the copy, so the output does not depend on the engine waiting as well. Measured A/B/B/A:
  300 fps with the lighting off went from 10.4 to 8.2 ms per output frame, and 60 fps with the
  lighting on from 36.6 to 33.3 ms. `mirv_filmmaker render waitpresent 1` restores the old behaviour.
- The CPU encoder is the slow path: at 60 fps with the lighting off, a 3-second clip took 9.9 s at
  Best, 8.5 s at Fastest and 7.8 s on the GPU.
- With the lighting on, the export-only extras (full-resolution cloud and fog march with 64 steps,
  every lamp face refreshed every frame, exact actor bounds) cost about 4 ms per frame in this
  scene; `mirv_relight previewcapture 1` records at preview quality for comparison.
