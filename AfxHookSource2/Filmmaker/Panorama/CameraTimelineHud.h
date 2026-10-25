#pragma once

// CameraTimelineHud: a native-CS2-styled camera TIMELINE / scrub bar.
//
// Mirrors the proven MovieHud / MarkerHud bridge pattern: a PanoramaBridge pinned
// to the in-game HUD panel, the panel built ONCE from embedded JS, and a small
// state JSON pushed each frame. JS buttons / the scrubber emit
// "mirv_filmmaker camtl ..." console commands (the same back-end as the hotkeys).
//
// The "state" attribute is pushed with playhead tick/time, marker list, and
// transport settings. The old embedded curve view was retired in favor of
// GraphEditorExperimentHud / GraphEditorJs.

#include "PanoramaBridge.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace advancedfx { class ICommandArgs; }

namespace Filmmaker {

class CameraTimelineHud {
public:
	// Main/UI thread, once per frame from Filmmaker::RunMainThreadFrame.
	void RunFrame();

	void SetVisible(bool v) { m_visible = v; }
	void Toggle() { m_visible = !m_visible; }
	bool Visible() const { return m_visible; }

	// UI-mouse mode: MirvInput is suspended so the OS cursor shows. While the camera
	// timeline / curve editor is open standalone this is forced on; otherwise it is the
	// regular native-demo-bar mouse toggle.
	//
	// EDITOR-HOSTED exception: inside Camera Editor Mode the cursor is NOT forced -- the
	// user must still be able to press G to drop into GAME mouse and fly the free cam to
	// frame a shot. So when hosted the cursor follows the toggle (m_cursor) only.
	void SetCursor(bool v) { m_cursor = v; }
	void ToggleCursor() { m_cursor = !m_cursor; }
	bool Cursor() const { return m_editorHosted ? m_cursor : (m_visible || m_cursor); }
	bool CursorForced() const { return m_editorHosted ? false : m_visible; }

	// Camera Editor Mode hosting: CameraEditorHud forces this panel open + docked and
	// owns the gameplay-HUD hide (driven by the "hosted" state flag this pushes to JS).
	void SetEditorHosted(bool v) { m_editorHosted = v; }
	bool EditorHosted() const { return m_editorHosted; }

	void SetView(int) { }
	void ToggleView() { }
	int View() const { return 0; }

	// The per-keyframe menu (right-click a diamond) is a MODAL surface: while it is up the
	// wheel belongs to its style list, not to the workspace behind it. The JS publishes the flag
	// on the panel root; RunFrame reads it back each frame.
	bool MenuOpen() const { return m_menuOpen; }

	// The camera CS2's demo-bar camera button shows (0 first person, 1 chase, 2 free cam), -1 when
	// the label is missing or unrecognised. Read back from the JS each frame.
	int NativeCameraMode() const { return m_nativeCameraMode; }

	// CS2's own spectator state, read off the native demo bar's panel each frame
	// (GetDemoControllerState): the spectated PAWN index (-1 in free cam or when unknown) and the
	// observer mode (2 first person, 3 chase, 4 roaming; -1 unknown). Correct in third person, where
	// a GOTV demo's local observer reads nothing and the render-eye match cannot see the player.
	int NativeSpectatedPawn() const { return m_nativeSpecPawn; }
	int NativeObserverMode() const { return m_nativeObsMode; }
	// GetDemoPlayers as "controller,pawn,alive,team,slot;..." (CustomizeTargetMath::ParseDemoPlayers).
	// Empty while no demo controller answers. Refreshed a few times a second.
	const std::string& NativeDemoPlayers() const { return m_nativeDemoPlayers; }
	// Main thread: spectate this player controller the way CS2's own player list does. Runs on the
	// next RunFrame, after the panel is built. The engine ignores a dead player.
	void RequestNativeSpectate(int controllerIndex) { m_spectateRequest = controllerIndex; }

	// Any thread: whether a client-px point is on CS2's native demo bar (the JS publishes its rect
	// each frame as 'barrect'). False while the bar is hidden or no demo plays.
	bool BarContains(int x, int y) const {
		return m_barValid.load() && x >= m_bar[0].load() && x < m_bar[2].load() && y >= m_bar[1].load() && y < m_bar[3].load();
	}

	// WndProc/input thread: accumulate notches. Main thread: DispatchWheel drains them into the
	// panel's JS. Same split, and the same reason, as ConfigHud's channel.
	void PushWheel(int notches);

	void RequestEval(const std::string& js) { m_evalQueue.push_back(js); }

private:
	void* FindRoot();
	bool BuildIfNeeded();
	void Teardown();
	std::string BuildStateJson();
	// One-shot after a build: hands the JS the GTA V shake style list from the C++ table, so the
	// right-click keyframe menu and the engine agree on what index means which style.
	void PushShakeStyles();

	PanoramaBridge m_bridge;
	void* m_hudPanel = nullptr; // HUD context we built against (rebuild if it changes)
	void* m_root = nullptr;     // #CamTimelineRoot
	short m_symState = -1;
	short m_symMenuOpen = -1;
	bool m_menuOpen = false;        // the keyframe menu is up and owns the wheel
	short m_symCamMode = -1;        // "cammode" -- the JS publishes CS2's camera button label
	int m_nativeCameraMode = -1;
	short m_symSpecState = -1;      // "specstate" -- "<spectated pawn> <observer mode>"
	short m_symDemoPlayers = -1;    // "demoplayers" -- the native player list, see NativeDemoPlayers
	int m_nativeSpecPawn = -1;
	int m_nativeObsMode = -1;
	std::string m_nativeDemoPlayers;
	int m_spectateRequest = -1;
	short m_symBarRect = -1;        // "barrect" -- the native demo bar's on-screen rect
	std::string m_barRectText;
	std::atomic<int> m_bar[4] = {};
	std::atomic<bool> m_barValid{ false };
	void ReadBarRect();             // main thread
	void ReadNativeSpectator();     // main thread, after render()
	int m_pendingWheel = 0;         // guarded by m_wheelMutex
	std::mutex m_wheelMutex;
	void DispatchWheel();           // main thread only
	bool m_built = false;
	bool m_visible = false; // hidden until 'camtl open'
	bool m_cursor = false;  // global freecam cursor mode (G toggles; never opens the editor)
	bool m_editorHosted = false; // true while hosted inside Camera Editor Mode
	std::string m_lastState;
	std::vector<std::string> m_evalQueue;
};

CameraTimelineHud& CameraTimelineHudRef();

// Console command entry: handles "mirv_filmmaker camtl ...". argc/args/cmd are forwarded
// from FilmmakerCommand.cpp's dispatcher (args->ArgV(0) == "mirv_filmmaker", ArgV(1) == "camtl").
void CameraTimeline_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd);

// Selects the current (or first) camera-path marker for the editor/timeline, if any exist.
// Used by CameraTimelineCommand.cpp ("camtl open/toggle"). Returns false (no-op) when the path
// has no markers.
bool FocusEditorCameraIfAny();

} // namespace Filmmaker
