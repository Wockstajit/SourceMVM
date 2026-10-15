#pragma once

// MovieMode: the in-demo "camera director". It owns the camera MODE state
// machine (Default first-person / ThirdPerson / FreeCam) and the X-ray / cursor
// toggles, and translates captured input (mouse wheel, LMB/RMB, X, G) into
// actions.
//
// Threading: input taps (On*) run on the WndProc/input thread and only ENQUEUE
// work (mode changes + console commands) under a mutex. FlushActions() runs on
// the main/UI thread (from Filmmaker::RunMainThreadFrame) and is the only place
// that touches the engine (ExecuteClientCmd) or the camera system, mirroring how
// the rest of the Filmmaker module keeps engine work on the main thread.
//
// X-ray starts OFF in every demo: FlushActions forces spec_show_xray 0 once per demo session
// (ApplyXrayDefault), because CS2's own default of 1 paints through-wall silhouettes over every
// shot. X and the Camera Editor's X-Ray setting still turn it back on for that demo.
//
// It reuses CS2's own spectator commands (spec_mode / spec_next / spec_prev /
// spec_show_xray) rather than hand-rolling entity/observer manipulation, and
// drives the free camera through CameraBridge_SetFreeCamEnabled.

#include <mutex>
#include <vector>
#include <string>

namespace Filmmaker {

class MovieMode {
public:
	enum class Mode { Default = 0, ThirdPerson = 1, FreeCam = 2 };

	// --- input taps (WndProc / input thread). Return true if consumed. ---
	bool OnKey(int vkey, bool down);
	bool OnMouseButton(int button, bool down); // 0 = left, 1 = right
	bool OnMouseWheel(int delta, bool shiftDown, bool ctrlDown, int x = -1, int y = -1);
	// Left button down at client (x, y). True = it is a click on our UI in GAME mode: UI mode is
	// switched on and the caller hands this press
	// (and its release) straight to the game window, skipping the camera's click handling.
	// UiClickMath.h has the rule.
	bool OnUiClick(int x, int y, int clientW, int clientH);

	// --- main thread ---
	void FlushActions();
	// Once per frame, after the workspaces committed their preview: where the UI is, for OnUiClick.
	// preview = window fractions (WorkspacePreview), bar = the native demo bar in client px.
	void SetUiClickRegions(bool previewValid, float px0, float py0, float px1, float py1,
		bool barValid, float bx0, float by0, float bx1, float by1);

	// --- state for the HUD (thread-safe reads) ---
	Mode GetMode();
	bool GetXray();
	bool GetCursor();
	const char* ModeName();

	// Programmatic mode set (e.g. when the user toggles free cam via console).
	// keepCursor: skip the usual "camtl cursor off" side-effect -- used by UI surfaces
	// (Render workspace camera-source buttons) that must stay clickable after the switch.
	void SetMode(Mode mode, bool keepCursor = false);
	void SyncFromFreeCam(bool freeCamEnabled);

	// Demo skip by the shared step (5 s): direction < 0 back, > 0 forward. Any thread.
	// Used by the arrow keys and the native bar's -/+ buttons (`mirv_filmmaker skip`).
	void SkipSeconds(int direction);

private:
	void ApplyMode(Mode m, bool keepCursor); // main thread only
	void ApplyXrayDefault();                // main thread only: forces X-ray off once per demo
	void EnqueueCmd(const std::string& c);  // any thread (locks)

	std::mutex m_mutex;
	Mode m_mode = Mode::Default;
	bool m_xray = false;
	// Retained for the HUD "cursor" state field only. The director no longer
	// intercepts clicks at all (CS2's own spectator + native demo UI handle
	// them), so this stays true and is informational.
	bool m_cursor = true;
	bool m_controlDown = false; // input-thread modifier state for Ctrl+Z curve undo
	bool m_shiftDown = false;   // input-thread modifier state for Ctrl+Shift+Z curve redo
	bool m_spaceDown = false;   // input-thread Space state; swallows OS auto-repeat so transport toggles once per press
	bool m_modeDirty = false;
	Mode m_pendingMode = Mode::Default;
	bool m_pendingKeepCursor = false; // consumed by the next ApplyMode
	std::vector<std::string> m_cmdQueue;
	// Click-to-UI-mode regions (guarded by m_mutex; written by SetUiClickRegions).
	bool m_uiPreviewValid = false, m_uiBarValid = false;
	float m_uiPreview[4] = {}, m_uiBar[4] = {};

	// X-ray default-off bookkeeping (main thread only -- ApplyXrayDefault):
	std::wstring m_xrayDefaultPath;  // demo we have already forced spec_show_xray 0 for
	int m_xrayDemoGoneFrames = 0;    // consecutive demo-less frames; outlasts seek blips
};

// Singleton accessor (constructed on first use).
MovieMode& MovieModeRef();

} // namespace Filmmaker
