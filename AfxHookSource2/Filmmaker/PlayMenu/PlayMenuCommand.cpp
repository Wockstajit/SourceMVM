// Console command surface for the Play-screen takeover: "mirv_filmmaker play ...".
//
// Split out of FilmmakerCommand.cpp per the file-per-feature pattern (the RunCommand entry
// point is declared in the owning system's header -- here, PlayMenu/PlayMenuHud.h).
//
// The eval verb is the live-verification path: netcon's ui_eval truncates at 256 bytes, so
// checks go through the short report helpers the injected JS defines rather than inline JS.
//   mirv_filmmaker play eval $.Msg($.MvmPlay.report())
//   mirv_filmmaker play eval $.Msg($.MvmPlay.wsReport())
//   mirv_filmmaker play eval $.Msg($.MvmPlay.wsOrder(5))

#include "PlayMenuHud.h"
#include "../Workshop/WorkshopOrganizer.h"

#include "../../../Shared/AfxConsole.h"

#include <string>
#include <cstring>

namespace Filmmaker {

void PlayMenu_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd) {
	const char* sub = (argc >= 3) ? args->ArgV(2) : "status";

	if (0 == _stricmp(sub, "status")) {
		advancedfx::Message("%s", PlayMenu_Describe().c_str());
		return;
	}

	if (0 == _stricmp(sub, "rebuild")) {
		PlayMenu_RequestRebuild();
		advancedfx::Message("%s play: rebuild requested.\n", cmd);
		return;
	}

	if (0 == _stricmp(sub, "rescan")) {
		WorkshopOrganizer_Rescan();
		advancedfx::Message("%s", WorkshopOrganizer_Describe().c_str());
		return;
	}

	if (0 == _stricmp(sub, "eval")) {
		if (argc < 4) {
			advancedfx::Warning("usage: %s play eval <panorama js>\n", cmd);
			return;
		}
		std::string js;
		for (int i = 3; i < argc; ++i) {
			if (i > 3)
				js += ' ';
			js += args->ArgV(i);
		}
		PlayMenu_RequestEval(js);
		advancedfx::Message("%s play: queued eval (%zu chars).\n", cmd, js.size());
		return;
	}

	if (0 == _stricmp(sub, "ws")) {
		// Drives the organiser directly, bypassing the UI -- useful for verifying the store and
		// the payload without clicking: e.g. "mirv_filmmaker play ws sort installed".
		if (argc < 4) {
			advancedfx::Warning("usage: %s play ws <sort|hide|cat|words|reset|rescan> ...\n", cmd);
			return;
		}
		std::string line = "ws";
		for (int i = 3; i < argc; ++i) {
			line += ' ';
			line += args->ArgV(i);
		}
		WorkshopOrganizer_HandleCommand(line);
		advancedfx::Message("%s", WorkshopOrganizer_Describe().c_str());
		return;
	}

	advancedfx::Message(
		"%s play status - Play-page takeover + workshop organiser state.\n"
		"%s play rebuild - re-inject the Play-page script.\n"
		"%s play rescan - re-read the Steam workshop content folder.\n"
		"%s play ws <sort|hide|cat|words|reset|rescan> ... - drive the organiser from the console.\n"
		"%s play eval <js> - run Panorama JS in the main-menu context.\n",
		cmd, cmd, cmd, cmd, cmd);
}

} // namespace Filmmaker
