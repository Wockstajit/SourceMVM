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
// Main thread only.

#include <AfxHookSource2/Filmmaker/Movie/CamMarkers.h>

namespace advancedfx { class ICommandArgs; }

namespace SourceFilmmaker {

enum class PathMode { Editing, Armed, Playing };

struct PathState {
	int count = 0;
	int selected = -1;
	int aimed = -1;            // the marker the view aims at (CamMarkerPickMath.h), -1 for none
	PathMode mode = PathMode::Editing;
	Filmmaker::PathSettings settings;
	double duration = 0;       // the timing axis's length (Freeze pacing), seconds
	const char* notice = "";   // the last refusal ("Need at least 2 camera markers..."), for 3.5 s
};

// Once a frame: aim, the armed hold, playback and the end of the path.
void CameraPath_Frame();

const Filmmaker::CamMarkers& CameraPath_Markers();
PathState CameraPath_State();
// The path flies the view: armed or playing. The director then leaves the mouse and wheel alone.
bool CameraPath_OwnsView();

void CameraPath_Place();           // K: a marker at the current view and tick (one per tick)
bool CameraPath_DeleteAimed();     // L: false when no marker is aimed at
void CameraPath_Delete(int index);
void CameraPath_DeleteAll();
void CameraPath_Select(int index);
void CameraPath_Arm();             // jump to the first marker, paused, its pose held
void CameraPath_Play();            // from armed (or arms first)
void CameraPath_Pause();
void CameraPath_Stop();

// The path's keys outside the workspaces (CS2's MovieMode): K, J, L; while armed Space plays, Esc
// cancels and the arrows are held; while playing Space pauses, Esc stops, W A S D stop the path and
// still move the free cam. True when the key was the path's.
bool CameraPath_OnKey(int vk, bool down, bool repeat);

// mirv_filmmaker marker ...
void CameraPath_Command(int argc, advancedfx::ICommandArgs* args);
void CameraPath_PrintHealth();

} // namespace SourceFilmmaker
