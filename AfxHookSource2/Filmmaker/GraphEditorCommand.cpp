// Console command surface for the experimental After-Effects-style graph editor:
// "mirv_filmmaker grapheditor ...". Entirely separate from the stable camtl/marker
// back-end: it drives GraphEditorExperimentHud's own isolated model. The JS overlay
// issues these; they are also bind-able for power users.
//
// Split out of FilmmakerCommand.cpp per the Cosmetics_RunCommand file-per-feature pattern
// (the RunCommand entry point is declared in the owning system's header -- here,
// Panorama/GraphEditorExperimentHud.h).

#include "Panorama/GraphEditorExperimentHud.h"
#include "Movie/FollowCamera.h"

#include "../../Shared/AfxConsole.h"

#include <cstdlib>
#include <cstring>
#include <string>

namespace Filmmaker {

void GraphEditor_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd) {
	auto& ge = Filmmaker::GraphEditorExperimentHudRef();
	if (argc < 3) {
		advancedfx::Message(
			"%s grapheditor on|off|toggle - EXPERIMENTAL AE-style camera graph editor (must be in a demo).\n"
			"%s grapheditor drive on|off|toggle - live-drive the camera from the experimental curves.\n"
			"%s grapheditor reseed | undo | redo | ease in|out|inout [all] | smooth|linear [sel] | align <ch> <id> | align playhead.\n"
			"%s grapheditor chan <0..6> show|hide|solo|unsolo  (0=x 1=y 2=z 3=pitch 4=yaw 5=roll 6=fov).\n"
			"%s grapheditor select <ch> <id> [add] | selset <ch> <id> [<ch> <id> ...] | selclear.\n"
			"%s grapheditor editbegin | movesel <dTick> <dVal> | movekey <ch> <id> <tick> <val>.\n"
			"%s grapheditor setval <ch> <id> <val> | addkey <ch> <tick> <val> | delkey <ch> <id> | delsel.\n"
			"%s grapheditor handle <ch> <id> left|right <tx> <dv> [reflect] | clearhandles <ch> <id>.\n"
			"%s grapheditor playhead <tick> | playhead release | key <shortcut> | eval <panorama js>.\n",
			cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd);
		return;
	}
	const char* a = args->ArgV(2);
	const char* a3 = (argc >= 4) ? args->ArgV(3) : "";
	const char* a4 = (argc >= 5) ? args->ArgV(4) : "";
	const char* a5 = (argc >= 6) ? args->ArgV(5) : "";
	const char* a6 = (argc >= 7) ? args->ArgV(6) : "";
	const char* a7 = (argc >= 8) ? args->ArgV(7) : "";
	const char* a8 = (argc >= 9) ? args->ArgV(8) : "";

	if (0 == _stricmp(a, "on") || 0 == _stricmp(a, "open") || 0 == _stricmp(a, "1")) {
		ge.SetEnabled(true);
		advancedfx::Message("mirv_filmmaker: experimental graph editor ON (must be in a demo).\n");
	} else if (0 == _stricmp(a, "off") || 0 == _stricmp(a, "close") || 0 == _stricmp(a, "0")) {
		ge.SetEnabled(false);
		advancedfx::Message("mirv_filmmaker: experimental graph editor off.\n");
	} else if (0 == _stricmp(a, "toggle")) {
		ge.Toggle();
		advancedfx::Message("mirv_filmmaker: experimental graph editor %s.\n", ge.Enabled() ? "ON" : "off");
	} else if (0 == _stricmp(a, "drive")) {
		if (0 == _stricmp(a3, "on") || 0 == _stricmp(a3, "1")) {
			Filmmaker::FollowCameraRef().StopPreview("graph camera drive started");
			ge.SetDrive(true);
		}
		else if (0 == _stricmp(a3, "off") || 0 == _stricmp(a3, "0")) ge.SetDrive(false);
		else {
			if (!ge.Drive()) Filmmaker::FollowCameraRef().StopPreview("graph camera drive started");
			ge.ToggleDrive();
		}
	} else if (0 == _stricmp(a, "reseed")) ge.CmdReseed();
	else if (0 == _stricmp(a, "undo")) ge.CmdUndo();
	else if (0 == _stricmp(a, "redo")) ge.CmdRedo();
	else if (0 == _stricmp(a, "ease")) {
		// Easing preset: in|out|inout. Default = current selection (right-click menu); trailing
		// "all" applies it to every keyframe (the inspector's path-wide Ease button).
		int mode = (0 == _stricmp(a3, "out")) ? 1 : (0 == _stricmp(a3, "inout") ? 2 : 0);
		ge.CmdEase(mode, 0 != _stricmp(a4, "all"));
	} else if (0 == _stricmp(a, "align")) {
		// Snap the whole selection to one tick (vertical alignment across channels):
		// "align <ch> <id>" uses that key's tick (the right-clicked key), "align playhead"
		// uses the current playhead tick.
		if (0 == _stricmp(a3, "playhead")) {
			ge.CmdAlignSelectionToPlayhead();
		}
		else {
			if (argc < 5) { advancedfx::Warning("usage: %s grapheditor align <ch> <id> | align playhead\n", cmd); return; }
			ge.CmdAlignSelection(atoi(a3), atoi(a4));
		}
	} else if (0 == _stricmp(a, "smooth") || 0 == _stricmp(a, "linear")) {
		// Graph-wide Smooth/Linear. ALSO sets CameraPath::Interp (one interpolation, two views --
		// the camera timeline scrubs through CamPathEval, which only knows that global mode).
		// Optional "sel" argument restricts it to the current selection and stays graph-local.
		bool sel = (0 == _stricmp(a3, "sel"));
		ge.CmdSetInterp(0 == _stricmp(a, "smooth"), sel);
	} else if (0 == _stricmp(a, "chan")) {
		if (argc < 5) { advancedfx::Warning("usage: %s grapheditor chan <0..6> show|hide|solo|unsolo\n", cmd); return; }
		int op = 1;
		if (0 == _stricmp(a4, "hide")) op = 0; else if (0 == _stricmp(a4, "show")) op = 1;
		else if (0 == _stricmp(a4, "solo")) op = 2; else if (0 == _stricmp(a4, "unsolo")) op = 3;
		ge.CmdChannel(atoi(a3), op);
	} else if (0 == _stricmp(a, "select")) {
		if (argc < 5) { advancedfx::Warning("usage: %s grapheditor select <ch> <id> [add]\n", cmd); return; }
		ge.CmdSelect(atoi(a3), atoi(a4), (argc >= 6 && 0 == _stricmp(a5, "add")));
	} else if (0 == _stricmp(a, "selall")) ge.CmdSelectAll();
	else if (0 == _stricmp(a, "selset")) {
		// Flat space-separated <ch> <id> pairs starting at ArgV(3). Uses the engine tokenizer
		// (one int per token) instead of a single "ch:id,ch:id" token, which the console
		// splits on the comma -- that dropped all but the first pair and broke box-select.
		// First pair replaces the selection (additive=false); the rest extend it.
		bool first = true;
		for (int i = 3; i + 1 < argc; i += 2) {
			ge.CmdSelect(atoi(args->ArgV(i)), atoi(args->ArgV(i + 1)), !first);
			first = false;
		}
	}
	else if (0 == _stricmp(a, "selclear")) ge.CmdSelectClear();
	else if (0 == _stricmp(a, "editbegin")) ge.CmdEditBegin();
	else if (0 == _stricmp(a, "movesel")) {
		if (argc < 5) { advancedfx::Warning("usage: %s grapheditor movesel <dTick> <dVal>\n", cmd); return; }
		ge.CmdMoveSelectedBy(atof(a3), atof(a4));
	} else if (0 == _stricmp(a, "movekey")) {
		if (argc < 7) { advancedfx::Warning("usage: %s grapheditor movekey <ch> <id> <tick> <val>\n", cmd); return; }
		ge.CmdMoveKeyAbs(atoi(a3), atoi(a4), atof(a5), atof(a6));
	} else if (0 == _stricmp(a, "setval")) {
		if (argc < 6) { advancedfx::Warning("usage: %s grapheditor setval <ch> <id> <val>\n", cmd); return; }
		ge.CmdSetValue(atoi(a3), atoi(a4), atof(a5));
	} else if (0 == _stricmp(a, "addkey")) {
		if (argc < 6) { advancedfx::Warning("usage: %s grapheditor addkey <ch> <tick> <val>\n", cmd); return; }
		ge.CmdAddKey(atoi(a3), atof(a4), atof(a5));
	} else if (0 == _stricmp(a, "delkey")) {
		if (argc < 5) { advancedfx::Warning("usage: %s grapheditor delkey <ch> <id>\n", cmd); return; }
		ge.CmdDeleteKey(atoi(a3), atoi(a4));
	} else if (0 == _stricmp(a, "delsel")) ge.CmdDeleteSelected();
	else if (0 == _stricmp(a, "clear")) ge.CmdClear();
	else if (0 == _stricmp(a, "handle")) {
		if (argc < 8) { advancedfx::Warning("usage: %s grapheditor handle <ch> <id> left|right <tx> <dv> [reflect]\n", cmd); return; }
		int side = (0 == _stricmp(a5, "left")) ? -1 : 1;
		bool reflect = (argc >= 9) ? (atoi(a8) != 0) : true;
		ge.CmdSetHandle(atoi(a3), atoi(a4), side, atof(a6), atof(a7), reflect);
	} else if (0 == _stricmp(a, "clearhandles")) {
		if (argc < 5) { advancedfx::Warning("usage: %s grapheditor clearhandles <ch> <id>\n", cmd); return; }
		ge.CmdClearHandles(atoi(a3), atoi(a4));
	} else if (0 == _stricmp(a, "playhead")) {
		if (argc >= 4 && 0 == _stricmp(a3, "release")) ge.CmdPlayhead(0.0, true);
		else if (argc >= 4) ge.CmdPlayhead(atof(a3), false);
		else advancedfx::Warning("usage: %s grapheditor playhead <tick> | release\n", cmd);
	} else if (0 == _stricmp(a, "key")) {
		// A keyboard shortcut for the TIMELINE dock (MovieMode forwards Delete, Ctrl+Z, Ctrl+A, F9,
		// Esc). The dock decides what it means: only it knows which model -- camera or scene -- holds
		// the selection and which one was edited last. The name is whitelisted, so this can never
		// become an arbitrary-script path.
		static const char* kKeys[] = { "delete", "undo", "redo", "selall", "easeinout", "easein", "easeout", "escape" };
		bool known = false;
		for (const char* k : kKeys) if (0 == _stricmp(a3, k)) known = true;
		if (!known) { advancedfx::Warning("usage: %s grapheditor key delete|undo|redo|selall|easeinout|easein|easeout|escape\n", cmd); return; }
		ge.RequestEval(std::string("$.GraphExp && $.GraphExp.key && $.GraphExp.key('") + a3 + "');");
	} else if (0 == _stricmp(a, "eval")) {
		if (argc < 4) { advancedfx::Warning("usage: %s grapheditor eval <panorama js>\n", cmd); return; }
		std::string js;
		for (int i = 3; i < argc; ++i) { if (i > 3) js += ' '; js += args->ArgV(i); }
		ge.RequestEval(js);
	} else advancedfx::Warning("%s grapheditor: unknown action '%s'\n", cmd, a);
}

} // namespace Filmmaker
