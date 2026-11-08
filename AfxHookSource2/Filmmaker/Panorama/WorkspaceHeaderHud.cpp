#include "WorkspaceHeaderHud.h"

#include "PanoramaFindPanel.h"
#include "WorkspaceHeaderJs.h"
#include "UiLayout.h"
#include "ConfigHud.h"
#include "CameraTimelineHud.h"

#include "../Filmmaker.h"                  // CameraEditor_Active, CameraEditor_CustomizeModalOpen
#include "../Movie/TimelineDock.h"
#include "../Movie/HudBlackout.h"
#include "../Render/RenderCommand.h"       // Render_IsModalOpen, Render_IsJobActive
#include "../Render/RenderSessionDriver.h" // capture chrome gate
#include "../Cosmetics/CustomizeTarget.h"
#include "../Debug/MvmDebugLog.h"

#include "../../DeathMsg.h" // AfxHookSource2_GetPanoramaHudPanel

#include <sstream>

namespace Filmmaker {

WorkspaceHeaderHud& WorkspaceHeaderHudRef() {
	static WorkspaceHeaderHud s_instance;
	return s_instance;
}

bool WorkspaceHeader_Visible() { return WorkspaceHeaderHudRef().Visible(); }
bool WorkspaceHeader_PopupOpen() { return WorkspaceHeaderHudRef().PopupOpen(); }

void* WorkspaceHeaderHud::FindRoot() {
	void* ctx = m_bridge.ContextPanel();
	return ctx ? FindChildById(ctx, "MvmHeaderRoot") : nullptr;
}

bool WorkspaceHeaderHud::BuildIfNeeded() {
	if (m_built) return true;
	if (!m_bridge.CanRunScript() || !m_bridge.ContextPanel()) return false;
	if (!m_bridge.RunScript(kWorkspaceHeaderJs)) return false;
	m_symState = m_bridge.MakeSymbol("state");
	m_symRect = m_bridge.MakeSymbol("winrect");
	m_symPopupOpen = m_bridge.MakeSymbol("popupopen");
	m_root = FindRoot();
	m_built = (m_root != nullptr);
	m_lastJson.clear();
	return m_built;
}

void WorkspaceHeaderHud::Teardown() {
	if (m_built && m_hudPanel && m_bridge.ContextPanel()) {
		m_bridge.RunScript(
			"(function(){var e=$('#MvmHeaderRoot'); if(e) e.DeleteAsync(0);"
			" var s=$('#MvmHeaderSettingsRoot'); if(s) s.DeleteAsync(0); $.MvmHeader=null;})();");
	}
	m_built = false;
	m_root = nullptr;
	m_hudPanel = nullptr;
	m_lastJson.clear();
	m_visible = false;
	m_popupOpen = false;
	UiLayout_SetPanelRect(UiPanel::Header, 0, 0, 0, 0);
	// Only when it is ours: a release by a non-owner is logged (mvm_debug), and this runs every
	// frame the layout is not modular.
	if (TimelineDock_Host() == DockHost::Header) TimelineDock_Release(DockHost::Header);
}

std::string WorkspaceHeaderHud::BuildStateJson() {
	std::ostringstream o;
	o << "{";
	o << "\"enabled\":" << (m_visible ? "true" : "false");
	o << ",\"modular\":true";
	o << ",\"config\":" << (ConfigHud_Enabled() ? "true" : "false");
	o << ",\"editor\":" << (CameraEditor_Active() ? "true" : "false");
	o << ",\"render\":" << (Render_IsModalOpen() ? "true" : "false");
	o << ",\"renderJob\":" << (Render_IsJobActive() ? "true" : "false");
	o << ",\"dock\":\"" << TimelineDock_TabName() << "\"";
	o << ",\"customize\":" << (CameraEditor_CustomizeModalOpen() ? "true" : "false");
	o << ",\"customizeReady\":" << (CustomizeTarget_Available() ? "true" : "false");
	o << "}";
	return o.str();
}

void WorkspaceHeaderHud::RunFrame() {
	m_bridge.Init();

	// The camera timeline's reading, which pumps earlier this frame: a demo_gototick seek reports
	// no demo for a few frames, and taking that raw tore the bar down on every seek -- and with it
	// the TIMELINE dock it hosts, ending the scrub that caused the seek (Codex review on #137).
	const bool playingDemo = CameraTimelineHudRef().DemoActive();
	unsigned char* hud = (playingDemo && UiLayout_Modular()) ? AfxHookSource2_GetPanoramaHudPanel() : nullptr;
	if (!hud) {
		Teardown();
		m_bridge.SetContextPanel(nullptr);
		return;
	}
	if (hud != m_hudPanel) { m_hudPanel = hud; m_built = false; }
	m_bridge.SetContextPanel(hud);

	// The Render workspace is the docked full-screen job even in MODULAR layout, and a capture
	// or F8 clears every overlay: the bar steps aside for all three.
	m_visible = !Render_IsModalOpen() && !RenderSessionDriver_WantsChromeHidden() && !HudBlackout_Active();

	// The dock along the bottom with no window open: this bar is its host. A window that opens
	// outranks it (TimelineDock.h), and a host that is not needed gives it up, which parks it.
	const bool hostDock = m_visible && !ConfigHud_Enabled() && !CameraEditor_Active()
		&& TimelineDock_Tab() == DockTab::Timeline;
	if (hostDock) {
		TimelineDock_Claim(DockHost::Header);
		TimelineDock_Apply(DockHost::Header);
	} else if (TimelineDock_Host() == DockHost::Header) {
		TimelineDock_Release(DockHost::Header);
	}

	if (!BuildIfNeeded()) return;
	m_root = FindRoot();
	if (!m_root) { m_built = false; return; }

	if (!m_evalQueue.empty()) {
		m_bridge.RunScript(m_evalQueue);
		m_evalQueue.clear();
	}

	const std::string json = BuildStateJson();
	if (json != m_lastJson) {
		m_bridge.SetAttributeString(m_root, m_symState, json.c_str());
		m_lastJson = json;
	}
	m_bridge.RunScriptCached("$.MvmHeader && $.MvmHeader.render();", "mvm/header.render");

	const std::string rect = m_visible ? m_bridge.GetAttributeString(m_root, m_symRect, "") : std::string();
	UiLayout_SetPanelRectString(UiPanel::Header, rect.c_str());
	m_popupOpen = m_visible && m_bridge.GetAttributeString(m_root, m_symPopupOpen, "0") == "1";
}

} // namespace Filmmaker
