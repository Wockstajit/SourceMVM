// Console command surface for the camera timeline: "mirv_filmmaker camtl ...". The
// Panorama panel (CameraTimelineHud) issues these; they also work from the console. Panel
// show/hide go to the HUD; data ops go to CameraPath.
//
// Split out of FilmmakerCommand.cpp per the Cosmetics_RunCommand file-per-feature pattern
// (the RunCommand entry point + the shared FocusEditorCameraIfAny() helper are declared in
// the owning system's header -- here, Panorama/CameraTimelineHud.h).

#include "Filmmaker.h"
#include "MouseCapture.h"
#include "Panorama/CameraTimelineHud.h"
#include "Panorama/GraphEditorExperimentHud.h" // MirrorPathInterp: keep the graph's handles in step
#include "Panorama/ConfigHud.h" // ConfigHud_Enabled: the demo Config panel owns the cursor too
#include "Movie/CameraPath.h"
#include "Movie/FollowCamera.h"
#include "Movie/MovieMode.h"

#include "../../Shared/AfxConsole.h"

#include <cstdlib>
#include <cstring>

namespace Filmmaker {

namespace {

void PlayCameraTimeline(Filmmaker::CameraPath& cp) {
	Filmmaker::FollowCameraRef().StopPreview("camera path started");
	if (Filmmaker::CameraEditor_Active())
		cp.PlayFromEditor();
	else
		cp.PlayFromTimeline();
}

} // namespace

// SELECTION ONLY -- deliberately not SelectForEditor.
//
// Every caller of this is "a panel became visible" or "the bottom editor tool changed":
// camtl open and camtl toggle. None of them is the user asking
// to go to a camera. SelectForEditor is a full navigation action -- it pauses the demo, issues
// demo_gototick, forces free cam, teleports the viewport to the keyframe and latches scrub -- so
// merely showing the timeline or flipping between the Camera Timeline and Graph views yanked the
// view onto a marker. That is the report's "switching editor tools" and "finishing an edit"
// triggers, and it violates its own rule that camera paths are opt-in.
//
// CameraEditorHud::OnEnter already established the right precedent for exactly this case:
// pre-select a key so the inspector has something to read, teleport=false, so the current view
// and camera mode are left as they are. Going to a camera stays an explicit action (nav arrows,
// preview, edit -> SelectForEditor).
bool FocusEditorCameraIfAny() {
	Filmmaker::CameraPath& cp = Filmmaker::CameraPathRef();
	if (cp.Count() <= 0)
		return false;
	cp.SelectIndex(cp.Selected() >= 0 ? cp.Selected() : 0, /*teleport*/ false);
	return true;
}

void CameraTimeline_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd) {
	using CP = Filmmaker::CameraPath;
	Filmmaker::CameraTimelineHud& tl = Filmmaker::CameraTimelineHudRef();
	Filmmaker::CameraPath& cp = Filmmaker::CameraPathRef();

	if (argc < 3) {
		advancedfx::Message(
			"%s camtl open|close|toggle - show/hide the camera timeline panel.\n"
			"%s camtl scrub <tick> - tick-perfect scrub to <tick> (paused).\n"
			"%s camtl play - editor: seek to playhead and play; timeline: play from first marker.\n"
			"%s camtl playtest - seek to the current editor tick and play immediately.\n"
			"%s camtl playpath - play demo normally; Live dolly engages inside marker range.\n"
			"%s camtl pause - pause camera-path playback at the current tick.\n"
			"%s camtl stop - stop any lingering path playback and scrub.\n"
			"%s camtl addkey | delkey <i> | movekey <i> <tick>.\n"
			"%s camtl setval <i> <ch 0..6> <v> - 0=x 1=y 2=z 3=pitch 4=yaw 5=tilt 6=fov.\n"
			"%s camtl undo - undo the last curve value/retime edit (Ctrl+Z).\n"
			"%s camtl ease <i> none|in|out|inout  |  speed <i> <0.2..1.0>.\n"
			"%s camtl interp linear|cubic|cycle.\n"
			"%s camtl cursor on|off|toggle - UI-mouse mode in the Camera Editor and Config.\n"
			"%s camtl clear - remove ALL keyframes.\n"
			"%s camtl eval <panorama js>.\n",
			cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd);
		return;
	}

	const char* a = args->ArgV(2);
	const char* a3 = (argc >= 4) ? args->ArgV(3) : "";
	const char* a4 = (argc >= 5) ? args->ArgV(4) : "";
	const char* a5 = (argc >= 6) ? args->ArgV(5) : "";

	if (0 == _stricmp(a, "open")) {
		tl.SetVisible(true);
		FocusEditorCameraIfAny();
		advancedfx::Message("mirv_filmmaker: camera timeline shown (must be in a demo).\n");
	}
	else if (0 == _stricmp(a, "close")) { tl.SetVisible(false); cp.StopScrub(); }
	else if (0 == _stricmp(a, "toggle")) {
		const bool opening = !tl.Visible();
		tl.Toggle();
		if (opening) {
			FocusEditorCameraIfAny();
		}
		else if (!opening) cp.StopScrub();
	}
	else if (0 == _stricmp(a, "view")) {
		tl.SetView(0);
		if (0 == _stricmp(a3, "curve"))
			advancedfx::Warning("%s camtl view curve is not available; use %s dock timeline (GRAPH view).\n", cmd, cmd);
	}
	else if (0 == _stricmp(a, "scrub")) { // slider release: seek the world too
		if (argc < 4) { advancedfx::Warning("usage: %s camtl scrub <tick>\n", cmd); return; }
		cp.ScrubToTick(atof(a3), true);
	}
	else if (0 == _stricmp(a, "scrubpreview")) { // slider drag: glide the camera only (no seek)
		if (argc < 4) { advancedfx::Warning("usage: %s camtl scrubpreview <tick>\n", cmd); return; }
		cp.ScrubToTick(atof(a3), false);
	}
	else if (0 == _stricmp(a, "select")) {
		if (argc < 4) { advancedfx::Warning("usage: %s camtl select <i>\n", cmd); return; }
		cp.SelectForEditor(atoi(a3));
	}
	else if (0 == _stricmp(a, "selectdelta")) {
		cp.SelectEditorDelta((argc >= 4) ? atoi(a3) : 1);
	}
	else if (0 == _stricmp(a, "play")) PlayCameraTimeline(cp);
	else if (0 == _stricmp(a, "playtest")) {
		Filmmaker::FollowCameraRef().StopPreview("camera path started");
		cp.PlayFromEditor();
	}
	else if (0 == _stricmp(a, "playfrom")) { // a timeline click during playback: carry on from there
		if (argc < 4) { advancedfx::Warning("usage: %s camtl playfrom <tick>\n", cmd); return; }
		Filmmaker::FollowCameraRef().StopPreview("camera path started");
		cp.PlayFromEditor(atoi(a3));
	}
	else if (0 == _stricmp(a, "playtimeline")) {
		Filmmaker::FollowCameraRef().StopPreview("camera path started");
		cp.PlayFromTimeline();
	}
	else if (0 == _stricmp(a, "playpath")) {
		Filmmaker::FollowCameraRef().StopPreview("camera path started");
		cp.PlayPath();
	}
	else if (0 == _stricmp(a, "pause")) cp.PausePreview();
	else if (0 == _stricmp(a, "debug")) {
		bool on = (argc >= 4) ? (atoi(a3) != 0) : !cp.Debug();
		cp.SetDebug(on);
		advancedfx::Warning("%s camtl debug %d ([campath]/[setupview] instrumentation %s).\n",
			cmd, on ? 1 : 0, on ? "ON" : "OFF");
	}
	else if (0 == _stricmp(a, "stop")) { cp.StopPreview(); cp.StopScrub(); }
	else if (0 == _stricmp(a, "autopath")) { // the camera path layer's AUTO button
		const char* how = (argc >= 4) ? args->ArgV(3) : "toggle";
		const bool on = 0 == _stricmp(how, "on") || (0 != _stricmp(how, "off") && !Filmmaker::CameraPath_AutoPath());
		Filmmaker::CameraPath_SetAutoPath(on);
		advancedfx::Message("mirv_filmmaker: AUTO %s -- %s\n", on ? "on" : "off", on
			? "while the timeline plays, the view follows the camera path inside its keys."
			: "Space plays the demo; J plays the camera path.");
	}
	else if (0 == _stricmp(a, "addkey")) cp.PlaceMarker();
	else if (0 == _stricmp(a, "delkey")) {
		if (argc < 4) { advancedfx::Warning("usage: %s camtl delkey <i>\n", cmd); return; }
		cp.DeleteIndex(atoi(a3));
	}
	else if (0 == _stricmp(a, "movekey")) {
		if (argc < 5) { advancedfx::Warning("usage: %s camtl movekey <i> <tick>\n", cmd); return; }
		cp.MoveKey(atoi(a3), atoi(a4));
	}
	else if (0 == _stricmp(a, "setval")) {
		if (argc < 6) { advancedfx::Warning("usage: %s camtl setval <i> <ch> <v>\n", cmd); return; }
		cp.SetChannelValue(atoi(a3), atoi(a4), atof(a5));
	}
	else if (0 == _stricmp(a, "editbegin")) cp.BeginCurveValueEdit();
	else if (0 == _stricmp(a, "setvalpreview")) {
		if (argc < 6) { advancedfx::Warning("usage: %s camtl setvalpreview <i> <ch> <v>\n", cmd); return; }
		cp.PreviewChannelValue(atoi(a3), atoi(a4), atof(a5));
	}
	else if (0 == _stricmp(a, "editend")) cp.EndCurveValueEdit();
	else if (0 == _stricmp(a, "undo")) cp.UndoCurveEdit();
	else if (0 == _stricmp(a, "redo")) cp.RedoCurveEdit();
	else if (0 == _stricmp(a, "ease")) {
		if (argc < 5) { advancedfx::Warning("usage: %s camtl ease <i> none|in|out|inout\n", cmd); return; }
		Filmmaker::Ease e = Filmmaker::Ease::None;
		if (0 == _stricmp(a4, "in")) e = Filmmaker::Ease::In;
		else if (0 == _stricmp(a4, "out")) e = Filmmaker::Ease::Out;
		else if (0 == _stricmp(a4, "inout")) e = Filmmaker::Ease::InOut;
		cp.SetEaseIndex(atoi(a3), e);
	}
	else if (0 == _stricmp(a, "speed")) {
		if (argc < 5) { advancedfx::Warning("usage: %s camtl speed <i> <0.2..1.0>\n", cmd); return; }
		cp.SetSpeedMulIndex(atoi(a3), (float)atof(a4));
	}
	else if (0 == _stricmp(a, "interp")) {
		if (0 == _stricmp(a3, "linear")) cp.SetInterp(CP::Interp::Linear);
		else if (0 == _stricmp(a3, "cubic") || 0 == _stricmp(a3, "smooth") || 0 == _stricmp(a3, "bezier"))
			cp.SetInterp(CP::Interp::Bezier);
		else cp.CycleInterp();
		// One interpolation, two views: reshape the graph's Bezier handles to the mode just set,
		// so flipping Linear/Smooth here and then opening the graph shows the same curve the
		// camera flies. (Safe no-op when the graph was never seeded.)
		Filmmaker::GraphEditorExperimentHudRef().MirrorPathInterp(cp.GetInterp() == CP::Interp::Bezier);
	}
	else if (0 == _stricmp(a, "zoom") || 0 == _stricmp(a, "pan")) {
		advancedfx::Warning("%s camtl %s is not available; use %s dock timeline.\n", cmd, a, cmd);
	}
	else if (0 == _stricmp(a, "gamehud")) {
		advancedfx::Message("mirv_filmmaker: camera timeline HUD toggle is disabled.\n");
	}
	else if (0 == _stricmp(a, "cursor")) {
		// UI-mouse mode belongs to the Camera Editor and Config workspaces, where the cursor flips
		// between clicking the panel and flying the free cam. Plain demo viewing has none: CS2's own
		// cameras give the cursor back by themselves when a look-around drag ends.
		if (!tl.EditorHosted() && !ConfigHud_Enabled()) {
			tl.SetCursor(false);
			advancedfx::Message("mirv_filmmaker: the UI mouse is only used in the Camera Editor and Config.\n");
			return;
		}
		if (tl.CursorForced()) {
			advancedfx::Message("mirv_filmmaker: cursor is forced on while the camera timeline is open.\n");
			return;
		}
		bool nextCursor = tl.Cursor();
		if (0 == _stricmp(a3, "on")) nextCursor = true;
		else if (0 == _stricmp(a3, "off")) nextCursor = false;
		else nextCursor = !nextCursor;
		if (Filmmaker::CameraEditor_Active())
			Filmmaker::CameraEditor_SetCursorMode(nextCursor);
		else {
			tl.SetCursor(nextCursor);
			if (nextCursor) MouseCapture_ReleaseForUi("cursor on"); // Config: same hand-back as the editor
		}
		// Returning to free-cam look (cursor off) releases any active scrub so the
		// camera stops being pinned to the scrub tick and the user can fly again.
		if (!nextCursor) {
			cp.StopScrub();
			// Back to flying: the free cam needs the mouse (see MouseCapture_TakeForCamera).
			MouseCapture_TakeForCamera("cursor off");
		}
	}
	else if (0 == _stricmp(a, "clear")) cp.DeleteAll(true); // remove all keyframes
	else if (0 == _stricmp(a, "eval")) {
		if (argc < 4) { advancedfx::Warning("usage: %s camtl eval <panorama js>\n", cmd); return; }
		std::string js;
		for (int i = 3; i < argc; ++i) { if (i > 3) js += ' '; js += args->ArgV(i); }
		tl.RequestEval(js);
	}
	else advancedfx::Warning("%s camtl: unknown action '%s'\n", cmd, a);
}

} // namespace Filmmaker
