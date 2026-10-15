#pragma once

// End-of-demo hold: stops playback before the demo runs out from under the editor.
//
// TWO hold points, whichever comes first:
//
//   POSTGAME -- the moment CCSGameRules reports the match-over phase. That is where CS2's
//     end-of-match sequence starts (win panel, then the podium line-up, then the final
//     scoreboard), and it runs for tens of seconds of demo time. It is also where the editor's
//     scaled preview comes apart: the engine swaps HUD roots for the podium, the world blit
//     stops matching the preview rect, the game fills the screen and the timeline stops taking
//     scrubs. There is nothing to film after this point, so playback stops before it.
//
//   LAST TICK -- the original guard, a second before the header's playback_ticks, for demos
//     where the phase is never reported (POV demos, unfinished matches, a CS2 update moving
//     the schema field). Also the backstop if you scrub past the postgame hold by hand.
//
// Either way: pressing play while held restarts the demo from the beginning instead of running
// into the stop record and disconnecting to the main menu. Scrubbing BACK from a hold works
// normally -- the hold only ever re-arms when you are past its tick again.
//
// Driven once per frame from Filmmaker::RunMainThreadFrame(). Always on while a demo is active;
// `mirv_filmmaker endhold 0/1` toggles it at runtime, `endhold status` reports what it sees.

namespace advancedfx { class ICommandArgs; }

namespace Filmmaker {

void DemoEndHold_RunFrame();

void DemoEndHold_SetEnabled(bool enabled);
bool DemoEndHold_Enabled();

// mirv_filmmaker endhold [on|off|toggle|status|postgame on|off|phase <n>]
void DemoEndHold_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd);

} // namespace Filmmaker
