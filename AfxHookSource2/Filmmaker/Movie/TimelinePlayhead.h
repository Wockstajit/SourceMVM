#pragma once

// TimelinePlayhead: the ONE timeline cursor, shared by the camera timeline and the graph editor.
//
// WHY THIS EXISTS
//
// The camera timeline and the graph editor are two VIEWS OF THE SAME SHOT. "Where is the
// playhead" therefore has exactly one correct answer, and both panels must read it from the same
// place or they drift apart the moment either one is dragged.
//
// They used to own separate scrub state:
//
//     GraphEditorExperimentHud : m_scrubbing / m_scrubTick
//     CameraPath (via CamPlayback) : m_scrubbing / m_scrubTick
//
// Both fell back to the demo tick when idle, which made them look synchronised until you touched
// one. Neither drag seeks the world (an engine seek per dragged tick is far too expensive -- the
// world moves on RELEASE), so during and after a drag the demo tick is stale, and the panel that
// did not do the dragging kept reporting that stale tick. Measured live: after dragging the graph
// ruler to tick 5000, `graph.playhead` reported 5000 while the demo -- and therefore the camera
// timeline's readout and slider -- still sat at 17801.
//
// THE CONTRACT
//
//   * Not scrubbing -> the playhead IS the demo tick. Both panels agree for free.
//   * Scrubbing     -> the playhead is the dragged tick. The world has not moved yet; whichever
//                      panel is on screen previews the camera there.
//   * Release       -> ends the scrub AND seeks the demo to that tick (DemoScrub_Release, sent
//                      BEFORE the release). Both callers must honour this; a release that does
//                      not seek re-creates the exact bug this module removes.
//   * Held          -> after a release the playhead keeps reporting the released tick while the
//                      world travels there, and afterwards while the demo sits where it landed
//                      (within a tick or two: a seek can land one past). A seek takes frames and
//                      can end a tick off, and reading the demo tick in that window made the
//                      playhead jump back for the whole seek and then sit beside the key it was
//                      sent to -- so the key read as "not here" and a value edit dropped a second
//                      key one tick away. Playback, a step, or any seek elsewhere moves the demo
//                      tick and hands the playhead back to it.
//
// This owns the CURSOR only -- which tick the user is pointing at. It does not drive the camera,
// evaluate a pose or touch the engine; CamPlayback::TickScrub and
// GraphEditorExperimentHud::DriveCameraThisFrame still each render their own view of that tick.
//
// Threading: main/UI thread only (both HUD pumps and the console dispatch run there).

namespace Filmmaker {

// Enter or continue a drag at `tick`. Idempotent; call it for every dragged value.
void TimelinePlayhead_BeginScrub(double tick);

// Release: hold the released tick until the demo has landed there, then follow the demo tick (see
// "Held" above). The CALLER is responsible for seeking the demo to the final tick first
// (CameraPath::ScrubToTick with seek=true, or the graph's `scrub release`). With no drag open it
// changes nothing, which is what graph enter/exit/clear rely on when they call it as a reset.
void TimelinePlayhead_EndScrub();

bool TimelinePlayhead_Scrubbing();

// The cursor: the dragged tick while scrubbing, otherwise the live demo tick.
double TimelinePlayhead_Tick();

// Drop the scrub (demo change, panel teardown) so a new session starts on the demo tick.
void TimelinePlayhead_Reset();

} // namespace Filmmaker
