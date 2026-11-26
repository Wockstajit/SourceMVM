#define NOMINMAX
#include "SceneEditorHud.h"
#include "SceneEditor.h"
#include "SceneEditorJs.h"
#include "SceneEditorGizmoJs.h"
#include "SceneEditorTimelineJs.h"

#include "../Panorama/PanoramaFindPanel.h"
#include "../Panorama/WorkspacePreview.h"
#include "../Takes/ActorAnimations.h"
#include "../Takes/Takes.h"
#include "../../DeathMsg.h" // AfxHookSource2_GetPanoramaHudPanel

#include <sstream>

namespace Filmmaker {

SceneEditorHud &SceneEditorHudRef() {
	static SceneEditorHud s_instance;
	return s_instance;
}

void *SceneEditorHud::FindRoot() {
	void *ctx = m_bridge.ContextPanel();
	return ctx ? FindChildById(ctx, "SceneEditorRoot") : nullptr;
}

bool SceneEditorHud::BuildIfNeeded() {
	if (m_built) return true;
	if (!m_bridge.CanRunScript() || !m_bridge.ContextPanel()) return false;
	std::string script;
	script.reserve(48000);
	script += kSceneEditorJsHeader;
	script += kSceneEditorGizmoJsA;
	script += kSceneEditorGizmoJsB;
	script += kSceneEditorTimelineJsA;
	script += kSceneEditorTimelineJsB;
	script += kSceneEditorJsBody;
	script += kSceneEditorJsFooter;
	if (!m_bridge.RunScript(script.c_str())) return false;
	m_symState = m_bridge.MakeSymbol("state");
	m_symTextCapture = m_bridge.MakeSymbol("textcapture");
	m_symPreviewRect = m_bridge.MakeSymbol("previewrect");
	m_root = FindRoot();
	m_built = (m_root != nullptr);
	m_lastState.clear();
	m_textCapture = false;
	return m_built;
}

void SceneEditorHud::Teardown() {
	WorkspacePreviewRef().Release(WorkspacePreview::Owner::SceneEditor);
	if (m_built && m_hudPanel && m_bridge.ContextPanel()) {
		m_bridge.RunScript("if($.SceneEditor && $.SceneEditor.restoreHud) $.SceneEditor.restoreHud(); var r=$('#SceneEditorRoot'); if(r) r.DeleteAsync(0); $.SceneEditor=null;");
	}
	m_built = false;
	m_root = nullptr;
	m_hudPanel = nullptr;
	m_lastState.clear();
	m_textCapture = false;
	{
		std::lock_guard<std::mutex> lk(m_charMutex);
		m_pendingChars.clear();
	}
	{
		std::lock_guard<std::mutex> lk(m_wheelMutex);
		m_pendingWheel = 0;
	}
}

void SceneEditorHud::UpdateScaleRequest() {
	if (!m_root || m_symPreviewRect < 0) {
		WorkspacePreviewRef().Release(WorkspacePreview::Owner::SceneEditor);
		return;
	}
	float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
	std::string pr = m_bridge.GetAttributeString(m_root, m_symPreviewRect, "");
	std::istringstream is(pr);
	const bool parsed = static_cast<bool>(is >> x0 >> y0 >> x1 >> y1);
	const bool valid = parsed && (x1 - x0 > 0.01f) && (y1 - y0 > 0.01f)
		&& x0 >= 0.f && y0 >= 0.f && x1 <= 1.0001f && y1 <= 1.0001f;
	if (valid)
		WorkspacePreviewRef().Submit(WorkspacePreview::Owner::SceneEditor, x0, y0, x1, y1);
	else
		WorkspacePreviewRef().Release(WorkspacePreview::Owner::SceneEditor);
}

void SceneEditorHud::PushWheel(int notches) {
	std::lock_guard<std::mutex> lk(m_wheelMutex);
	int next = m_pendingWheel + notches;
	if (next > 32) next = 32;
	if (next < -32) next = -32;
	m_pendingWheel = next;
}

void SceneEditorHud::PushChar(unsigned charCode) {
	std::lock_guard<std::mutex> lk(m_charMutex);
	if (m_pendingChars.size() < 128) m_pendingChars.push_back(charCode);
}

void SceneEditorHud::DispatchWheel() {
	int notches = 0;
	{
		std::lock_guard<std::mutex> lk(m_wheelMutex);
		notches = m_pendingWheel;
		m_pendingWheel = 0;
	}
	if (notches == 0 || !m_root) return;
	char buf[96];
	snprintf(buf, sizeof(buf), "$.SceneEditor && $.SceneEditor.wheel && $.SceneEditor.wheel(%d);", notches);
	m_bridge.RunScript(buf);
}

void SceneEditorHud::DispatchChars() {
	std::vector<unsigned> chars;
	{
		std::lock_guard<std::mutex> lk(m_charMutex);
		chars.swap(m_pendingChars);
	}
	if (chars.empty() || !m_root) return;
	std::string js = "$.SceneEditor && $.SceneEditor.chars && $.SceneEditor.chars('";
	for (size_t i = 0; i < chars.size(); ++i) {
		if (i) js += ',';
		js += std::to_string(chars[i]);
	}
	js += "');";
	m_bridge.RunScript(js.c_str());
}

void SceneEditorHud::RunFrame() {
	m_bridge.Init();
	const bool enabled = SceneEditor::Enabled();
	unsigned char *hud = AfxHookSource2_GetPanoramaHudPanel();
	if (!hud || !Takes::Available()) {
		WorkspacePreviewRef().Release(WorkspacePreview::Owner::SceneEditor);
		if (m_wasEnabled) {
			m_wasEnabled = false;
			Teardown();
		}
		m_bridge.SetContextPanel(nullptr);
		m_hudPanel = nullptr;
		m_root = nullptr;
		m_built = false;
		return;
	}
	if (hud != m_hudPanel) {
		m_hudPanel = hud;
		m_built = false;
	}
	m_bridge.SetContextPanel(hud);

	if (!enabled) {
		WorkspacePreviewRef().Release(WorkspacePreview::Owner::SceneEditor);
		if (m_wasEnabled) {
			m_wasEnabled = false;
			Teardown();
		}
		return;
	}
	m_wasEnabled = true;

	if (!BuildIfNeeded()) {
		WorkspacePreviewRef().Release(WorkspacePreview::Owner::SceneEditor);
		return;
	}
	m_root = FindRoot();
	if (!m_root) {
		WorkspacePreviewRef().Release(WorkspacePreview::Owner::SceneEditor);
		m_built = false;
		return;
	}

	{
		std::vector<std::string> evals;
		{
			std::lock_guard<std::mutex> lk(m_evalMutex);
			evals.swap(m_evalQueue);
		}
		for (const std::string &js : evals) m_bridge.RunScript(js.c_str());
	}

	if (SceneEditor::TakeCatalogRequested()) {
		std::string js = "$.SceneEditor && $.SceneEditor.setCatalog && $.SceneEditor.setCatalog(" + ActorAnim::CatalogJson() + ");";
		m_bridge.RunScript(js.c_str());
	}

	const std::string state = SceneEditor::BuildStateJson();
	if (state != m_lastState) {
		m_bridge.SetAttributeString(m_root, m_symState, state.c_str());
		m_lastState = state;
	}
	m_bridge.RunScriptCached("$.SceneEditor && $.SceneEditor.render();", "mvm/sceneeditor.render");
	UpdateScaleRequest();
	m_textCapture = m_root && (m_bridge.GetAttributeString(m_root, m_symTextCapture, "0") == "1");
	DispatchChars();
	DispatchWheel();
}

} // namespace Filmmaker
