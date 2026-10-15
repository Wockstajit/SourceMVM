#pragma once

// THE demo playback speed. One value, three scrubbers.
//
// CS2's native demo bar (DemoBarButtons), the camera timeline dock and the graph editor dock all
// carry the same speed scrubber now, and they have to agree: set 0.25x on one and the other two
// must read 0.25x, not their own last-known value.
//
// WHY THIS IS TRACKED HERE INSTEAD OF READ FROM THE ENGINE. Both obvious sources are dead ends,
// and both were tried live before this module existed:
//
//   * Panorama's `GetDemoControllerState().fTimeScale` -- the call the docks used to make -- does
//     NOT exist on the in-game HUD context panel this hook builds into. Verified over netcon:
//     "Uncaught TypeError: $.GetContextPanel(...).GetDemoControllerState is not a function". Every
//     read was silently swallowed by its own guard, so each scrubber only ever knew what IT had
//     set, and setting 0.25x in one dock left the others reading 1x.
//
//   * `demo_timescale` is a COMMAND, not a convar (`help demo_timescale` prints a description with
//     no value), so ICvar::FindConVar cannot find it and a cvar read answers with the fallback
//     forever.
//
// So the speed is SET through here and remembered here. Everything that changes demo speed must go
// through DemoTimescale_Set (or `mirv_filmmaker timescale <v>`, which is the same thing from the
// console) rather than issuing `demo_timescale` itself -- that is what keeps the three bars in
// step. A bare `demo_timescale` typed at the console still works, it just will not move the
// scrubbers until something sets the speed through here again.

namespace advancedfx { class ICommandArgs; }

namespace Filmmaker {

// Last speed set through this module. 1.0 until something sets one.
float DemoTimescale_Get();

// Record the speed and apply it to the engine. Non-positive / absurd values are ignored.
void DemoTimescale_Set(float scale);

// mirv_filmmaker timescale [<v>] -- set the demo speed, or report it.
void DemoTimescale_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd);

} // namespace Filmmaker
