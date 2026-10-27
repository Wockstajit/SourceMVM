#pragma once

// Camera paths in CS:S, CS2's: markers placed on the demo (K), aimed at to delete (L), armed and
// played (J, Space, Esc), flown by HLAE's free camera (mirv_input) as CS2 flies its own. The marker
// list, the path and its playback are CS2's shared code (AfxHookSource2/Filmmaker/Movie: CamMarkers,
// CamPathEval on HLAE's CamPath, CamPlayback, CamMarkerPickMath); this is the facade the director,
// the Camera Editor and the console drive, as CS2's CameraPath is.
//
// Timing as in CS2: Live plays the path against the demo's own ticks, Freeze on the wall clock with
// the demo held. Speed modes Manual, Constant and Per-segment, curve Linear or Smooth, per-marker
// ease. A path needs two markers to play.
//
// The Camera Editor's PATH CAMERA inspector edits the path through the same calls CS2's does: select
// a camera (the demo goes to its tick and the view to its pose), move it to the playhead, change its
// FOV, roll, ease and segment speed, and reposition it by flying the free cam there.
//
// Main thread only.

#include <AfxHookSource2/Filmmaker/Movie/CamMarkers.h>

#include <vector>

namespace advancedfx { class ICommandArgs; }

namespace SourceFilmmaker {

namespace Ui { struct PathOverlayModel; }

enum class PathMode { Editing, Reposition, Armed, Playing };
const char* PathModeName(PathMode mode);

// CS2's marker channels (CameraPath::SetChannelValue): x, y, z, pitch, yaw, roll, FOV.
enum PathChannel { kChannelX = 0, kChannelY, kChannelZ, kChannelPitch, kChannelYaw, kChannelRoll, kChannelFov };

struct PathState {
	int count = 0;
	int selected = -1;
	int aimed = -1;            // the marker the view aims at (CamMarkerPickMath.h), -1 for none
	PathMode mode = PathMode::Editing;
	Filmmaker::PathSettings settings;
	double duration = 0;       // the timing axis's length (Freeze pacing), seconds
	const char* notice = "";   // the last refusal ("Need at least 2 camera markers..."), for 3.5 s
	bool menuOpen = false;     // the CAMERA MARKER card is up (F on an aimed marker)
};

// Once a frame: aim, the armed hold, playback and the end of the path.
void CameraPath_Frame();

const Filmmaker::CamMarkers& CameraPath_Markers();
PathState CameraPath_State();
// The path flies the view: armed or playing. The director then leaves the mouse and wheel alone.
bool CameraPath_OwnsView();

// The path in the world (Ui/PathOverlay.h) for a width x height screen: its markers, its line and the
// playhead at the demo's tick. False when there is nothing to draw: no markers, or a path playing
// (CS2 hides its markers while the path owns the shot).
bool CameraPath_Overlay(Ui::PathOverlayModel& out, float width, float height);

void CameraPath_Place();           // K: a marker at the current view and tick (one per tick)
bool CameraPath_DeleteAimed();     // L: false when no marker is aimed at
void CameraPath_Delete(int index);
void CameraPath_DeleteAll();
void CameraPath_Select(int index);
// The editor's selection (CS2's SelectForEditor): the demo goes paused to the camera's tick and the
// free cam holds its pose until the seek lands, then the free cam is the user's again.
void CameraPath_SelectForEditor(int index);
void CameraPath_SelectDelta(int delta);
// Retimes a camera, held between its neighbours' ticks (CS2's MoveKey).
void CameraPath_MoveKey(int index, int tick);
// A value edit as one undo step: begin, any number of previews (the camera shows each), end.
void CameraPath_EditBegin();
void CameraPath_EditValue(int index, int channel, double value);
void CameraPath_EditEnd();
void CameraPath_Undo();
void CameraPath_Redo();
void CameraPath_SetEase(int index, Filmmaker::Ease ease);
void CameraPath_SetSpeedMul(int index, float mul); // its outgoing segment, 0.2..1 (Per-segment)
void CameraPath_SetConstSpeed(float mul);          // the whole path, 0.2..1 (Constant)
void CameraPath_SetInterp(int interp);             // 0 Linear, 1 Smooth
void CameraPath_SetSpeedMode(int mode);            // 0 Manual, 1 Constant, 2 Per-segment
void CameraPath_SetTiming(int timing);             // 0 Live, 1 Freeze
void CameraPath_SetAutoSnap(bool on);              // the card's arrows move the view to the marker
// Reposition (CS2's): the demo goes to the selected camera's tick and the free cam is the user's;
// F or a left click puts the camera where the view is, Esc cancels.
void CameraPath_BeginReposition();
void CameraPath_PlaceReposition();
void CameraPath_CancelReposition();
bool CameraPath_Repositioning();

// The TIMELINE dock bakes its keys into the path (CS2's ReplaceMarkersFromGraph): the whole marker
// list replaced, the selection kept, the path rebuilt and saved. pushUndo makes it one undo step, the
// first bake of a gesture.
void CameraPath_ReplaceMarkersFromGraph(const std::vector<Filmmaker::CamMarker>& markers, bool pushUndo);
// The path's pose at a demo tick (x, y, z, pitch, yaw, roll, FOV); false under two markers.
bool CameraPath_EvalPoseAtTick(double tick, double out[7]);
// AUTO, the CAMERA PATH layer's button on the TIMELINE dock (CS2's RunAutoPath): while the dock is
// open and the demo plays through the path's keys in Live timing, the view follows the path; before
// the first key and after the last it is the game's. J plays the path either way.
bool CameraPath_AutoPath();
void CameraPath_SetAutoPath(bool on);
// The CAMERA MARKER card (CS2's marker menu): F on an aimed marker opens it for that marker, F or Esc
// closes it. While it is up the cursor is the card's.
void CameraPath_OpenMenu(int index);
void CameraPath_CloseMenu();
bool CameraPath_MenuOpen();
// The card's arrows (CS2's SelectDelta): the next or previous marker, the view moved to it when
// auto-snap is on.
void CameraPath_SelectStep(int delta);
void CameraPath_Arm();             // jump to the first marker, paused, its pose held
void CameraPath_Play();            // from armed (or arms first)
void CameraPath_Pause();
void CameraPath_Stop();

// The path's keys outside the workspaces (CS2's MovieMode): K, J, L, F on an aimed marker; while
// armed Space plays, Esc cancels and the arrows are held; while playing Space pauses, Esc stops,
// W A S D stop the path and still move the free cam; while the card is up F and Esc close it.
// True when the key was the path's.
bool CameraPath_OnKey(int vk, bool down, bool repeat);

// mirv_filmmaker marker ...
void CameraPath_Command(int argc, advancedfx::ICommandArgs* args);
void CameraPath_PrintHealth();

} // namespace SourceFilmmaker
