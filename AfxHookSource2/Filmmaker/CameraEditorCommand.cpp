// Console command surface for the dedicated camera-editor workspace: "mirv_filmmaker
// editor ...". Bind it to a key, e.g. bind "F9" "mirv_filmmaker editor toggle".
//
// Split out of FilmmakerCommand.cpp per the Cosmetics_RunCommand file-per-feature pattern
// (the RunCommand entry point is declared in the owning system's header -- here,
// Panorama/CameraEditorHud.h).

#include "Filmmaker.h"
#include "Movie/TimelineDock.h"
#include "FilmmakerCommandUtil.h"
#include "Panorama/CameraEditorHud.h"
#include "Panorama/CameraTimelineHud.h"
#include "Render/RenderCommand.h"

#include "../../Shared/AfxConsole.h"

#include <cstring>
#include <string>

namespace Filmmaker {

void CameraEditor_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd) {
	const char* arg = (argc >= 3) ? args->ArgV(2) : "toggle";
	if (0 == _stricmp(arg, "state")) {
		advancedfx::Message("[cameraeditor][state] %s\n",
			Filmmaker::CameraEditorHudRef().DebugStateJson().c_str());
	} else if (0 == _stricmp(arg, "state64")) {
		PrintEncodedState("[cameraeditor][state64]",
			Filmmaker::CameraEditorHudRef().DebugStateJson());
	} else if (0 == _stricmp(arg, "eval")) {
		if (argc < 4) { advancedfx::Warning("usage: %s editor eval <panorama js>\n", cmd); return; }
		std::string js;
		for (int i = 3; i < argc; ++i) { if (i > 3) js += ' '; js += args->ArgV(i); }
		Filmmaker::CameraEditorHudRef().RequestEval(js);
	} else if (0 == _stricmp(arg, "customize")) {
		Filmmaker::CameraEditorHudRef().RequestOpenCustomize();
		Filmmaker::CameraEditor_Set(true);
	} else if (0 == _stricmp(arg, "flashinspector")) {
		Filmmaker::CameraEditor_FlashInspector();
	} else if (0 == _stricmp(arg, "render")) {
		Filmmaker::Render_SetModalOpen(true);
		Filmmaker::CameraEditor_Set(true);
	} else if (0 == _stricmp(arg, "scale")) {
		// TRUE scaled preview viewport (render-layer). Falls back to the crop when off.
		const char* a2 = (argc >= 4) ? args->ArgV(3) : "toggle";
		if (0 == _stricmp(a2, "on") || 0 == _stricmp(a2, "1")) Filmmaker::CameraEditor_SetScale(true);
		else if (0 == _stricmp(a2, "off") || 0 == _stricmp(a2, "0")) Filmmaker::CameraEditor_SetScale(false);
		else Filmmaker::CameraEditor_ToggleScale();
		advancedfx::Message("mirv_filmmaker: camera editor scaled preview %s (auto-off while recording).\n",
			Filmmaker::CameraEditor_ScaleActive() ? "ON" : "off");
	} else if (0 == _stricmp(arg, "curveeditor")) {
		// ALIAS. The dock is shared with the CONFIG panel now, so its canonical verb is
		// `mirv_filmmaker dock <tab>` (Filmmaker::TimelineDock_RunCommand). This spelling stays
		// because it is in people's binds; both go through the same parser, so they cannot drift.
		const char* a2 = (argc >= 4) ? args->ArgV(3) : "toggle";
		Filmmaker::TimelineDock_Pick(a2);
		if (Filmmaker::TimelineDock_Tab() != Filmmaker::DockTab::Native)
			FocusEditorCameraIfAny();
	} else if (0 == _stricmp(arg, "hud")) {
		// Game-HUD visibility behind the editor: hide all / in-game (radar + HP/ammo, no
		// spectator observer panel) / show all (full spectator HUD).
		using HV = Filmmaker::CameraEditorHud::HudView;
		const char* a2 = (argc >= 4) ? args->ArgV(3) : "cycle";
		if (0 == _stricmp(a2, "hidden") || 0 == _stricmp(a2, "hideall") || 0 == _stricmp(a2, "hide") || 0 == _stricmp(a2, "none"))
			Filmmaker::CameraEditorHudRef().SetHudView(HV::HideAll);
		else if (0 == _stricmp(a2, "game") || 0 == _stricmp(a2, "ingame") || 0 == _stricmp(a2, "in-game"))
			Filmmaker::CameraEditorHudRef().SetHudView(HV::InGame);
		else if (0 == _stricmp(a2, "full") || 0 == _stricmp(a2, "showall") || 0 == _stricmp(a2, "show") || 0 == _stricmp(a2, "all"))
			Filmmaker::CameraEditorHudRef().SetHudView(HV::ShowAll);
		else Filmmaker::CameraEditorHudRef().CycleHudView();
		advancedfx::Message("mirv_filmmaker: editor game-HUD = %s.\n", Filmmaker::CameraEditor_HudViewName());
	} else if (0 == _stricmp(arg, "debug")) {
		// Viewport/HUD debug overlay: on-screen window/render-target/viewport readout so the
		// custom editor viewport can be compared 1:1 against the normal game viewport.
		const char* a2 = (argc >= 4) ? args->ArgV(3) : "toggle";
		if (0 == _stricmp(a2, "on") || 0 == _stricmp(a2, "1")) Filmmaker::CameraEditorHudRef().SetDebugOverlay(true);
		else if (0 == _stricmp(a2, "off") || 0 == _stricmp(a2, "0")) Filmmaker::CameraEditorHudRef().SetDebugOverlay(false);
		else Filmmaker::CameraEditorHudRef().ToggleDebugOverlay();
		advancedfx::Message("mirv_filmmaker: editor debug overlay %s.\n",
			Filmmaker::CameraEditorHudRef().DebugOverlay() ? "ON" : "off");
	} else {
		if (0 == _stricmp(arg, "on") || 0 == _stricmp(arg, "open") || 0 == _stricmp(arg, "1")) Filmmaker::CameraEditor_Set(true);
		else if (0 == _stricmp(arg, "off") || 0 == _stricmp(arg, "close") || 0 == _stricmp(arg, "0")) Filmmaker::CameraEditor_Set(false);
		else Filmmaker::CameraEditor_Toggle();
		advancedfx::Message("mirv_filmmaker: camera editor mode %s (must be in a demo).\n",
			Filmmaker::CameraEditor_Active() ? "ON" : "off");
	}
}

} // namespace Filmmaker
