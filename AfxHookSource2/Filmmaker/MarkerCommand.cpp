// Console command surface for camera markers / dolly path: "mirv_filmmaker marker ...".
// Mirrors the BO2 Theater dolly workflow; also the back-end the K/J/L/F hotkeys + the
// Panorama marker menu issue. All actions run on the main thread (console dispatch).
//
// Split out of FilmmakerCommand.cpp per the Cosmetics_RunCommand file-per-feature pattern
// (the RunCommand entry point is declared in the owning system's header -- here,
// Movie/CameraPath.h -- not a dedicated command header).

#include "Movie/CameraPath.h"
#include "Movie/FollowCamera.h"

#include "../../Shared/AfxConsole.h"

#include <cstdlib>
#include <cstring>

namespace Filmmaker {

void Marker_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd) {
	using CP = Filmmaker::CameraPath;
	Filmmaker::CameraPath& cp = Filmmaker::CameraPathRef();

	if (argc < 3) {
		advancedfx::Message(
			"%s marker place - add a marker at the current camera pose (also K).\n"
			"%s marker preview|arm - jump to first marker and arm playback.\n"
			"%s marker play|previewplay - play the armed path.\n"
			"%s marker previewstop - stop playback (X).\n"
			"%s marker delete <i> | deleteall confirm - remove markers (L = aimed).\n"
			"%s marker select <i> | next | prev - select (menu arrows teleport).\n"
			"%s marker edit <i> | close - open/close the settings menu (F = aimed).\n"
			"%s marker reposition | repositionplace | repositioncancel - move the\n"
			"   selected marker (left-click places, Esc cancels).\n"
			"%s marker speedmode manual|constant|persegment|cycle.\n"
			"%s marker interp linear|bezier|cycle  (path shape, separate from speed).\n"
			"%s marker timing live|freeze|toggle  (global - all cameras).\n"
			"%s marker speedmul <0.2..1.0> - selected marker's OUTGOING segment speed.\n"
			"%s marker constspeed <0.2..1.0> | cycle - global Constant-mode speed.\n"
			"%s marker autosnap on|off|toggle - snap viewer to a marker when selected.\n"
			"%s marker list | save | load.\n",
			cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd);
		return;
	}

	const char* a = args->ArgV(2);
	const char* a3 = (argc >= 4) ? args->ArgV(3) : "";

	if (0 == _stricmp(a, "place")) cp.PlaceMarker();
	else if (0 == _stricmp(a, "preview") || 0 == _stricmp(a, "arm")) cp.ArmPreview();
	else if (0 == _stricmp(a, "play") || 0 == _stricmp(a, "previewplay")) {
		Filmmaker::FollowCameraRef().StopPreview("camera path started");
		cp.StartPreviewPlay();
	}
	else if (0 == _stricmp(a, "previewstop") || 0 == _stricmp(a, "stop")) cp.StopPreview();
	else if (0 == _stricmp(a, "hudtoggle")) advancedfx::Message("mirv_filmmaker: camera path HUD toggle is disabled.\n");
	else if (0 == _stricmp(a, "repositionplace")) cp.PlaceReposition();
	else if (0 == _stricmp(a, "repositioncancel")) cp.CancelReposition();
	else if (0 == _stricmp(a, "delete")) {
		if (argc < 4) { advancedfx::Warning("usage: %s marker delete <i>\n", cmd); return; }
		cp.DeleteIndex(atoi(a3));
	}
	else if (0 == _stricmp(a, "deleteall")) cp.DeleteAll(argc >= 4 && 0 == _stricmp(a3, "confirm"));
	else if (0 == _stricmp(a, "select")) {
		if (argc < 4) { advancedfx::Warning("usage: %s marker select <i>\n", cmd); return; }
		cp.SelectIndex(atoi(a3), /*teleport*/ true);
	}
	else if (0 == _stricmp(a, "next")) cp.SelectDelta(+1);
	else if (0 == _stricmp(a, "prev")) cp.SelectDelta(-1);
	else if (0 == _stricmp(a, "edit")) {
		if (argc < 4) { advancedfx::Warning("usage: %s marker edit <i>\n", cmd); return; }
		cp.OpenMenu(atoi(a3));
	}
	else if (0 == _stricmp(a, "close") || 0 == _stricmp(a, "done")) cp.CloseMenu();
	else if (0 == _stricmp(a, "reposition")) cp.BeginReposition();
	else if (0 == _stricmp(a, "speedmode")) {
		if (0 == _stricmp(a3, "manual")) cp.SetSpeedMode(CP::SpeedMode::Manual);
		else if (0 == _stricmp(a3, "constant")) cp.SetSpeedMode(CP::SpeedMode::Constant);
		else if (0 == _stricmp(a3, "persegment")) cp.SetSpeedMode(CP::SpeedMode::PerSegment);
		else cp.CycleSpeedMode();
	}
	else if (0 == _stricmp(a, "interp")) {
		if (0 == _stricmp(a3, "linear")) cp.SetInterp(CP::Interp::Linear);
		else if (0 == _stricmp(a3, "bezier")) cp.SetInterp(CP::Interp::Bezier);
		else cp.CycleInterp();
	}
	else if (0 == _stricmp(a, "timing")) {
		if (0 == _stricmp(a3, "live")) cp.SetTiming(CP::Timing::Live);
		else if (0 == _stricmp(a3, "freeze")) cp.SetTiming(CP::Timing::Freeze);
		else cp.ToggleTiming();
	}
	else if (0 == _stricmp(a, "speedmul")) {
		if (argc < 4) { advancedfx::Warning("usage: %s marker speedmul <0.2..1.0>\n", cmd); return; }
		cp.SetSelectedSpeedMul((float)atof(a3));
	}
	else if (0 == _stricmp(a, "constspeed")) {
		if (argc >= 4 && 0 == _stricmp(a3, "cycle")) cp.CycleConstSpeed();
		else if (argc >= 4) cp.SetConstSpeed((float)atof(a3));
		else cp.CycleConstSpeed();
	}
	else if (0 == _stricmp(a, "autosnap")) {
		if (0 == _stricmp(a3, "on")) cp.SetAutoSnap(true);
		else if (0 == _stricmp(a3, "off")) cp.SetAutoSnap(false);
		else cp.ToggleAutoSnap();
	}
	else if (0 == _stricmp(a, "list")) {
		advancedfx::Message("[campath] %d marker(s), selected #%d, speed=%s, timing=%s, interp=%s.\n",
			cp.Count(), cp.Selected(), cp.SpeedModeName(), cp.TimingName(), cp.InterpName());
	}
	else if (0 == _stricmp(a, "save")) cp.Save();
	else if (0 == _stricmp(a, "load")) cp.Load();
	else advancedfx::Warning("%s marker: unknown action '%s'\n", cmd, a);
}

} // namespace Filmmaker
