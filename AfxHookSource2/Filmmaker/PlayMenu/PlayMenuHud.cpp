#include "PlayMenuHud.h"

#include "PlayMenuStripJs.h"
#include "WorkshopFilterJs.h"
#include "WorkshopKeywordEditorJs.h"
#include "../Panorama/PanoramaBridge.h"
#include "../Panorama/PanoramaFindPanel.h"
#include "../Workshop/WorkshopOrganizer.h"

#include "../../DeathMsg.h" // AfxHookSource2_GetPanoramaMainMenuPanel

#include <string>
#include <vector>
#include <sstream>

namespace Filmmaker {

namespace {

PanoramaBridge g_bridge;
bool g_built = false;
bool g_buildAttempted = false;
void* g_host = nullptr;
short g_symCmd = -1;
short g_symWs = -1;
uint64_t g_lastWsVersion = (uint64_t)-1;
std::vector<std::string> g_evalQueue;

void* FindHost() {
	void* ctx = g_bridge.ContextPanel();
	if (!ctx)
		return nullptr;
	return FindChildById(ctx, "MvmPlayMenuHost");
}

void BuildUi() {
	g_buildAttempted = true;

	// Three fragments, injected in order: the namespace + tab strip, the workshop sidebar, then
	// the keyword editor. Split because MSVC caps a raw string literal at ~16380 bytes, and
	// because each one stands on its own -- fragment 2 works without 3, fragment 1 without
	// either. They compose through the shared $.MvmPlay object.
	if (!g_bridge.RunScript(kPlayMenuStripJs))
		return;
	// Spliced from two literals (the 16380-byte cap again), hence std::string, not a literal.
	g_bridge.RunScript(kWorkshopFilterJs.c_str());
	g_bridge.RunScript(kWorkshopKeywordEditorJs);

	g_host = FindHost();
	g_symCmd = g_bridge.MakeSymbol("cmd");
	g_symWs = g_bridge.MakeSymbol("ws");
	g_built = (g_host != nullptr);
	g_lastWsVersion = (uint64_t)-1; // force a data push
}

void PushWorkshop() {
	if (!g_host)
		g_host = FindHost();
	if (!g_host || g_symWs < 0)
		return;
	const std::string json = WorkshopOrganizer_BuildJson();
	g_bridge.SetAttributeString(g_host, g_symWs, json.c_str());
	g_bridge.RunScript("$.MvmPlay && $.MvmPlay.renderWs();");
}

void PollCommands() {
	if (!g_host || g_symCmd < 0)
		return;

	std::string queued = g_bridge.GetAttributeString(g_host, g_symCmd, "");
	if (queued.empty())
		return;
	g_bridge.SetAttributeString(g_host, g_symCmd, "");

	std::istringstream iss(queued);
	std::string line;
	while (std::getline(iss, line)) {
		while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
			line.pop_back();
		if (line.empty())
			continue;
		// Unknown lines are dropped rather than warned about: the JS is the only producer, so an
		// unknown line means a stale injected script, and spamming the console per frame would be
		// worse than silence.
		WorkshopOrganizer_HandleCommand(line);
	}
}

} // namespace

void PlayMenu_RequestRebuild() {
	g_built = false;
	g_buildAttempted = false;
	g_host = nullptr;
}

void PlayMenu_RequestEval(const std::string& js) {
	g_evalQueue.push_back(js);
}

void PlayMenu_RunMainThreadFrame() {
	g_bridge.Init();

	// The play page only exists in the main menu. While a demo/map is loaded the main-menu
	// panel is gone and so is everything we injected, so drop straight back to "not built" --
	// the next visit to the menu rebuilds. Pinning the context (rather than letting the bridge
	// auto-follow) keeps us from injecting the page script into the in-game HUD, where it would
	// create a host panel that never finds a Play page.
	unsigned char* mainMenu = AfxHookSource2_GetPanoramaMainMenuPanel();
	if (!mainMenu) {
		g_built = false;
		g_buildAttempted = false;
		g_host = nullptr;
		return;
	}
	g_bridge.SetContextPanel(mainMenu);

	if (!g_evalQueue.empty() && g_bridge.CanRunScript()) {
		for (const std::string& js : g_evalQueue)
			g_bridge.RunScript(js);
		g_evalQueue.clear();
	}

	if (!g_built) {
		if (!g_buildAttempted && g_bridge.CanRunScript())
			BuildUi();
		if (!g_built)
			return;
	}

	// Re-locate the host every frame. It can be dropped by a menu reload, and a stale cached
	// pointer would be a use-after-free -- the same reason FilmmakerMenu::RunFrame re-finds its
	// tab. If it has truly vanished, rebuild on the next frame.
	g_host = FindHost();
	if (!g_host) {
		g_built = false;
		g_buildAttempted = false;
		return;
	}

	const uint64_t wsVersion = WorkshopOrganizer_Version();
	if (wsVersion != g_lastWsVersion) {
		PushWorkshop();
		g_lastWsVersion = wsVersion;
	}


	PollCommands();
}

std::string PlayMenu_Describe() {
	std::ostringstream out;
	out << "play menu: " << (g_built ? "built" : "not built")
	    << ", host " << (g_host ? "found" : "missing")
	    << ", main menu " << (AfxHookSource2_GetPanoramaMainMenuPanel() ? "present" : "absent") << "\n";
	out << WorkshopOrganizer_Describe();
	return out.str();
}

} // namespace Filmmaker
