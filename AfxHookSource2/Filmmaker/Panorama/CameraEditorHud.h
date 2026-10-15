#pragma once

// CameraEditorHud: a dedicated "Camera Editor Mode" workspace shell rendered with
// CS2's native Panorama in the in-game HUD (CSGOHud) context.
//
// This is a PRESENTATION SHELL over the existing camera-editing back-end; it adds no
// new editing logic. While enabled it:
//   * frames the live game as a "preview" in the top-left (a right-side property
//     inspector + a bottom timeline + a dimmed letterbox make the open area read as a
//     viewport -- note the preview is a CROP of the full-screen frame, not a scaled
//     copy, since Panorama only composites overlays on top of the full game render);
//   * docks a BOTTOM panel under the preview whose mode is selectable (BottomMode):
//       - CameraTimeline (default): the custom camera timeline (CameraTimelineHud) overlay;
//       - Graph: the experimental graph curve editor overlay;
//       - Native: kept only for the `editor curveeditor native` console command -- shows no
//         overlay (just the bottom tab bar). The native CS2 demo bar is HIDDEN the whole time
//         the editor is open, so it can never bleed through behind the overlays.
//     A persistent BOTTOM tab bar (drawn by CameraEditorJs where CS2's old CAM EDITOR / MOUSE
//     buttons sat) flips between Camera Timeline and Graph; all are driven by the same
//     "mirv_filmmaker editor curveeditor ..." console commands;
//   * surfaces selected-camera settings (FOV, roll, interpolation, segment speed,
//     easing, freeze/live, speed mode) as Slider / stepper / cycle controls that issue
//     those same commands;
//   * hides the rest of the gameplay HUD (radar/health/ammo/scoreboard) AND the native demo
//     bar for a clean workspace -- the actual hide is centralised in CameraTimelineHud's JS
//     via the "hosted" state flag, so a single script owns native-HUD visibility.
//
// Mirrors the proven MovieHud / CameraTimelineHud bridge pattern: a PanoramaBridge
// pinned to the in-game HUD panel, a panel built ONCE from embedded JS, and a small
// state JSON pushed each frame.
//
//   C++ -> JS : attribute "state" (camera readouts + selected-key settings), then
//               $.CamEditor.render().
//   JS  -> C++: buttons / sliders issue "mirv_filmmaker ..." console commands.

#include "PanoramaBridge.h"
#include "../Movie/TimelineDock.h" // the dock mode lives here now, shared with CONFIG

#include <mutex>
#include <string>
#include <vector>

namespace advancedfx { class ICommandArgs; }

namespace Filmmaker {

class CameraEditorHud {
public:
	// Main/UI thread, once per frame from Filmmaker::RunMainThreadFrame. Runs LAST so
	// its host orchestration (timeline hosting + HUD hide) wins over the other panels.
	void RunFrame();

	void SetEnabled(bool v) { m_enabled = v; }
	void Toggle() { m_enabled = !m_enabled; }
	bool Enabled() const { return m_enabled; }

	// TRUE scaled preview viewport (render-layer): when on, the whole rendered frame is
	// scaled down into the preview rect via CViewportScaler instead of showing a crop.
	// Only takes effect while the editor is open and not recording. Safe to toggle anytime.
	void SetScale(bool v) { m_scaleEnabled = v; }
	void ToggleScale() { m_scaleEnabled = !m_scaleEnabled; }
	bool ScaleEnabled() const { return m_scaleEnabled; }

	enum class BottomMode { Native = 0, CameraTimeline = 1, Graph = 2 };

	// Bottom editor selection. Native = CS2's own demo timeline stays visible; CameraTimeline
	// and Graph are explicit overlays opened by buttons.
	//
	// THE MODE IS NOT STORED HERE any more -- it belongs to Movie/TimelineDock.h, because the
	// CONFIG panel shows the same dock and could not switch tabs while this was private state.
	// These stay as the editor's spelling of the shared value so every existing caller and
	// console alias keeps working.
	static BottomMode FromTab(DockTab t) {
		return t == DockTab::Graph ? BottomMode::Graph
			: (t == DockTab::Camera ? BottomMode::CameraTimeline : BottomMode::Native);
	}
	static DockTab ToTab(BottomMode m) {
		return m == BottomMode::Graph ? DockTab::Graph
			: (m == BottomMode::CameraTimeline ? DockTab::Camera : DockTab::Native);
	}
	void SetBottomMode(BottomMode mode) { TimelineDock_SetTab(ToTab(mode)); }
	void SetUseTimeline(bool v) { SetBottomMode(v ? BottomMode::CameraTimeline : BottomMode::Graph); }
	void ToggleUseTimeline() {
		SetBottomMode(UseTimeline() ? BottomMode::Graph : BottomMode::CameraTimeline);
	}
	bool UseTimeline() const { return TimelineDock_Tab() == DockTab::Camera; }
	BottomMode GetBottomMode() const { return FromTab(TimelineDock_Tab()); }

	// Game-HUD visibility while the editor is open. HideAll = clean workspace (the original
	// auto-hide, default); InGame = keep radar + HP/ammo but strip the spectator observer
	// panel (player avatar/name bar); ShowAll = full spectator HUD. The actual show/hide lives
	// in CameraTimelineHud's JS, which reads this through the "hudView" state field.
	enum class HudView { HideAll = 0, InGame = 1, ShowAll = 2 };
	void SetHudView(HudView v) { m_hudView = v; }
	void CycleHudView() { m_hudView = (HudView)(((int)m_hudView + 1) % 3); }
	// What is applied: a running render job's HUD mode while it lasts, otherwise the user's pick.
	HudView GetHudView() const { return m_hudViewOverride >= 0 ? (HudView)m_hudViewOverride : m_hudView; }
	// The user's pick -- the only value that is persisted. A render job sets its HUD mode as an
	// override instead of overwriting the pick, so a job that never finishes cleanly (a crash, a
	// quit mid-render) cannot leave Config and the editor stuck on the job's "hide everything".
	HudView UserHudView() const { return m_hudView; }
	void SetHudViewOverride(HudView v) { m_hudViewOverride = (int)v; }
	void ClearHudViewOverride() { m_hudViewOverride = -1; }

	// Viewport/HUD debug overlay: on-screen readout of window/render-target/viewport geometry so
	// the custom editor viewport can be compared 1:1 against the normal game viewport.
	void SetDebugOverlay(bool v) { m_debugOverlay = v; }
	void ToggleDebugOverlay() { m_debugOverlay = !m_debugOverlay; }
	bool DebugOverlay() const { return m_debugOverlay; }

	// The scaled-preview rect (normalised window fractions, x0 y0 x1 y1) the world blit uses,
	// plus whether the scaled viewport is live. The timeline HUD reads these to scale the native
	// game HUD into the SAME rect so the HUD lines up with the shrunk world preview.
	// NOTE: the effective scale flag, not the persisted toggle -- the Render workspace forces
	// the scaled blit on (see UpdateScaleRequest). The native game HUD must follow the SAME
	// decision or it lands outside the shrunk world preview.
	bool ScaledHudActive() const { return m_scaleEffective && m_previewValid; }
	bool PreviewRect(float& x0, float& y0, float& x1, float& y1) const {
		x0 = m_previewX0; y0 = m_previewY0; x1 = m_previewX1; y1 = m_previewY1; return m_previewValid;
	}

	std::string DebugStateJson() { return BuildStateJson(); }

	void RequestEval(const std::string& js) { m_evalQueue.push_back(js); }
	void RequestOpenCustomize() { m_openCustomizeRequested = true; }

	// Monotonic pulse token. The JS flashes the inspector whenever this changes; a counter rather
	// than a bool so repeated presses each produce a visible pulse instead of latching.
	void FlashInspector() { ++m_flashToken; }
	unsigned FlashToken() const { return m_flashToken; }

	// True while the Customize Player modal (custOverlay) is open, per the JS-published
	// "customizeopen" attribute (read every frame in RunFrame, same pattern as previewrect).
	// The input layer (MovieMode + GetSuspendMirvInput) uses this to make the modal an
	// exclusive surface: no click/key/scroll should leak to the game while it's up.
	bool CustomizeModalOpen() const { return m_customizeOpen; }

	// True while a Panorama field in OUR UI is collecting typed characters (currently the Render
	// workspace's FPS / resolution boxes). CS2 does not give an in-game HUD TextEntry real
	// keyboard focus, so characters are captured from WM_CHAR and pushed in -- the same route the
	// Customize modal's search boxes use. While this is set, keys must not also drive the game.
	bool TextCaptureActive() const { return m_textCapture; }

	// Captured-input pipe for the Customize modal (see Filmmaker.h CameraEditor_CustomizePush*).
	// Called from the WndProc/input thread; enqueues under a lock. Drained + dispatched to the JS
	// on the main thread in RunFrame (DispatchCustomizeInput). Wheel events keep their coordinates so
	// JS can decide whether to zoom the preview or scroll the controls; typed characters keep order.
	void PushCustomizeWheel(int notches, int x, int y);
	void PushCustomizeChar(unsigned charCode);

	// Captured-wheel pipe for the INSPECTOR COLUMN (distinct from the Customize modal's, which is
	// only alive while that modal is open). Same reason it has to exist at all: Panorama never
	// receives the wheel in this in-game HUD context, so `overflow: scroll` on the column drew a
	// scrollbar nothing could drive and pointing at the middle of the inspector and scrolling did
	// nothing. Captured on the WndProc/input thread, drained on the main thread in RunFrame.
	void PushWheel(int notches);

private:
	void* FindRoot();
	bool BuildIfNeeded();
	void Teardown();
	std::string BuildStateJson();
	void OnEnter(); // one-shot: host the timeline, hide MovieHud (no free-cam / no jump on open)
	void OnExit();  // one-shot: un-host the timeline, restore MovieHud, stop scrub
	void UpdateScaleRequest(); // publish the preview rect to the render-layer viewport scaler
	void UpdateCustomizeModalState(); // reads the "customizeopen" attribute JS published this frame
	void DispatchCustomizeInput();    // main thread: drain the captured-input queue -> RunScript into the modal JS
	void DispatchWheel();             // main thread: drain the inspector wheel queue -> api.wheel()

	PanoramaBridge m_bridge;
	void* m_hudPanel = nullptr; // HUD context we built against (rebuild if it changes)
	void* m_root = nullptr;     // #CamEditorRoot
	short m_symState = -1;
	short m_symPreviewRect = -1; // "previewrect" -- JS publishes the preview rect fractions
	short m_symDebugPanels = -1; // "debugpanels" -- JS publishes measured HUD/editor rects
	short m_symRenderWorkspace = -1; // "renderworkspace" -- final JS-projected Render visibility
	short m_symRenderHit = -1; // "renderhit" -- preview/catcher geometry used for click-boundary diagnostics
	short m_symCustomizeOpen = -1; // "customizeopen" -- JS publishes custOverlay.visible ("0"/"1")
	short m_symTextCapture = -1;   // "textcapture" -- JS publishes whether a text field has focus
	bool m_customizeOpen = false;
	bool m_textCapture = false; // JS-published "textcapture" attribute (a field wants keystrokes)
	// Captured-input queue for the modal (WndProc thread -> main thread). Guarded by m_custInputMutex.
	std::mutex m_custInputMutex;
	struct CustomizeWheelEvent { int notches; int x; int y; };
	std::vector<CustomizeWheelEvent> m_custWheelEvents; // ordered wheel events; coords decide preview zoom vs scroll
	std::vector<unsigned> m_custChars;   // ordered typed character codes pending
	// Inspector-column wheel. Coalesced into ONE integer rather than queued: only the sign and the
	// magnitude matter, so a fast flick becomes one proportional scroll instead of a queue that
	// drains over several frames.
	std::mutex m_wheelMutex;
	int m_pendingWheel = 0;
	bool m_built = false;
	bool m_enabled = false;     // Camera Editor Mode on/off
	bool m_scaleEnabled = true;   // user preference (persisted): scaled-preview viewport on/off
	bool m_scaleEffective = true; // m_scaleEnabled OR forced by the Render workspace; recomputed
	                              // each frame in UpdateScaleRequest and read by ScaledHudActive
	HudView m_hudView = HudView::ShowAll; // game-HUD visibility while the editor/Config is open (default: full game UI)
	int m_hudViewOverride = -1;           // a render job's HudView for its duration, -1 = none
	bool m_debugOverlay = false;         // viewport/HUD debug readout overlay
	bool m_debugOverlayWasOn = false;    // previous frame's m_debugOverlay; drives the one-shot
	                                     // hide when it clears while the editor is CLOSED (the
	                                     // panel hides itself from pushed state, so something
	                                     // has to push once more -- see RunFrame)
	// Last preview rect parsed from the JS-published "previewrect" (normalised window fractions).
	float m_previewX0 = 0, m_previewY0 = 0, m_previewX1 = 0, m_previewY1 = 0;
	bool m_previewValid = false;
	bool m_wasEnabled = false;  // for enter/exit edge detection
	bool m_prevMovieHud = false; // MovieHud visibility to restore on exit
	std::string m_lastState;
	std::vector<std::string> m_evalQueue;
	bool m_openCustomizeRequested = false;
	unsigned m_flashToken = 0;
};

CameraEditorHud& CameraEditorHudRef();

// Console command entry: handles "mirv_filmmaker editor ...". argc/args/cmd are forwarded
// from FilmmakerCommand.cpp's dispatcher (args->ArgV(0) == "mirv_filmmaker", ArgV(1) == "editor").
void CameraEditor_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd);

// One captured wheel notch (+1 = up) for the editor's inspector column. Called from
// MovieMode::OnMouseWheel, which consumes the wheel in UI-mouse mode so it cannot reach the demo
// spectator behind the panel. Thread-safe.
void CameraEditor_PushWheel(int notches);

} // namespace Filmmaker
