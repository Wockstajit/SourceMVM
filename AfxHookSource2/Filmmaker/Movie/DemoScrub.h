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
void DemoScrub_Release(double tick);
// True from the first drag until the world has landed after the release.
bool DemoScrub_Active();

// Main thread, once per frame.
void DemoScrub_RunFrame();

// Console: `mirv_filmmaker scrub drag|release <tick>`, `mirv_filmmaker scrub status`.
void DemoScrub_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd);

} // namespace Filmmaker
