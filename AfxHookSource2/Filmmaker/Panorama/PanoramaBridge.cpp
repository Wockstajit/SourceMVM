#include "PanoramaBridge.h"

#include "../../DeathMsg.h" // CS2::PanoramaUIEngine/UIPanel offsets + engine/runscript accessors

#include <atomic>

namespace Filmmaker {

namespace {
std::atomic<bool> g_engineQuitting{ false };
}

void PanoramaBridge::NoteEngineQuitting() { g_engineQuitting.store(true, std::memory_order_relaxed); }
bool PanoramaBridge::EngineQuitting() { return g_engineQuitting.load(std::memory_order_relaxed); }

void PanoramaBridge::Init() {
	if (!m_engine)
		m_engine = AfxHookSource2_GetPanoramaUiEngine();
	if (!m_runScript)
		m_runScript = AfxHookSource2_GetPanoramaRunScript();
	// In auto mode, prefer the main-menu panel as the build context: it is the
	// window that owns the top navbar, so the "Demos" button can attach there.
	// Fall back to the in-game HUD panel (valid while a map/demo is loaded) so
	// the page can still be built/opened when no main menu exists.
	if (m_autoContext) {
		if (unsigned char* mainMenu = AfxHookSource2_GetPanoramaMainMenuPanel())
			m_contextPanel = mainMenu;
		else if (unsigned char* hud = AfxHookSource2_GetPanoramaHudPanel())
			m_contextPanel = hud;
	}
}

short PanoramaBridge::MakeSymbol(const char* name) const {
	if (!m_engine || !*m_engine || CS2::PanoramaUIEngine::makeSymbol == 0)
		return -1;
	typedef short(__fastcall* makeSymbol_t)(unsigned char* engine, int type, const char* name);
	const auto makeSymbol = *(makeSymbol_t*)((*(unsigned char**)(*m_engine)) + CS2::PanoramaUIEngine::makeSymbol);
	return makeSymbol(*m_engine, 0, name);
}

std::string PanoramaBridge::GetAttributeString(void* uiPanel, short symbol, const char* defaultValue) const {
	if (EngineQuitting() || !uiPanel || CS2::PanoramaUIPanel::getAttributeString == 0)
		return defaultValue ? defaultValue : "";
	typedef const char* (__fastcall* getAttributeString_t)(void* panel, short attr, const char* def);
	const auto fn = *(getAttributeString_t*)(*(unsigned char**)uiPanel + CS2::PanoramaUIPanel::getAttributeString);
	const char* v = fn(uiPanel, symbol, defaultValue ? defaultValue : "");
	return v ? std::string(v) : std::string();
}

void PanoramaBridge::SetAttributeString(void* uiPanel, short symbol, const char* value) const {
	if (EngineQuitting() || !uiPanel || CS2::PanoramaUIPanel::setAttributeString == 0)
		return;
	typedef void* (__fastcall* setAttributeString_t)(void* panel, short attr, const char* value);
	const auto fn = *(setAttributeString_t*)(*(unsigned char**)uiPanel + CS2::PanoramaUIPanel::setAttributeString);
	fn(uiPanel, symbol, value ? value : "");
}

bool PanoramaBridge::RunScript(const std::string& source) {
	if (!CanRunScript() || !m_contextPanel)
		return false;

	// void RunScript(CUIEngine* this, CUIPanel* ctx, const char* src, const char* originFile, uint64 line)
	typedef void(__fastcall* runScript_t)(void* engine, void* ctx, const char* src, const char* originFile, uint64_t line);
	const auto fn = (runScript_t)m_runScript;
	// THE INCREMENTING LINE IS THE DEADLOCK EDITOR'S FRAME RATE, AND IT CANNOT SIMPLY BE PINNED.
	//
	// A non-zero origin line makes RunScript skip its compiled-script cache (see m_line), so every
	// call recompiles from source. The workspace calls this once per frame, which measured at ~159
	// ms per call in a replay: 6 fps with the editor open against 164-209 fps for the game with no
	// hook at all. The cost is inside this call and invisible either side of it -- the script body
	// itself measured ~1 ms.
	//
	// Pinning the line to 0 does fix the frame rate (195 fps) and BREAKS THE UI: every caller then
	// shares the origin "filmmaker":0, the cache hands back another call site's compiled script, and
	// the workspace panels never build (panelsBuilt=no, layout NOT SOLVED). Verified both ways.
	//
	// The real fix is to stop baking per-frame state into the script TEXT, so the source is constant
	// and one compiled script can be reused at a stable origin of its own. That needs a data channel
	// for the state: CUIPanel attributes are the one CS2 uses, but SetAttributeString/GetAttributeString
	// are resolved in DeathMsg.cpp, which this target does not build, so they are 0 here and the
	// bridge's attribute methods are no-ops until those offsets are scanned for Deadlock.
	fn(*m_engine, m_contextPanel, source.c_str(), "filmmaker", ++m_line);
	return true;
}

// The cached counterpart, and on CS2 the "real fix" the comment above describes is already in
// place: per-frame state rides panel ATTRIBUTES here, so the only script text left inside the
// frame loop is one constant one-liner per open panel. Those were still paying a full V8 compile
// every frame, once per panel, for source that never changed.
//
// line 0 puts the cache in play; a per-site originFile stops the sites colliding inside it.
bool PanoramaBridge::RunScriptCached(const char* source, const char* originFile) {
	if (!CanRunScript() || !m_contextPanel || !source || !originFile)
		return false;
	typedef void(__fastcall* runScript_t)(void* engine, void* ctx, const char* src, const char* originFile, uint64_t line);
	const auto fn = (runScript_t)m_runScript;
	fn(*m_engine, m_contextPanel, source, originFile, 0);
	return true;
}

} // namespace Filmmaker
