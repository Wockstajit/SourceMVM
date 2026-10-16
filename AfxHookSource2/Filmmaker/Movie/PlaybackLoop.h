#pragma once

// PlaybackLoop: an In/Out region playback jumps back to instead of running past.
//
// This is a REVIEW tool, not an export one. You park a loop over a two-second frag and keep
// watching it while you tune a camera, a recoil curve or an effect, without reaching for the
// scrubber between every pass.
//
// DELIBERATELY SEPARATE FROM THE RENDER AND EXPORT RANGES.
//
// All three are "two ticks and a duration", so the obvious move is to share one range. The
// report asks for the opposite, and it is right: the loop is something you move constantly
// while working, and the render range is the thing you spent time getting exactly right. If
// they were the same object, every loop nudge would silently redefine what a render would
// produce -- and you would not find out until the export came back wrong. They stay
// independent, and moving a range from one to the other is an explicit action
// (`mirv_filmmaker loop torender` / `fromrender`), never a side effect.
//
// BEHAVIOUR
//
//   * Only fires while playback is RUNNING. A paused demo parked past Out is left alone --
//     otherwise the loop would fight you every frame you sat there.
//   * Fires on tick >= Out, seeking back to In. At 16x timescale the tick advances ~17 per
//     frame, so the test has to be ">=", never "==".
//   * Scrubbing BEFORE In is not fought. You get the lead-in, and the loop closes when
//     playback reaches Out as usual.
//   * The seek is debounced. demo_gototick replays packets over several frames, during which
//     the current tick still reads at/past Out; without the guard the first loop would issue a
//     seek every frame until it landed.
//
// Persisted per demo as the "loop" section of source_mvm/demos/cs2/<demo>/project.mvm,
// owned entirely by this module.
//
// Driven once per frame from Filmmaker::RunMainThreadFrame(). Main/UI thread only.

#include <sstream>

namespace advancedfx { class ICommandArgs; }

namespace Filmmaker {

void PlaybackLoop_RunFrame();

void PlaybackLoop_SetEnabled(bool enabled);
bool PlaybackLoop_Enabled();

// In/Out in DEMO ticks. Setting one side before the other is fine -- the loop simply stays
// inactive until both exist and Out is greater than In.
void PlaybackLoop_SetIn(int tick);
void PlaybackLoop_SetOut(int tick);
// Atomic endpoint update used by draggable flags. Endpoints are ordered and persisted once,
// so crossing the flags swaps their roles instead of temporarily invalidating the loop.
void PlaybackLoop_SetRange(int inTick, int outTick);
void PlaybackLoop_Clear();
// A practice session begins or ends: read the playing demo's loop again on the next frame. Every
// edit saves at once, so there is nothing unsaved to drop.
void PlaybackLoop_ReloadProject();

// False when the region is unset or degenerate, in which case the loop cannot run.
bool PlaybackLoop_Range(int& inTick, int& outTick);

// For the timeline UI: loop state + region, appended to the HUD state JSON.
void PlaybackLoop_AppendStateJson(std::ostringstream& o);

// mirv_filmmaker loop [on|off|toggle|in [<tick>]|out [<tick>]|range <in> <out>|clear|status|torender|fromrender]
void PlaybackLoop_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd);

} // namespace Filmmaker
