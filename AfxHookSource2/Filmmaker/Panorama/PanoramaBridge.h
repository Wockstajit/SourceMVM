#pragma once

// Thin wrapper around CS2's Panorama UI engine for the filmmaker menu.
//
// Reuses addresses already resolved in DeathMsg.cpp:
//   - the UI engine pointer,
//   - get/setAttributeString + makeSymbol,
//   - CUIEngine::RunScript (function resolved from panorama.dll), and
//   - the in-game HUD panel (used as the RunScript context to create the UI).
//
// All calls are null-guarded: if RunScript did not resolve, every method is a
// safe no-op and the console-driven demo browser keeps working.

#include <cstdint>
#include <string>

namespace Filmmaker {

// ===========================================================================
// CANONICAL PANORAMA Z-LAYER MAP (single source of truth)
//
// Every in-game filmmaker HUD root is created as a child of the SAME context panel
// (AfxHookSource2_GetPanoramaHudPanel()), so all of these z-index values compete in
// ONE stacking context. Keep them DISTINCT and in this documented order. The literal
// values live in each *Js.h root-creation line; if you change one, update it here too.
//
//   50  MovieHud root          (MovieHudJs.h)            - director help/status cards
//   51  FxDebugHud root        (FxDebugHudJs.h)          - FX wisp-swap debug squares (opt-in)
//   53  CameraEditor root      (CameraEditorJs.h)        - editor chrome/backdrop (below timeline)
//   55  CameraTimeline root    (CameraTimelineJs.h)      - timeline bar (sits on the editor backdrop)
//   58  native demo controller (CameraTimelineJs.h)      - ONLY while docked in a workspace, so CS2's
//                                                          player list / round menu open above the editor
//   59  CamTimelineRange root  (CameraTimelineRangeJs.h) - loop in/out flags; above the docked native bar
//   60  MarkerHud root         (MarkerHudJs.h)           - BO2-style marker-edit menu
//   65  GraphEditor root       (GraphEditorJs.h)         - experimental graph overlay
//   220 CamEditorConfirmRoot   (CameraEditorJs.h)        - "clear all" confirm modal (above ALL HUD roots;
//                                                          as a child of the z53 root it hid behind z55/z65)
//   221 CamEditorSettingsRoot  (CameraEditorJs.h)        - gear settings fly-out (same reason)
//   230 CamEditorCustomizeRoot (CameraEditorCustomizeJs.h) - Customize Player modal (same reason: as a
//                                                          z230 CHILD of z53 the timeline root z55 cut
//                                                          off its bottom rows and CANCEL, and the graph
//                                                          root z65 painted over it outright)
//   260 CamEditorCustomizeConfirmRoot (CameraEditorCustomizeJs.h) - "reset to default" confirm; must also
//                                                          outrank CamEditorCustomizeRoot above it
//   270 MvmBarTipRoot          (CameraTimelineJs.h)      - demo-bar button hover hint (above the inspector)
//   280 ConfigHudOverlayRoot   (ConfigHudWidgetsJs.h)    - CONFIG tooltips, dropdowns and menus. As children
//                                                          of ConfigHudRoot (z52) they drew BEHIND the docked
//                                                          native demo controller (z58)
//
// In-root local layers (children, scoped to their own root - do NOT collide cross-root):
//   150 CameraEditor debug overlay,
//   100 FilmmakerGui search popup (main-menu context, separate panel).
// ===========================================================================

class PanoramaBridge {
public:
	// Grabs the engine + RunScript pointers and (in auto mode) the HUD context.
	void Init();

	// The engine is quitting: from its quit request on, Panorama is being torn down, and a script
	// or panel call lands in freed memory (crash report 2026-10-13: MovieHud's teardown script ran
	// after HSR_QUIT and read 0xffffffffffffffff in panorama.dll). Set from the console capture
	// when the engine logs the quit request; never cleared, the process is ending.
	static void NoteEngineQuitting();
	static bool EngineQuitting();

	bool HasEngine() const { return m_engine != nullptr && *m_engine != nullptr; }
	bool HasRunScript() const { return m_runScript != nullptr; }
	bool CanRunScript() const { return HasEngine() && HasRunScript() && !EngineQuitting(); }

	// Pins the context panel that scripts run inside (disables HUD auto-context).
	void SetContextPanel(void* uiPanel) { m_contextPanel = uiPanel; m_autoContext = false; }
	// Null once the engine is quitting: every HUD bails on a missing context, so none touches a
	// panel that is being destroyed.
	void* ContextPanel() const { return EngineQuitting() ? nullptr : m_contextPanel; }

	// Runs Panorama JavaScript in the context panel. No-op unless CanRunScript()
	// and a context panel are set.
	//
	// EVERY CALL RECOMPILES. The origin line is bumped per call, which is what makes CS2 skip its
	// compiled-script cache -- necessary here, because this overload's source changes from call to
	// call (state pushes, eval, generated fragments). Use it for those, and NOT for a per-frame
	// call whose text is a constant: see RunScriptCached.
	bool RunScript(const std::string& source);

	// Same, for a call site whose source is a COMPILE-TIME CONSTANT -- in practice the per-frame
	// `$.Thing && $.Thing.render();` pumps, which are the only scripts run every single frame.
	//
	// CS2 caches a compiled script against its ORIGIN FILE and consults that cache only when the
	// origin LINE is 0 (PanoramaBridge.cpp::RunScript records how that was established). So a
	// constant script wants line 0 AND an origin of its own: line 0 alone is not enough, because
	// every call site then shares the origin "filmmaker":0, the cache hands back some other site's
	// compiled script, and the workspace panels silently stop building.
	//
	// `originFile` is therefore a KEY, not a label. It must be unique per script text and stable
	// for the life of the process, and the source passed with it must never vary -- pass a
	// different string under the same origin and Panorama will run the first one forever.
	bool RunScriptCached(const char* source, const char* originFile);

	// Attribute IO on a CUIPanel* (reuses the resolved get/setAttributeString).
	std::string GetAttributeString(void* uiPanel, short symbol, const char* defaultValue = "") const;
	void SetAttributeString(void* uiPanel, short symbol, const char* value) const;

	// Creates / interns a Panorama symbol by name (reuses the resolved makeSymbol).
	short MakeSymbol(const char* name) const;

private:
	unsigned char** m_engine = nullptr;     // AfxHookSource2_GetPanoramaUiEngine()
	void* m_runScript = nullptr;            // AfxHookSource2_GetPanoramaRunScript()
	void* m_contextPanel = nullptr;
	bool m_autoContext = true;              // when set, follow the HUD panel
	// Non-zero -> RunScript skips caching, which costs a full recompile on every call. Pinning it
	// to 0 is NOT the fix; it collides every call site on one origin. See PanoramaBridge.cpp::RunScript.
	uint64_t m_line = 0;
};

} // namespace Filmmaker
