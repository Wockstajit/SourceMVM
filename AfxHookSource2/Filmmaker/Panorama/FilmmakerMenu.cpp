#include "FilmmakerMenu.h"

#include "PanoramaFindPanel.h"
#include "FilmmakerGuiJs.h"
#include "../Filmmaker.h"
#include "../Demo/DemoLibrary.h"
#include "../Demo/DemoRepair.h"
#include "../Demo/DemoOrganizer.h"
#include "../Community/ToolVersion.h" // What's new payload + its two commands

#include "../../DeathMsg.h" // CS2::PanoramaUIPanel offsets
#include "../../UiInputGate.h" // pause-menu level for the director input gate

#include <cstring>
#include <cstdlib>
#include <sstream>

namespace Filmmaker {

void* FilmmakerMenu::FindRootPanel() {
	void* ctx = m_bridge.ContextPanel();
	if (!ctx)
		return nullptr;
	return FindChildById(ctx, "FilmmakerMenuTab");
}

void FilmmakerMenu::BuildUi() {
	m_buildAttempted = true;
	if (!m_bridge.RunScript(kFilmmakerGuiJs))
		return;

	m_rootPanel = FindRootPanel();
	m_symCmd = m_bridge.MakeSymbol("cmd");
	m_symDemos = m_bridge.MakeSymbol("demos");
	m_symOrg = m_bridge.MakeSymbol("org");
	m_symWhatsNew = m_bridge.MakeSymbol("whatsnew");
	m_built = (m_rootPanel != nullptr);
	m_lastVersion = (uint64_t)-1; // force a data push
	m_lastOrgVersion = (uint64_t)-1;
	m_whatsNewPushed = false;
	m_lastRepairSignature.clear();

	// Intentionally do NOT pin the context to our page: $.Filmmaker lives in the
	// (stable) main-menu JS context, and keeping that context lets FindRootPanel
	// re-locate the tab every frame (it may be shown/hidden/recreated by CS2's
	// tab system once it is parented under #JsMainMenuContent).
}

void FilmmakerMenu::PushData(DemoLibrary& library) {
	if (!m_rootPanel)
		m_rootPanel = FindRootPanel();
	if (!m_rootPanel || m_symDemos < 0)
		return;

	const std::string json = library.BuildJson();
	m_bridge.SetAttributeString(m_rootPanel, m_symDemos, json.c_str());
	m_bridge.RunScript("$.Filmmaker && $.Filmmaker.render();");
}

void FilmmakerMenu::PushOrganizer() {
	if (!m_rootPanel)
		m_rootPanel = FindRootPanel();
	if (!m_rootPanel || m_symOrg < 0)
		return;

	const std::string json = DemoOrganizer_BuildJson();
	m_bridge.SetAttributeString(m_rootPanel, m_symOrg, json.c_str());
	m_bridge.RunScript("$.Filmmaker && $.Filmmaker.renderOrg();");
}

void FilmmakerMenu::PushWhatsNew() {
	if (!m_rootPanel || m_symWhatsNew < 0)
		return;
	const std::string json = ToolVersion_WhatsNewJson();
	m_bridge.SetAttributeString(m_rootPanel, m_symWhatsNew, json.c_str());
	m_bridge.RunScript("$.Filmmaker && $.Filmmaker.renderWhatsNew && $.Filmmaker.renderWhatsNew();");
	if (m_whatsNewOpen) {
		m_whatsNewOpen = false;
		m_bridge.RunScript("$.Filmmaker && $.Filmmaker.whatsNew && $.Filmmaker.whatsNew();");
	}
	m_whatsNewPushed = true;
}

void FilmmakerMenu::RequestWhatsNew(bool forget) {
	if (forget) ToolVersion_ForgetWhatsNewSeen();
	m_whatsNewPushed = false;
	m_whatsNewOpen = true;
}

void FilmmakerMenu::PollCommands(DemoLibrary& library) {
	if (!m_rootPanel || m_symCmd < 0)
		return;

	std::string queued = m_bridge.GetAttributeString(m_rootPanel, m_symCmd, "");
	if (queued.empty())
		return;
	m_bridge.SetAttributeString(m_rootPanel, m_symCmd, "");

	std::istringstream iss(queued);
	std::string line;
	while (std::getline(iss, line)) {
		if (line.empty())
			continue;
		if (DemoOrganizer_HandleCommand(line)) {
			// Sort mode / demo folders / collapse state. Owned by DemoOrganizer, not the library.
		} else if (line == "refresh") {
			RequestRescan();
		} else if (line == "addfolder") {
			RequestAddFolder();
		} else if (line == "adddemo") {
			RequestAddDemo();
		} else if (line.rfind("openfolder ", 0) == 0) {
			OpenDemoProjectFolder((size_t)std::atoi(line.c_str() + 11));
		} else if (line.rfind("renamedemo ", 0) == 0) {
			// "renamedemo <index> <new name>" -- the name is LAST and unquoted so it may contain
			// spaces, the same shape "org rename" uses. Everything after the first space that
			// follows the index is the name, verbatim.
			//
			// A malformed line hands RenameDemoFile an empty name on purpose: validating and
			// reporting belongs to the one place that already has to sanitise the name against
			// path separators, and this dispatcher stays as dumb as its neighbours.
			const std::string rest = line.substr(11);
			const size_t sp = rest.find(' ');
			RenameDemoFile((size_t)std::atoi(rest.c_str()),
				(sp == std::string::npos) ? std::string() : rest.substr(sp + 1));
		} else if (line.rfind("watch ", 0) == 0) {
			Watch((size_t)std::atoi(line.c_str() + 6));
		} else if (line.rfind("prepare ", 0) == 0) {
			PrepareDemo((size_t)std::atoi(line.c_str() + 8));
		} else if (line.rfind("rounds ", 0) == 0) {
			library.EnsureRounds((size_t)std::atoi(line.c_str() + 7));
		} else if (line.rfind("hide ", 0) == 0) {
			// Delete button: drops the demo from the list only -- the .dem stays on disk.
			library.HideDemo((size_t)std::atoi(line.c_str() + 5));
		} else if (line == "unhideall") {
			library.UnhideAll();
		} else if (line == "clearall") {
			// Clears the browser only. Demo files remain on disk and can be restored.
			library.HideAllVisible();
		} else if (line == "whatsnew seen") {
			ToolVersion_MarkWhatsNewSeen();
		} else if (line == "whatsnew changelog") {
			ToolVersion_OpenChangelogOnline();
		}
	}
}

void FilmmakerMenu::RunFrame(DemoLibrary& library) {
	m_bridge.Init();

	// Drain any queued ui_eval scripts (live REPL). Runs on the main thread here,
	// independent of whether our tab is built, as long as the bridge can run JS.
	if (!m_evalQueue.empty() && m_bridge.CanRunScript() && m_bridge.ContextPanel()) {
		for (const std::string& js : m_evalQueue)
			m_bridge.RunScript(js);
		m_evalQueue.clear();
	}

	// Pause-menu tracker for the director input gate: CS2's own CSGOShow/HidePauseMenu (and
	// MainMenu) Panorama events are the only reliable "is the menu actually on screen" signal
	// (see UiInputGate.h for the two cheaper signals that measured wrong). A tiny subscriber in
	// this stable menu context records the level into a context-panel attribute; polled every
	// frame, independent of the Demos-tab lifecycle below.
	if (m_bridge.CanRunScript() && m_bridge.ContextPanel()) {
		if (!m_pauseTrackerInstalled) {
			// NOTE: Panorama JS has no `window`; referencing it throws and silently aborts the
			// whole script (cf. the style='' crash note) -- the install guard is an attribute.
			static const char* kPauseMenuTrackerJs =
				"(function(){"
				" var cp = $.GetContextPanel();"
				" if (cp.GetAttributeString('mvmPauseTracker','') === '1') return;"
				" cp.SetAttributeString('mvmPauseTracker','1');"
				" var set = function(v){ try { cp.SetAttributeString('mvmPauseMenuOpen', v); } catch(e){} };"
				" set('0');" // The menu context can exist while hidden; only a show event claims input.
				" $.RegisterForUnhandledEvent('CSGOShowPauseMenu', function(){ set('1'); });"
				" $.RegisterForUnhandledEvent('CSGOHidePauseMenu', function(){ set('0'); });"
				" $.RegisterForUnhandledEvent('CSGOShowMainMenu', function(){ set('1'); });"
				" $.RegisterForUnhandledEvent('CSGOHideMainMenu', function(){ set('0'); });"
				"})();";
			if (m_bridge.RunScript(kPauseMenuTrackerJs)) {
				m_pauseTrackerInstalled = true;
				m_symPauseMenu = m_bridge.MakeSymbol("mvmPauseMenuOpen");
			}
		} else if (m_symPauseMenu >= 0) {
			const std::string v =
				m_bridge.GetAttributeString(m_bridge.ContextPanel(), m_symPauseMenu, "");
			if (!v.empty())
				UiInputGate_SupplyPauseMenuOpen(v == "1");
		}
	}

	// Build the UI once, when the bridge is configured (engine + RunScript +
	// context). Only one attempt per context availability, to avoid per-frame
	// re-injection (which would spam). RequestRebuild() resets this.
	if (!m_built) {
		if (!m_buildAttempted && m_bridge.CanRunScript() && m_bridge.ContextPanel())
			BuildUi();
		if (!m_built)
			return; // nothing to drive yet (safe)
	}

	// Re-locate the tab every frame: it can be shown/hidden by the tab system or
	// dropped on a menu reload. A stale cached pointer would be a use-after-free,
	// so always re-find. If it has truly vanished, rebuild it next frame.
	m_rootPanel = FindRootPanel();
	if (!m_rootPanel) {
		m_built = false;
		m_buildAttempted = false;
		return;
	}

	// Repair runs independently of the library scan, so its status must also
	// invalidate the UI payload or the Watch-row label would never advance.
	const uint64_t version = library.Version();
	const std::string repairSignature =
		std::to_string((int)DemoRepair_State()) + "|" + DemoRepair_StatusText();
	if (version != m_lastVersion || repairSignature != m_lastRepairSignature) {
		PushData(library);
		m_lastVersion = version;
		m_lastRepairSignature = repairSignature;
	}

	// Organiser state (sort mode, demo folders, last-played times) moves independently of the
	// library, so it gets its own version gate rather than riding the scan version.
	const uint64_t orgVersion = DemoOrganizer_Version();
	if (orgVersion != m_lastOrgVersion) {
		PushOrganizer();
		m_lastOrgVersion = orgVersion;
	}

	if (!m_whatsNewPushed)
		PushWhatsNew();

	PollCommands(library);

	// Show the page on request (console "mirv_filmmaker ui").
	if (m_showRequested) {
		m_showRequested = false;
		m_bridge.RunScript("$.Filmmaker && $.Filmmaker.show();");
	}
}

} // namespace Filmmaker
