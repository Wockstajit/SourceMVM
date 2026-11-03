#pragma once

// What the live demo scrub (DemoScrub.cpp) asks the game it runs in. The scrub's rules (glide toward a
// playhead ahead, restore behind it, land a release exactly, the frame cache's serves) are the same in
// every game; how a game reads its demo clock and issues its demo commands is not. CS2 answers in
// DemoScrubHostSource2.cpp, CS:S in AfxHookSource/Filmmaker/SourceDemoScrub.cpp.
//
// Main thread only.

#include <string>

namespace Filmmaker {

// A demo is open: its whole tick, its time in ticks with the fraction a paused demo can rest at
// between two ticks, and whether it is paused. False without a demo.
bool ScrubHost_DemoState(int& tick, double& now, bool& paused);
// A scrub begins: anything the clock learned about the previous demo is forgotten.
void ScrubHost_BeginScrub();

// The demo restores a backward seek in a frame or two (CS2's keyframed copy, CS:S's rewind in place).
// Otherwise a backward seek replays from far back (CS2) or reloads the demo (CS:S without the rewind),
// and a seek waits longer before it is judged lost. Asked once a scrub.
bool ScrubHost_FastRestores();

// How far past its tick a seek may land (ScrubInput::seekLandsPast): CS2's land on it or one past;
// CS:S's stop on the demo's next packet. Asked once a scrub.
int ScrubHost_SeekLandsPast();

// A render job owns the demo clock: scrubbing is ignored while one runs.
bool ScrubHost_RenderActive();

// The demo speed the user chose, which comes back when a scrub ends.
void ScrubHost_RestoreUserTimescale();
double ScrubHost_UserTimescale();

// The commands. Each goes out on the next frame, the way the console runs them.
void ScrubHost_Pause();                                  // only ever sent while playing: demo_pause toggles in CS2
void ScrubHost_Resume();
void ScrubHost_Play(double timescale, bool resume);      // play toward the playhead at this speed
void ScrubHost_Step(int ticks, double timescale, int fromTick); // play `ticks` real ticks and pause on the last
void ScrubHost_Seek(int tick);                           // from a paused demo; lands paused on the tick

// The last frame's length in seconds (a step's speed is capped to a tick a frame).
double ScrubHost_FrameSeconds();

// The preview shows its last settled frame while a restore and its play-in are in flight.
void ScrubHost_HoldPreview(bool hold);

// "tick=... curtime=..." for `mirv_filmmaker scrub status`.
std::string ScrubHost_ClockText();

} // namespace Filmmaker
