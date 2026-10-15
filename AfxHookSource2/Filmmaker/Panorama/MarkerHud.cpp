#include "MarkerHud.h"

#include "PanoramaFindPanel.h"

#include "MarkerHudJs.h"
#include "../Movie/CameraPath.h"
#include "../Render/RenderSessionDriver.h" // WantsChromeHidden (capture chrome gate)

#include "../../DeathMsg.h" // AfxHookSource2_GetPanoramaHudPanel + PanoramaUIPanel offsets

#include "../../../Deps/release/prop/AfxHookSource/SourceSdkShared.h"
#include "../../../Deps/release/prop/cs2/sdk_src/public/cdll_int.h"

#include <cstring>
#include <sstream>

extern SOURCESDK::CS2::ISource2EngineToClient* g_pEngineToClient;

namespace Filmmaker {

namespace {


double r2(double v) { double s = (v < 0) ? -1.0 : 1.0; return s * (long long)(v * s * 100.0 + 0.5) / 100.0; }

} // namespace

MarkerHud& MarkerHudRef() {
	static MarkerHud s_instance;
	return s_instance;
}

void* MarkerHud::FindRoot() {
	void* ctx = m_bridge.ContextPanel();
	if (!ctx)
		return nullptr;
	return FindChildById(ctx, "MarkerHudRoot");
}

bool MarkerHud::BuildIfNeeded() {
	if (m_built)
		return true;
	if (!m_bridge.CanRunScript() || !m_bridge.ContextPanel())
		return false;
	if (!m_bridge.RunScript(kMarkerHudJs))
		return false;
	m_symState = m_bridge.MakeSymbol("state");
	m_root = FindRoot();
	m_built = (m_root != nullptr);
	m_lastJson.clear();
	return m_built;
}

void MarkerHud::Teardown() {
	if (m_built && m_hudPanel && m_bridge.ContextPanel()) {
		m_bridge.RunScript(
			"(function(){var e=$('#MarkerHudRoot'); if(e) e.DeleteAsync(0); $.MarkerHud=null;})();");
	}
	m_built = false;
	m_root = nullptr;
	m_hudPanel = nullptr;
	m_lastJson.clear();
}

std::string MarkerHud::BuildStateJson() {
	CameraPath& cp = CameraPathRef();
	const char* mode;
	switch (cp.GetMode()) {
	case CameraPath::Mode::Reposition: mode = "reposition"; break;
	case CameraPath::Mode::PreviewArmed: mode = "previewArmed"; break;
	case CameraPath::Mode::PreviewPlaying: mode = "previewPlaying"; break;
	default: mode = "editing"; break;
	}
	// JSON-escape the transient notice (it's plain ASCII, but quote-safe anyway).
	std::string notice = cp.Notice();
	std::string noticeEsc;
	for (char c : notice) {
		if (c == '"' || c == '\\') noticeEsc.push_back('\\');
		noticeEsc.push_back(c);
	}

	std::ostringstream o;
	o << "{";
	// Capture chrome gate. The camera-path card and the prompt banner are drawn INTO the shot
	// (the banner is transformed into the preview rect), so a before-present capture would bake
	// "CAMERA PATH" / "REPOSITIONING CAMERA #n" straight into the recorded POV or free-cam
	// frames. Projected per frame, so the UI returns the frame the job leaves the capture states.
	o << "\"captureHide\":" << (RenderSessionDriver_WantsChromeHidden() ? "true" : "false");
	o << ",\"mode\":\"" << mode << "\"";
	o << ",\"menuOpen\":" << (cp.MenuOpen() ? "true" : "false");
	o << ",\"hudHidden\":" << (cp.HudHidden() ? "true" : "false");
	o << ",\"count\":" << cp.Count();
	o << ",\"selected\":" << cp.Selected();
	o << ",\"isLast\":" << (cp.SelectedIsLast() ? "true" : "false");
	o << ",\"speedMode\":\"" << cp.SpeedModeName() << "\"";
	o << ",\"timing\":\"" << cp.TimingName() << "\"";
	o << ",\"interp\":\"" << cp.InterpName() << "\"";
	o << ",\"speedMul\":" << r2(cp.SelectedSpeedMul());
	o << ",\"constSpeed\":" << r2(cp.ConstSpeed());
	o << ",\"autoSnap\":" << (cp.AutoSnap() ? "true" : "false");
	o << ",\"segment\":" << cp.PlaySegment();
	o << ",\"playing\":" << (cp.IsPlaying() ? "true" : "false");
	o << ",\"editorPlay\":" << (cp.EditorPlay() ? "true" : "false"); // editor test: no banner
	o << ",\"notice\":\"" << noticeEsc << "\"";
	o << "}";
	return o.str();
}

void MarkerHud::RunFrame() {
	m_bridge.Init();

	bool playingDemo = false;
	if (g_pEngineToClient) {
		if (auto pDemo = g_pEngineToClient->GetDemoFile())
			playingDemo = pDemo->IsPlayingDemo();
	}

	unsigned char* hud = playingDemo ? AfxHookSource2_GetPanoramaHudPanel() : nullptr;
	if (!hud) { // main menu / not playing a demo -> tear down + bail
		Teardown();
		return;
	}
	if (hud != m_hudPanel) { // HUD (re)created -> rebuild against the new context
		m_hudPanel = hud;
		m_built = false;
	}
	m_bridge.SetContextPanel(hud);

	if (!BuildIfNeeded())
		return;

	m_root = FindRoot();
	if (!m_root) { m_built = false; return; }

	std::string json = BuildStateJson();
	if (json != m_lastJson) {
		m_bridge.SetAttributeString(m_root, m_symState, json.c_str());
		m_bridge.RunScriptCached("$.MarkerHud && $.MarkerHud.render();", "mvm/markerhud.render");
		m_lastJson = json;
	}
}

} // namespace Filmmaker
