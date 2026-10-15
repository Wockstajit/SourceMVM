#pragma once

// An In/Out region playback jumps back to instead of running past.
//
// A REVIEW TOOL, NOT AN EXPORT ONE. You park a loop over a two-second fight and keep watching it
// while you tune a camera, without reaching for the scrubber between every pass.
//
// DELIBERATELY SEPARATE FROM THE RENDER RANGE. Both are "two ticks and a duration", so sharing one
// object is the obvious move and it is wrong. The loop is something you nudge constantly while
// working; the render range is the thing you spent time getting exactly right. Share them and every
// loop nudge silently redefines what a capture would produce, and you find out when the export comes
// back wrong. Moving a range between them is an explicit action -- `loop torender` / `loop
// fromrender` -- and never a side effect.
//
// BEHAVIOUR, and the reason for each rule:
//
//   * Only fires while playback is RUNNING. A paused replay parked past Out is left alone;
//     otherwise the loop fights you every frame you sit there looking at something.
//   * Fires on tick >= Out, not == Out. At 16x the tick advances seventeen per frame and an
//     equality test never matches.
//   * Scrubbing before In is not fought. You get the lead-in and the loop closes at Out as usual.
//   * The seek is debounced. Seeking replays packets over several frames, during which the current
//     tick still reads at or past Out -- without the guard the first wrap issues a seek every frame
//     until it lands, which is a seek storm on the most expensive operation available.
//
// Persisted per replay as the "loop" section of source_mvm/demos/deadlock/<demo>/project.mvm.
//
// Driven once per main-thread frame. Main thread only.

#include <string>

namespace advancedfx { class ICommandArgs; }

namespace Filmmaker {

void PlaybackLoop_RunFrame();

void PlaybackLoop_SetEnabled(bool enabled);
bool PlaybackLoop_Enabled();

// In/Out in demo ticks. Setting one side before the other is fine; the loop stays inactive until
// both exist and Out is meaningfully greater than In.
void PlaybackLoop_SetIn(int tick);
void PlaybackLoop_SetOut(int tick);

// Atomic endpoint update, for the draggable flags. Endpoints are ordered and persisted once, so
// dragging one flag past the other swaps their roles instead of briefly invalidating the region.
void PlaybackLoop_SetRange(int inTick, int outTick);

void PlaybackLoop_Clear();

// False when the region is unset or degenerate, in which case the loop cannot run. The UI shows
// that as "armed but incomplete" rather than as a loop that mysteriously never fires.
bool PlaybackLoop_Range(int& inTick, int& outTick);
bool PlaybackLoop_Ready();

// Appended to the workspace state JSON, for the timeline's shading and flags.
void PlaybackLoop_AppendStateJson(std::string& out);

// mirv_filmmaker loop [on|off|toggle|in [tick]|out [tick]|inat <0..1>|outat <0..1>
//                     |range <in> <out>|clear|status|torender|fromrender]
void PlaybackLoop_RunCommand(int argc, advancedfx::ICommandArgs* args);

void PlaybackLoop_Report();

} // namespace Filmmaker
