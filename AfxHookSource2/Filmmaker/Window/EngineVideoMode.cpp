#include "EngineVideoMode.h"

#include "../Filmmaker.h"
#include "../Panorama/FilmmakerMenu.h"
#include "../Panorama/PanoramaBridge.h"
#include "../Debug/MvmDebugLog.h"

#include <cstdio>

namespace Filmmaker {

bool EngineVideoMode_Available() {
	PanoramaBridge& bridge = Menu().Bridge();
	return bridge.CanRunScript() && bridge.ContextPanel() != nullptr;
}

bool EngineVideoMode_Apply(int width, int height, std::string& outError) {
	outError.clear();

	if (width <= 0 || height <= 0) {
		outError = "invalid size";
		return false;
	}
	if (!EngineVideoMode_Available()) {
		outError = "Panorama bridge not ready (no UI context yet)";
		return false;
	}

	// MEASURED 2026-08-07 (CS2 build 2000879): these two writes DO NOT LAND. The game answers
	//     Failed to find ui preference 'setting.defaultres'
	//     Failed to find ui preference 'setting.defaultresheight'
	// on the console. `setting.defaultres` is real, but it lives in the VIDEO CONFIG
	// (Steam userdata .../730/local/cfg/cs2_video.txt) and SetSettingString writes the separate
	// "ui preference" store. It is not a convar either -- `find defaultres` returns no results.
	//
	// That mattered enormously, because dispatching CSGOApplyVideoSettings after a failed write is
	// not a no-op: it applies the settings the engine ALREADY has, rebuilding every scene render
	// target at the stored resolution and dragging the window back to it. A resize committed this
	// way visibly snapped back to where it started -- the reported bug.
	//
	// So the dispatch is now GUARDED: the values are read back, and the event only fires if they
	// actually took. If GetSettingString is missing too, the catch swallows it and nothing is
	// dispatched, which is the safe direction. Should a future CS2 build expose the key, this
	// starts working on its own -- and until then the caller falls back to the SDL path, which is
	// the route that was measured to work.
	//
	// Wrapped in try/catch because a Panorama exception aborts the whole script, and this runs on
	// the game's UI thread -- an API rename after a CS2 update must degrade to "nothing happened",
	// not to a broken UI frame. GameInterfaceAPI is a global in every Panorama context.
	char js[640];
	snprintf(js, sizeof(js),
		"(function(){try{"
		"GameInterfaceAPI.SetSettingString('setting.defaultres','%d');"
		"GameInterfaceAPI.SetSettingString('setting.defaultresheight','%d');"
		"if(GameInterfaceAPI.GetSettingString('setting.defaultres')!='%d')return;"
		"if(GameInterfaceAPI.GetSettingString('setting.defaultresheight')!='%d')return;"
		"$.DispatchEvent('CSGOApplyVideoSettings');"
		"}catch(e){}})();",
		width, height, width, height);

	const bool ok = Menu().Bridge().RunScript(js);
	if (!ok)
		outError = "RunScript failed";

	if (MvmDebugLog_Active())
		MvmDebugLog_LinefAlways("window.videomode",
			"apply %dx%d runScript=%d (guarded setting.defaultres write; on this build the write "
			"fails and the CSGOApplyVideoSettings dispatch is skipped -- expect no size change)",
			width, height, ok ? 1 : 0);

	return ok;
}

bool EngineVideoMode_LeaveFullscreen(std::string& outError) {
	outError.clear();
	if (!EngineVideoMode_Available()) {
		outError = "Panorama bridge not ready (no UI context yet)";
		return false;
	}

	// The control and its handler come from reference/cs2 refs/layout/settings/settings_video.xml:
	// DisplayModeEnum options are displaymode0 = Windowed, 1 = Fullscreen, 2 = Fullscreen Windowed,
	// and its oninputsubmit is CSGODisplayModeSelectionChanged(). In the XML that is a Panorama EVENT:
	// as a JS function it is `undefined` here (measured 2026-10-13 -- the first version called it
	// directly, threw a ReferenceError the catch swallowed, and never reached the apply). Dispatched
	// as an event it works: the engine went Windowed with the game still responsive. A missing
	// control (the Video page was never opened) makes the script a no-op, and the caller times out
	// with a clear message.
	const char* js =
		"(function(){try{"
		"var p=$.GetContextPanel();while(p&&p.GetParent())p=p.GetParent();"
		"var dm=p&&p.FindChildTraverse('DisplayModeEnum');if(!dm)return;"
		"dm.SetSelected('displaymode0');"
		"$.DispatchEvent('CSGODisplayModeSelectionChanged');"
		"$.DispatchEvent('CSGOApplyVideoSettings');"
		"}catch(e){}})();";

	const bool ok = Menu().Bridge().RunScript(js);
	if (!ok)
		outError = "RunScript failed";
	if (MvmDebugLog_Active())
		MvmDebugLog_LinefAlways("window.videomode", "leave fullscreen: runScript=%d", ok ? 1 : 0);
	return ok;
}

} // namespace Filmmaker
