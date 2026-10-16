#pragma once

// Live demo scrubbing: the demo WORLD follows a timeline drag instead of freezing until release
// and jumping. Policy in DemoScrubMath.h; contract in Docs/features/filmmaker_demo_scrubbing.md.
//
// Every scrub surface feeds this: the camera timeline slider, the graph editor ruler and CS2's own
// demo bar (its slider is observed from the injected demo-bar script). Main thread only.

namespace advancedfx { class ICommandArgs; }

namespace Filmmaker {

// The playhead is being dragged to `tick` (may be fractional). The world follows.
void DemoScrub_Drag(double tick);
// The drag ended on `tick`: land exactly there, then put the user's speed and play state back.
// resume=false lands paused even if the demo was playing.
void DemoScrub_Release(double tick, bool resume = true);
// True from the first drag until the world has landed after the release.
bool DemoScrub_Active();
// While active: the tick the world is being steered to (the latest drag or release position).
bool DemoScrub_Target(double& tick);
// Another seek takes the demo clock: drop the scrub so it does not drag the world back to its own
// target. Restores the user's speed; leaves the play state to the caller.
void DemoScrub_Cancel();

// Main thread, once per frame.
void DemoScrub_RunFrame();

// Console: `mirv_filmmaker scrub drag|release <tick>`, `mirv_filmmaker scrub status`.
void DemoScrub_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd);

} // namespace Filmmaker
