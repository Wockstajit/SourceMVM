#pragma once

// The Camera Editor's TIMELINE dock, game side: CS2's GraphEditorExperimentHud without its Panorama
// glue. It owns the camera channels' keys (GraphExpModel, shared with CS2) and keeps them and the
// camera path one shot:
//
//   * every edit bakes the keys into the path (GraphExpModel::BakeMarkers, then
//     CameraPath_ReplaceMarkersFromGraph), one undo step per gesture;
//   * every frame, a path change the dock did not make (K, the PATH CAMERA tab, an undo, a load) is
//     adopted: seeded into an empty model or reconciled with the kept one;
//   * a scrub with AUTO on, a key edit, and a release until the demo lands show the path's pose at
//     the playhead (SourceTimelinePlayhead.h), inside the path's ticks only.
//
// The dock's UI (Ui/TimelineDock.h) reads the model and calls these. mirv_filmmaker grapheditor
// keeps CS2's verbs for the console and automation. Main thread only.

#include <AfxHookSource2/Filmmaker/Movie/GraphExpModel.h>

namespace advancedfx { class ICommandArgs; }

namespace SourceFilmmaker {

// The dock is on screen (the Camera Editor's TIMELINE tab). Edits bake only while it is.
void TimelineDock_SetEnabled(bool on);
bool TimelineDock_Enabled();

// Once a frame: adopt path changes, drive the camera, reset when the demo goes.
void TimelineDock_Frame();

const Filmmaker::GraphExpModel& TimelineDock_Model();

struct TimelineDockInfo {
	unsigned rev = 0;          // the model changed
	unsigned viewEpoch = 0;    // re-apply the suggested window (enter, reseed, clear, demo change)
	double viewT0 = 0, viewT1 = 1;     // suggested window: the keys, padded, else the playhead +- 1000
	double domainT0 = 0, domainT1 = 1; // the demo's ticks
	bool drive = true;         // the CAMERA PATH layer's switch: edits and scrubs preview the path
	bool autoPath = false;     // its AUTO button
	double playhead = 0;       // SourceTimelinePlayhead's tick
	bool scrubbing = false;
	bool playing = false;      // the demo plays (not paused)
	bool pathPlaying = false;  // the camera path flies the view
	double tickRate = 64;
	bool smoothPath = false;   // the path's interpolation is Smooth
	double live[7] = { 0, 0, 0, 0, 0, 0, 90 }; // the rendered camera, per channel
};
TimelineDockInfo TimelineDock_Info();

// CS2's grapheditor verbs. Each edit bakes into the camera path.
void TimelineDock_EditBegin(); // the next bake is one undo step
void TimelineDock_Select(int ch, int id, bool add);
void TimelineDock_SelectAdd(int ch, int id);
void TimelineDock_SelectAll();
void TimelineDock_SelectClear();
void TimelineDock_MoveSelectedBy(double dTick, double dValue);
int TimelineDock_AddKey(int ch, double tick, double value); // bracket with EditBegin
void TimelineDock_DeleteSelected();
void TimelineDock_SetHandle(int ch, int id, int side, double tx, double dv, bool reflect);
void TimelineDock_Ease(int mode /*0 in, 1 out, 2 in/out*/, bool selectedOnly);
void TimelineDock_SetInterp(bool smooth, bool selectedOnly); // whole path: also the path's interpolation
void TimelineDock_AlignToKey(int ch, int id);
void TimelineDock_AlignToTick(double tick);
void TimelineDock_Undo();
void TimelineDock_Redo();
void TimelineDock_Reseed();
void TimelineDock_SetDrive(bool on);
// The path changed Smooth / Linear from elsewhere: the model takes the same shape.
void TimelineDock_MirrorPathInterp(bool smooth);

// The playhead: a drag previews (release = false); release sends the demo there and holds it.
void TimelineDock_Playhead(double tick, bool release);

// The dock's drive flies the view right now.
bool TimelineDock_OwnsView();

// mirv_filmmaker grapheditor ...
void TimelineDock_Command(int argc, advancedfx::ICommandArgs* args);
void TimelineDock_PrintHealth();

} // namespace SourceFilmmaker
