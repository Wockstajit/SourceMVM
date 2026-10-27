#pragma once

// The one timeline cursor in CS:S, CS2's TimelinePlayhead (AfxHookSource2/Filmmaker/Movie/
// TimelinePlayhead.h, whose header says why there is only one):
//
//   * not scrubbing: the playhead is the demo tick;
//   * scrubbing: it is the dragged tick, and the world has not moved yet;
//   * released: the caller has sent the demo to the tick (DemoBar_SeekTo), and the playhead holds
//     that tick while the demo travels there and while it sits where it landed, within a tick or
//     two. Playback, a step or a seek elsewhere hands it back to the demo tick. Without the hold a
//     key jumped to reads as "not here" while the seek runs, and an edit keys a second key beside it.
//
// It owns the cursor only; the TIMELINE dock drives the camera from it (SourceTimelineDock.h).
// Main thread only.

namespace SourceFilmmaker {

// Enter or continue a drag at tick.
void TimelinePlayhead_BeginScrub(double tick);
// Release: hold the released tick until the demo has landed there. The caller seeks first.
void TimelinePlayhead_EndScrub();
bool TimelinePlayhead_Scrubbing();
// The dragged tick while scrubbing, the held tick after a release, else the demo tick.
double TimelinePlayhead_Tick();
// A new demo starts on its own tick.
void TimelinePlayhead_Reset();

} // namespace SourceFilmmaker
