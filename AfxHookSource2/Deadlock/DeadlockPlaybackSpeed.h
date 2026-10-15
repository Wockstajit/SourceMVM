#pragma once

// The replay's playback speed: one owner, readable by anything that draws it.
//
// It used to be a `static int` inside the console command's own body. That worked while the only
// way to change the speed was to press a button, and stopped working the moment a panel wanted to
// SHOW the speed: a slider that cannot read the dial has to keep its own copy, and the two drift
// the first time the speed is changed from anywhere else -- a console line, the other panel, a
// key bind. The timeline and the graph both carry a speed control, so that is two copies drifting
// from one dial.
//
// Writing it applies `demo_timescale` immediately. Reading it costs nothing, so the panels can
// seed their controls from it every frame.
//
// Main thread only: every caller is the console dispatch or a HUD pump, both of which run there.

namespace Deadlock {

// The current multiplier. 1.0 until something sets it.
double PlaybackSpeed();

// Set an exact multiplier, clamped by DeadlockPlaybackSpeedMath.h. Continuous -- a slider's value
// is taken as given rather than snapped to a preset, or the handle crawls away from the drop.
void PlaybackSpeed_Set(double speed);

// Move to the next preset beyond the current speed. dir > 0 faster, dir < 0 slower.
void PlaybackSpeed_Step(int dir);

// Re-apply the current speed to the engine. A replay load resets `demo_timescale`, so the dial has
// to be pushed again or the panel reads 4x while the replay runs at 1x.
void PlaybackSpeed_Reassert();

void PlaybackSpeed_Report();

} // namespace Deadlock
