// Console command surface for camera markers / dolly path: "mirv_filmmaker marker ...".
// Mirrors the BO2 Theater dolly workflow; also the back-end the K/J/L/F hotkeys + the
// Panorama marker menu issue. All actions run on the main thread (console dispatch).
//
// Split out of FilmmakerCommand.cpp per the Cosmetics_RunCommand file-per-feature pattern
// (the RunCommand entry point is declared in the owning system's header -- here,
// Movie/CameraPath.h -- not a dedicated command header).

#include "Movie/CameraPath.h"
#include "Movie/CameraBridge.h"
#include "Movie/DollyZoomMath.h"
#include "Movie/FollowCamera.h"
#include "Cosmetics/CosmeticEntityAccess.h" // CosmeticEntFromIndex: the players a dolly zoom can hold
#include "Debug/MvmDebugLog.h"
#include "Panorama/CameraEditorHud.h"

#include "../ClientEntitySystem.h"
#include "../MirvTime.h"
#include "../../Shared/AfxConsole.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

namespace Filmmaker {

namespace {

void ReportDollyZoom(const std::string& reason, int count = 0, int first = 0, int last = 0, bool fallback = false) {
	auto& editor = CameraEditorHudRef();
	if (!editor.Enabled()) return;
	std::ostringstream js;
	js << "$.CamEditor && $.CamEditor.dollyResult(" << count << ',' << first << ',' << last << ','
		<< (fallback ? "true" : "false") << ',' << std::quoted(reason) << ");";
	editor.RequestEval(js.str());
}

// The nearest live player near the middle of the frame, measured at the chest (48 units over the
// feet). SEH-guarded like every other pawn scan: a stale entity reads as nobody. -1 when none.
double NearestPlayerAhead(const DollyZoom::Pose& cam) {
	double best = -1;
	__try {
		const int highest = GetHighestEntityIndex();
		for (int i = 1; i <= highest; ++i) {
			CEntityInstance* ent = CosmeticEntFromIndex(i);
			if (!ent || !ent->IsPlayerPawn() || ent->GetHealth() <= 0) continue;
			float x = 0, y = 0, z = 0;
			ent->GetOrigin(x, y, z);
			const double chest[3] = { x, y, z + 48.0 };
			const double d = DollyZoom::SubjectDistance(cam, chest);
			if (d > 0 && (best < 0 || d < best)) best = d;
		}
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return -1;
	}
	return best;
}

// `marker dollyzoom [in|out] [seconds] [move|<n>%] [subject distance]`: keys from the current view and
// demo tick onward. The keys replace any marker inside the shot's span; markers outside it stay.
void DollyZoomCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd) {
	auto arg = [&](int i) -> const char* { return i < argc ? args->ArgV(i) : ""; };
	int at = 3;
	bool out = false;
	if (0 == _stricmp(arg(at), "in")) ++at;
	else if (0 == _stricmp(arg(at), "out")) { out = true; ++at; }
	DollyZoom::Options o;
	if (*arg(at)) o.seconds = atof(arg(at++));
	// The move in units, or with a trailing % as a share of the way to the subject.
	double move = -1, movePercent = -1;
	if (*arg(at)) {
		const std::string m = arg(at++);
		if (!m.empty() && m.back() == '%') movePercent = atof(m.c_str());
		else move = atof(m.c_str());
	}
	const double subject = *arg(at) ? atof(arg(at++)) : -1;
	int tick = 0;
	if (!g_MirvTime.GetCurrentDemoTick(tick)) {
		ReportDollyZoom("Open a demo to create a dolly zoom.");
		advancedfx::Warning("%s marker dollyzoom: open a demo first.\n", cmd);
		return;
	}

	double origin[3] = {}, angles[3] = {}, fov = 90;
	CameraBridge_GetCurrentCamera(origin, angles, fov);
	DollyZoom::Pose start;
	start.x = origin[0]; start.y = origin[1]; start.z = origin[2];
	start.pitch = angles[0]; start.yaw = angles[1]; start.roll = angles[2];
	start.fov = fov;

	const char* subjectFrom = "given";
	bool fallback = false;
	if (subject > 0) o.subjectDistance = subject;
	else {
		const double d = NearestPlayerAhead(start);
		if (d > 0) { o.subjectDistance = d; subjectFrom = "the nearest player ahead"; }
		else {
			o.subjectDistance = 300;
			subjectFrom = "no player near the middle of the frame: 300 units";
			fallback = true;
		}
	}
	// Half the way to the subject unless a move was given: a strong, safe default.
	if (movePercent > 0) move = o.subjectDistance * (std::min)(movePercent, 95.0) / 100.0;
	if (move <= 0) move = o.subjectDistance * 0.5;
	o.move = out ? -move : move;
	o.startTick = tick;
	const double interval = (double)g_MirvTime.interval_per_tick_get();
	o.tickRate = interval > 0 ? 1.0 / interval : 64.0;

	std::string why;
	const std::vector<DollyZoom::Key> keys = DollyZoom::Build(start, o, &why);
	if (keys.empty()) {
		ReportDollyZoom(why);
		advancedfx::Warning("%s marker dollyzoom: %s.\n", cmd, why.c_str());
		return;
	}
	CameraPath& cp = CameraPathRef();
	const int first = keys.front().tick, last = keys.back().tick;
	std::vector<CamMarker> markers;
	for (const CamMarker& m : cp.Markers())
		if (m.tick < first || m.tick > last) markers.push_back(m);
	for (const DollyZoom::Key& k : keys) {
		CamMarker m;
		m.x = k.pose.x; m.y = k.pose.y; m.z = k.pose.z;
		m.pitch = k.pose.pitch; m.yaw = k.pose.yaw; m.roll = k.pose.roll; m.fov = k.pose.fov;
		m.tick = k.tick;
		m.time = k.tick * interval;
		markers.push_back(m);
	}
	std::stable_sort(markers.begin(), markers.end(), [](const CamMarker& a, const CamMarker& b) { return a.tick < b.tick; });
	cp.ReplaceMarkersFromGraph(markers, true);
	// The keys carry the timing: the shot plays at the ticks they sit on.
	if (cp.GetSpeedMode() != CameraPath::SpeedMode::Manual) cp.SetSpeedMode(CameraPath::SpeedMode::Manual);
	ReportDollyZoom(why, (int)keys.size(), first, last, fallback);
	advancedfx::Message("%s marker dollyzoom: %s %.0f units over %.1f s, subject at %.0f units (%s), fov %.1f -> %.1f, %d keys at ticks %d..%d%s%s.\n",
		cmd, out ? "pull out" : "push in", std::fabs(o.move), o.seconds, o.subjectDistance, subjectFrom,
		keys.front().pose.fov, keys.back().pose.fov, (int)keys.size(), first, last, why.empty() ? "" : "; ", why.c_str());
	MvmDebugLog_LinefAlways("campath.dollyzoom", "dir=%s seconds=%.2f move=%.1f subject=%.1f (%s) fov=%.2f->%.2f keys=%zu ticks=%d..%d pos0=(%.1f %.1f %.1f) ang=(%.1f %.1f %.1f) note='%s'",
		out ? "out" : "in", o.seconds, o.move, o.subjectDistance, subjectFrom, keys.front().pose.fov, keys.back().pose.fov,
		keys.size(), first, last, start.x, start.y, start.z, start.pitch, start.yaw, start.roll, why.c_str());
}

} // namespace

void Marker_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd) {
	using CP = Filmmaker::CameraPath;
	Filmmaker::CameraPath& cp = Filmmaker::CameraPathRef();

	if (argc < 3) {
		advancedfx::Message(
			"%s marker place - add a marker at the current camera pose (also K).\n"
			"%s marker preview|arm - jump to first marker and arm playback.\n"
			"%s marker play|previewplay - play the armed path.\n"
			"%s marker playfromstart - jump to the first marker and play once there (J).\n"
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
			"%s marker list | save | load.\n"
			"%s marker dollyzoom [in|out] [seconds] [move|<n>%%] [subject distance] - a dolly zoom from the\n"
			"   current view and tick: the camera travels while the FOV holds the subject's size\n"
			"   (default: 3 s, half the way to the nearest player near the middle of the frame).\n",
			cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd);
		return;
	}

	const char* a = args->ArgV(2);
	const char* a3 = (argc >= 4) ? args->ArgV(3) : "";

	if (0 == _stricmp(a, "place")) cp.PlaceMarker();
	else if (0 == _stricmp(a, "preview") || 0 == _stricmp(a, "arm")) cp.ArmPreview();
	else if (0 == _stricmp(a, "playfromstart")) {
		// J starts the path like `play` does, so it gives the view to the path the same way: a
		// Follow Camera preview left running would drive the camera alongside it.
		Filmmaker::FollowCameraRef().StopPreview("camera path started");
		cp.PlayFromFirstKey();
	}
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
	else if (0 == _stricmp(a, "dollyzoom")) DollyZoomCommand(argc, args, cmd);
	else advancedfx::Warning("%s marker: unknown action '%s'\n", cmd, a);
}

} // namespace Filmmaker
