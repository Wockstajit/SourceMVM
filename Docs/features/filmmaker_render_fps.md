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
