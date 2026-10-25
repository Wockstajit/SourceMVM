#pragma once

// ConfigHud: a lightweight CONFIG panel rendered with CS2's native Panorama in the in-game
// HUD (CSGOHud) context -- the Camera Editor's little sibling.
//
// It reuses the editor's inspector look (see ConfigHudJs.h) but carries ONLY general
// UI / game-display configuration:
//   * the shared "Game UI" visibility picker (same HudView state the Camera Editor uses;
//     the actual show/hide stays centralised in CameraTimelineHud's JS, which honours the
//     picker while EITHER the editor is hosted OR this panel is open);
//   * a "Show UI Defaults" reset;
//   * the native demo-playback display toggles (X-Ray / True View / DOA / mismatch).
//
// Unlike the Camera Editor it does NOT host the camera timeline, does NOT hide the native
// demo bar (the regular game timeline stays visible), and has no camera tools at all.
// Mutually exclusive with the Camera Editor: opening one closes the other.
//
// Mirrors the proven MovieHud / CameraEditorHud bridge pattern: a PanoramaBridge pinned to
// the in-game HUD panel, a panel built ONCE from embedded JS, a small state JSON per frame.
//
//   C++ -> JS : attribute "state" ({enabled, cursor, hudView}), then $.ConfigHud.render().
//   JS  -> C++: buttons issue "mirv_filmmaker ..." console commands.

#include "PanoramaBridge.h"

#include <mutex>
#include <string>
#include <vector>

namespace advancedfx { class ICommandArgs; }

namespace Filmmaker {

class ConfigHud {
public:
	// Main/UI thread, once per frame from Filmmaker::RunMainThreadFrame.
	void RunFrame();

	// WHICH PANEL THIS IS. The demo Config workspace and the offline live-match Insert panel
	// share this class and its JS, but they are NOT the same panel and must not share state.
	// Everything that used to be common leaked between them: the live panel grabbed the DEMO
	// camera timeline's cursor flag, and one `m_enabled` meant a panel opened in one context
	// was still "open" after switching to the other, in the other's layout. The context is
	// latched at open time (never re-read per frame) so a demo-seek blip in IsPlayingDemo()
	// cannot silently reclassify a panel that is already up.
	enum class Context { Demo, Live };
	Context GetContext() const { return m_context; }
	bool IsLive() const { return m_context == Context::Live; }

	// The live panel owns its OWN UI-cursor flag. The demo panel keeps using the camera
	// timeline's, because in a demo the timeline genuinely is the cursor owner.
	bool LiveCursor() const { return m_liveCursor; }
	void SetLiveCursor(bool v) { m_liveCursor = v; }

	// Live REPL in THIS panel's Panorama context ("mirv_filmmaker config eval <js>"). The Camera
	// Editor, the camera timeline and the graph editor all already have one; Config did not, which
	// is why its own controls could not be driven or read back from netcon at all -- synthetic
	// mouse input does not reach CS2, so a button here was untestable without a human hand.
	void RequestEval(const std::string& js) { m_evalQueue.push_back(js); }

	void SetEnabled(bool v) { m_enabled = v; }
	void Toggle() { m_enabled = !m_enabled; }

	// Captured-wheel pipe. In this in-game HUD context Panorama never receives the mouse wheel:
	// MovieMode swallows it so it cannot reach the demo spectator behind the panel, which left
	// the inspector column unscrollable by design (see MovieMode::OnMouseWheel). The notch is
	// therefore captured on the WndProc/input thread, queued here, and handed to the JS on the
	// main thread in RunFrame -- the same contract the Customize modal uses
	// (CameraEditorHud::PushCustomizeWheel / DispatchCustomizeInput).
	void PushWheel(int notches);
	void PushChar(unsigned charCode);
	void PushKey(const char* name); // a named shortcut for the panel's JS ($.ConfigHud.key)
	bool TextCaptureActive() const { return m_textCapture; }
	// The Editor Settings fly-out is up: its full-screen overlay takes every click, preview included.
	bool PopupOpen() const { return m_popupOpen; }
	bool MapViewOpen() const { return m_mapViewOpen; }
	// The ENVIRONMENT tab edits in the preview: left picks and drags, right looks around.
	bool ViewEditing() const { return m_viewEdit; }
	bool Enabled() const { return m_enabled; }

	std::string DebugStateJson() { return BuildStateJson(); }

	// Demo Config uses the same scaled-preview contract as the Camera Editor. The offline
	// live-game Insert panel remains a full-size overlay and never reports a scaled HUD.
	bool ScaledHud(float& x0, float& y0, float& x1, float& y1) const {
		if (!(m_enabled && m_demoScaleActive && m_previewValid)) return false;
		x0 = m_previewX0; y0 = m_previewY0; x1 = m_previewX1; y1 = m_previewY1;
		return true;
	}

private:
	void* FindRoot();
	bool BuildIfNeeded();
	void Teardown();
	void DispatchWheel(); // main thread: drain the queue into the panel's JS
	void DispatchChars(); // main thread: drain typed numeric input into the panel's JS
	void UpdateTextCapture();
	std::string BuildStateJson();
	void OnEnter(); // one-shot: turn the UI cursor on so the panel is immediately clickable
	void OnExit();  // one-shot: tear down the chrome (HudView is shared state; not reset here)
	void UpdateScaleRequest(); // demo: forward previewrect; live Insert: clear the scaler

	std::vector<std::string> m_evalQueue;
	PanoramaBridge m_bridge;
	// Wheel notches captured off the WndProc thread, drained on the main thread.
	std::mutex m_wheelMutex;
	int m_pendingWheel = 0;
	std::mutex m_charMutex;
	std::vector<unsigned> m_pendingChars;
	std::vector<std::string> m_pendingKeys;
	void* m_hudPanel = nullptr; // HUD context we built against (rebuild if it changes)
	void* m_root = nullptr;     // #ConfigHudRoot
	short m_symState = -1;
	short m_symPreviewRect = -1;
	short m_symTextCapture = -1;
	short m_symPopupOpen = -1;     // "popupopen" -- EditorSettingsPopupJs.h publishes its overlay
	short m_symMapViewOpen = -1;   // "mapviewopen" -- ConfigHudAltEnvironmentJs.h, the MAP view is up
	short m_symViewEdit = -1;      // "viewedit" -- ConfigHudPropsGizmoJs.h, the ENVIRONMENT tab is up
	// Last preview rect parsed from the JS-published "previewrect" (normalised window fractions).
	bool m_previewValid = false;
	bool m_demoScaleActive = false;
	float m_previewX0 = 0, m_previewY0 = 0, m_previewX1 = 0, m_previewY1 = 0;
	bool m_built = false;
	bool m_enabled = false;
	bool m_wasEnabled = false;  // for enter/exit edge detection
	Context m_context = Context::Demo; // latched in OnEnter, never re-read per frame
	bool m_liveCursor = false;         // live panel's own UI-cursor flag (demo uses the timeline's)
	bool m_textCapture = false;
	bool m_popupOpen = false;
	bool m_mapViewOpen = false;
	bool m_viewEdit = false;
	bool m_prevMovieHud = false; // movie-director card visibility to restore on exit
	bool m_povReady = false;     // debounced spectated-player-available flag
	int m_povReadyHold = 0;      // frames the current reading has held steady
	std::string m_lastState;
};

ConfigHud& ConfigHudRef();

// True while the Config panel is open. Free-function form so CameraTimelineHud can publish
// a "configOpen" state field without pulling in this header's class (same pattern as
// CameraEditor_HudViewName in Filmmaker.cpp).
bool ConfigHud_Enabled();

// Forwards one captured wheel notch (+1 up / -1 down) to the open Config panel so its
// inspector column scrolls. Called from MovieMode::OnMouseWheel, which consumes the wheel in
// UI-mouse mode so it can never reach the demo spectator behind the panel.
void ConfigHud_PushWheel(int notches);

// --- live-match (Insert) panel only -------------------------------------------------------
// True while the panel that is open is the offline live-match one rather than the demo
// workspace. Both are ConfigHud, but they are separate panels with separate state.
bool ConfigHud_IsLive();
// The live panel's own UI-cursor flag. Deliberately NOT the camera timeline's: routing the
// live panel through `camtl cursor` meant G hit the timeline's spectator-mode gate, which
// refuses in first person and answers by forcing the cursor OFF -- so on a live map G could
// only ever take the mouse away, never give it back.
bool ConfigHud_LiveCursor();
void ConfigHud_SetLiveCursor(bool on);
void ConfigHud_ToggleLiveCursor();

// Width/Height typed-input bridge (WM_CHAR -> Config JS). While capture is active the same
// physical keys are swallowed before they can also drive demo playback.
void ConfigHud_PushChar(unsigned charCode);
bool ConfigHud_TextCaptureActive();
bool ConfigHud_PopupOpen(); // Config's Editor Settings fly-out or the MAP view is up
bool ConfigHud_MapViewOpen(); // the ENVIRONMENT > MAP view covers the preview
bool ConfigHud_ViewEditing(); // the ENVIRONMENT tab edits in the preview (UiClick::Input::viewEdit)
// Editing shortcuts handed to the panel's JS, which owns the selection they act on
// ("delete", "duplicate"). Input thread.
void ConfigHud_PushKey(const char* name);

// Demo Config's scaled-preview rect; false for the live-game Insert overlay.
bool ConfigHud_ScaledHud(float& x0, float& y0, float& x1, float& y1);

// Console command entry: handles "mirv_filmmaker config ...". argc/args/cmd are forwarded
// from FilmmakerCommand.cpp's dispatcher (args->ArgV(0) == "mirv_filmmaker", ArgV(1) == "config").
void ConfigHud_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd);

} // namespace Filmmaker
