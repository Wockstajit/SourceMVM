#include "../Config/FilmmakerSettingsStore.h"
#include "../Environment/EnvironmentScene.h"
#include "../Props/Props.h"
#include "MovieMode.h"
#include "../MouseCapture.h" // MouseCapture_TakeForCamera
#include "../Window/WindowResize.h" // WindowResize_GetClientSize: place clicks / notches on the preview

#include "CameraBridge.h"
#include "CameraPath.h"
#include "FollowCamera.h"
#include "../Filmmaker.h"            // CameraTimeline_Visible()
#include "../MvmTest.h"              // mvm_test offline FX menu
#include "HudBlackout.h"          // F8: clear every overlay while directing
#include "../Panorama/ConfigHud.h"   // ConfigHud_Enabled: Config panel gets editor-style G/wheel input priority
#include "../Render/RenderCommand.h"  // Render_IsModalOpen / Render_IsJobActive: Esc leaves the Render workspace
#include "../Panorama/CameraEditorHud.h" // CameraEditor_PushWheel: the editor's inspector scrolls the same way
#include "../Panorama/MovieHud.h"
#include "../Debug/MvmDebugLog.h" // MvmDebugLog_*
#include "../Demo/PlayingDemoPath.h" // ResolvePlayingDemoPath: per-demo X-ray default
#include "../Relight/RelightLightEditor.h" // LightEditorKey: F/Esc while the light editor is on
#include "../../MirvTime.h"
#include "../../ClientEntitySystem.h" // AfxGetLocalObserverState: the camera CS2 is showing
#include "../../MirvCommands.h"       // MirvFov_*: Ctrl+wheel FOV in CS2's own cameras
#include "FreeCamStepMath.h"          // NextFreeCamSpeed: Shift+wheel steps sv_specspeed the same way

#include "../../../Deps/release/prop/AfxHookSource/SourceSdkShared.h"
#include "../../../Deps/release/prop/cs2/sdk_src/public/cdll_int.h"
#include "../../../Deps/release/prop/cs2/sdk_src/public/icvar.h" // sv_specspeed

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <sstream>

// Provided by main.cpp (same global the Filmmaker "Watch" action uses).
extern SOURCESDK::CS2::ISource2EngineToClient* g_pEngineToClient;

namespace Filmmaker {

// Virtual-key codes for the director keys (avoid relying on <windows.h> macros
// for letters, which don't exist).
namespace {
	constexpr int kVK_X = 0x58;
	constexpr int kVK_F8 = 0x77;    // toggle the director help/status HUD
	constexpr int kVK_SHIFT = 0x10; // hold = slow/fine free-cam movement
	constexpr int kVK_CONTROL = 0x11;
	constexpr int kVK_SPACE = 0x20; // pause / resume the demo
	constexpr int kVK_LEFT = 0x25;  // skip -15s
	constexpr int kVK_RIGHT = 0x27; // skip +15s
	constexpr int kVK_ESC = 0x1B;   // close menu / cancel reposition / stop preview
	// Camera-marker / dolly path keys (BO2-style):
	constexpr int kVK_K = 0x4B;     // place a camera marker
	constexpr int kVK_L = 0x4C;     // delete the aimed-at marker
	constexpr int kVK_F = 0x46;     // edit the aimed-at marker
	constexpr int kVK_C = 0x43;     // Config open: Ctrl+C copies the selected scene object
	constexpr int kVK_D = 0x44;     // Config open: Ctrl+D duplicates the selected scene object
	constexpr int kVK_V = 0x56;     // Config open: Ctrl+V pastes it under the mouse
	constexpr int kVK_T = 0x54;     // Config open: fire one weapon recoil test shot
	constexpr int kVK_INSERT = 0x2D; // mvm_test FX menu toggle (offline live match)
	constexpr int kVK_Z = 0x5A;     // Ctrl+Z: undo last curve edit
	constexpr int kVK_A = 0x41;     // Ctrl+A: select all keyframes (graph editor)
	constexpr int kVK_DELETE = 0x2E;
	constexpr int kVK_BACKSPACE = 0x08;
	constexpr int kVK_J = 0x4A;     // arm / play / stop the camera path

	constexpr int kSkipSeconds = 5; // demo skip step: arrow keys and the bar's -/+ buttons

	// CS2 OBS_MODE_* enum: FIXED=1, IN_EYE=2, CHASE=3, ROAMING=4 (huddemocontroller.js). The three
	// demo cameras, in the order CS2's own camera button steps them.
	const char* SpecModeCommand(MovieMode::Mode m) {
		switch (m) {
		case MovieMode::Mode::ThirdPerson: return "spec_mode 3"; // chase
		case MovieMode::Mode::FreeCam: return "spec_mode 4";     // roaming
		default: return "spec_mode 2";                           // in-eye
		}
	}
	MovieMode::Mode ModeFromObserver(uint8_t obsMode) {
		if (obsMode == 3) return MovieMode::Mode::ThirdPerson;
		if (obsMode == 4) return MovieMode::Mode::FreeCam;
		return MovieMode::Mode::Default;
	}

	// The director input taps must do NOTHING (and must not consume the event)
	// unless a demo is actively playing. Otherwise they swallow main-menu /
	// live-game mouse and key input. Called from the WndProc thread.
	bool IsDemoActive() {
		if (!g_pEngineToClient)
			return false;
		auto pDemo = g_pEngineToClient->GetDemoFile();
		return pDemo && (pDemo->IsPlayingDemo() || pDemo->IsDemoPaused());
	}
}

// Skip the demo backward/forward by kSkipSeconds via an absolute demo_gototick (CS2 has no
// relative skip command). Tick rate is derived from MirvTime rather than hard-coded. With the
// camera timeline open the target is clamped to the path range and scrubbed instead.
void MovieMode::SkipSeconds(int direction) {
	int tick = 0;
	if (direction == 0 || !g_MirvTime.GetCurrentDemoTick(tick)) return;
	float ipt = g_MirvTime.interval_per_tick_get();
	int ticksPerSec = (ipt > 0.0f) ? (int)(1.0f / ipt + 0.5f) : 64;
	int delta = kSkipSeconds * ticksPerSec;
	int target = tick + (direction > 0 ? +delta : -delta);
	std::ostringstream oss;
	if (CameraTimeline_Visible() && CameraPathRef().HasPathRange()) {
		target = CameraPathRef().ClampToPathTick(target);
		oss << "mirv_filmmaker camtl scrub " << target;
	} else {
		if (target < 0) target = 0;
		oss << "demo_gototick " << target;
	}
	EnqueueCmd(oss.str());
}

MovieMode& MovieModeRef() {
	static MovieMode s_instance;
	return s_instance;
}

void MovieMode::EnqueueCmd(const std::string& c) {
	std::lock_guard<std::mutex> lk(m_mutex);
	m_cmdQueue.push_back(c);
}

bool MovieMode::OnMouseWheel(int delta, bool shiftDown, bool ctrlDown, int x, int y) {
	// LIVE Insert panel: consume the notch and hand it to the panel's own scroll column.
	//
	// This has to come BEFORE the !IsDemoActive() bail below, which is why the panel could not
	// be scrolled on a live map at all -- the wheel returned false here and went straight to the
	// game. (An older branch here gated on the since-removed TestHud panel; the
	// thing Insert actually opens is ConfigHud, so the branch never ran either.
	//
	// Panorama does not take the wheel away from the game in this HUD context, so consuming it
	// (return true) is what keeps it off the live player behind the panel.
	if (ConfigHud_IsLive()) {
		if (ConfigHud_LiveCursor()) {
			if (delta != 0) ConfigHud_PushWheel(delta > 0 ? +1 : -1);
			return true;
		}
		return false; // cursor handed back to the game: the wheel is the player's again
	}
	if (!IsDemoActive())
		return false;
	if (delta == 0)
		return false;

	// Customize modal open: the modal is an EXCLUSIVE surface, exactly like the editor's UI-mouse
	// mode -- NOTHING may reach the game. In the in-game HUD, letting the wheel fall through to CS2
	// still drives the demo spectator (switch player / cycle first-third person) even when a Panorama
	// scroll container is under the cursor -- Panorama does not take the wheel away from the game
	// here. So we CONSUME it (return true -> main.cpp `return 0` before CallWindowProcW) and forward
	// the notch into the modal's JS, which scrolls its own right column / open dropdown list itself
	// (manual transform-scroll, see CameraEditorCustomizeJs.h custWheel). Net: the list scrolls and
	// the demo behind the modal never moves.
	if (CameraEditor_CustomizeModalOpen()) {
		CameraEditor_CustomizePushWheel(delta > 0 ? +1 : -1, x, y);
		return true;
	}

	// The per-keyframe menu (right-click a diamond on the camera timeline) is modal in exactly the
	// same way: while it is up, its style list owns the wheel. Without this the notch fell through
	// to the workspace behind the menu and scrolled the CAMERA EDITOR'S INSPECTOR while the list
	// under the pointer did nothing -- the only way through the styles was to drag the list.
	// Checked BEFORE the workspace branch below, which is what used to swallow it.
	if (CameraTimeline_MenuOpen()) {
		CameraTimeline_PushWheel(delta > 0 ? +1 : -1);
		return true;
	}

	// In the director FREE CAM the wheel drives the camera when a modifier is held:
	//   Ctrl+scroll  = FOV zoom -- up = zoom in (lower FOV), down = zoom out (higher FOV),
	//   Shift+scroll = move SPEED (documented control).
	// Checked first so framing a shot works no matter which editor surface is open -- EXCEPT while
	// the graph dock is up: its timeline owns Ctrl/Shift+wheel (zoom / pan), and letting the notch
	// fall through to the FOV zoom is the reported "Ctrl+wheel changed my camera FOV while I was
	// zooming the timeline". Hand it to the editor's wheel pipe, which reaches $.GraphExp.wheel.
	if (CameraBridge_GetFreeCamEnabled() && (ctrlDown || shiftDown)) {
		if (GraphEditorExperiment_Enabled()) {
			CameraEditor_PushWheel(delta > 0 ? +1 : -1);
			return true;
		}
		if (ctrlDown) CameraBridge_AdjustFreeCamFov(delta > 0 ? +1 : -1);
		else CameraBridge_AdjustFreeCamSpeed(delta > 0 ? +1 : -1);
		return true;
	}

	// Plain wheel: over the game it always steps CS2's camera (UiClickMath.h). Inside a workspace,
	// a notch over a panel scrolls that panel instead -- Panorama never sees the wheel in this
	// context, so the panel scrolls its own column from this pipe. Config wins the tie when both
	// flags somehow read true; they are mutually exclusive in practice (CameraEditor_Set closes
	// Config). The experimental graph editor owns the wheel (timeline zoom) while it is open.
	if (GraphEditorExperiment_Enabled())
		return true;
	ctrlDown = ctrlDown || m_controlDown;
	shiftDown = shiftDown || m_shiftDown;
	const UiClick::Input where = ClickInput(x, y);
	// No position (-1, -1) = the notch came during a look-around drag, where CS2 parks the cursor
	// wherever it likes: it is the game's, whatever is under the parked pointer.
	const bool cycle = (x < 0 && y < 0) || UiClick::WheelCyclesCamera(where);
	const bool modifier = ctrlDown || shiftDown;
	if (MvmDebugLog_Active())
		MvmDebugLog_Linef("movie.wheel", "delta=%d at %d,%d shift=%d ctrl=%d workspace=%d preview=%d -> %s",
			delta, x, y, shiftDown ? 1 : 0, ctrlDown ? 1 : 0, where.workspace ? 1 : 0, where.preview.valid ? 1 : 0,
			!cycle ? "panel" : (ctrlDown ? "fov" : (shiftDown ? "speed" : "camera")));
	// Over the game in CS2's own cameras: Ctrl+wheel = FOV in any of them, Shift+wheel = CS2's
	// free-cam fly speed. Applied on the main thread (ApplyNativeCameraControls).
	if (cycle && modifier) {
		std::lock_guard<std::mutex> lk(m_mutex);
		if (ctrlDown) m_pendingFov += delta > 0 ? +1 : -1;
		else m_pendingSpeed += delta > 0 ? +1 : -1;
		return true;
	}
	if (!cycle) {
		if (ConfigHud_Enabled())
			ConfigHud_PushWheel(delta > 0 ? +1 : -1);
		else
			CameraEditor_PushWheel(delta > 0 ? +1 : -1);
		return true;
	}

	// Steps CS2's own demo camera, wrapping around at the ends:
	// up   = First person -> Chase -> Free cam -> First person ...
	// down = First person -> Free cam -> Chase -> First person ...
	// Applied on the main thread (FlushActions).
	std::lock_guard<std::mutex> lk(m_mutex);
	m_pendingCycle += delta > 0 ? +1 : -1;
	return true;
}

void MovieMode::SetUiClickRegions(bool previewValid, float px0, float py0, float px1, float py1) {
	std::lock_guard<std::mutex> lk(m_mutex);
	m_uiPreviewValid = previewValid;
	m_uiPreview[0] = px0; m_uiPreview[1] = py0; m_uiPreview[2] = px1; m_uiPreview[3] = py1;
}

// Where (x, y) falls this frame: inside a workspace or not, and the workspace's preview rect.
UiClick::Input MovieMode::ClickInput(int x, int y) {
	UiClick::Input in;
	in.uiModeOn = CameraTimeline_WantsCursor();
	in.workspace = CameraEditor_Active() || ConfigHud_Enabled();
	// The keyframe shake menu (right-click a diamond) opens over the preview too: a click on its
	// close button or rows is the menu's, not a switch to GAME mode.
	in.popupOpen = CameraEditor_PopupOpen() || ConfigHud_PopupOpen() || CameraTimeline_MenuOpen();
	in.x = (float)x; in.y = (float)y;
	int clientW = 0, clientH = 0;
	WindowResize_GetClientSize(clientW, clientH);
	std::lock_guard<std::mutex> lk(m_mutex);
	if (m_uiPreviewValid)
		in.preview = UiClick::FromFractions(m_uiPreview[0], m_uiPreview[1], m_uiPreview[2], m_uiPreview[3],
			(float)clientW, (float)clientH);
	return in;
}

// A camera path is playing, armed to play, or waiting for its start seek to land.
static bool PathPlaybackOwnsView() {
	const CameraPath& cp = CameraPathRef();
	const CameraPath::Mode mode = cp.GetMode();
	return mode == CameraPath::Mode::PreviewPlaying || mode == CameraPath::Mode::PreviewArmed
		|| cp.PlaybackPending();
}

bool MovieMode::OnUiClick(int x, int y) {
	if (!IsDemoActive() || ConfigHud_IsLive())
		return false;
	// Surfaces that place something with the click, or own every click while they are up, are left
	// to OnMouseButton exactly as before.
	if (FollowCameraRef().Repositioning() || CameraPathRef().GetMode() == CameraPath::Mode::Reposition
		|| CameraPathRef().MenuOpen() || GraphEditorExperiment_WantsCursor() || CameraEditor_CustomizeModalOpen())
		return false;

	const UiClick::Input in = ClickInput(x, y);
	const bool toUi = UiClick::ClickSwitchesToUiMode(in);
	const bool toGame = UiClick::ClickSwitchesToGameMode(in);
	if (MvmDebugLog_Active())
		MvmDebugLog_Linef("input.uiclick",
			"x=%d y=%d uiOn=%d workspace=%d popup=%d preview=%d(%.0f,%.0f,%.0f,%.0f) -> %s",
			x, y, in.uiModeOn ? 1 : 0, in.workspace ? 1 : 0, in.popupOpen ? 1 : 0,
			in.preview.valid ? 1 : 0, in.preview.x0, in.preview.y0, in.preview.x1, in.preview.y1,
			toUi ? "UI mode" : (toGame ? "GAME mode" : "unchanged"));
	if (toGame && PathPlaybackOwnsView()) {
		// The path is flying the camera: stay in UI mode, and let OnMouseButton swallow the press
		// (UiClick::PathPlaybackSwallowsPress). Switching to GAME here is how a click on the
		// preview mid-playback handed CS2 the mouse.
		return false;
	}
	if (toGame) {
		// The click itself carries on to the camera (CS2's look-around drag, or our free cam).
		// UI mode only drops next frame, so take the mouse here: MouseCapture_OnGameClick stands
		// down while the UI still owns the cursor.
		EnqueueCmd("mirv_filmmaker camtl cursor off");
		MouseCapture_TakeForCamera("preview click");
		return false;
	}
	if (!toUi)
		return false;
	EnqueueCmd("mirv_filmmaker camtl cursor on");
	return true;
}

bool MovieMode::OnMouseButton(int button, bool down, int x, int y) {
	// A camera path is PLAYING: a press in the workspace preview goes to nobody -- not to free-look
	// (it would fight the path) and not to CS2, whose right-click look-around and left-click player
	// step are what "the game takes the mouse during playback" was. The release of a swallowed
	// press is swallowed too, even if playback ended while the button was down, so CS2 never sees
	// half a click. The panels around the preview (pause, stop, the timeline) keep every click.
	if (button == 0 || button == 1) {
		if (!down && m_playbackSwallowed[button]) {
			m_playbackSwallowed[button] = false;
			return true;
		}
		if (down && IsDemoActive() && UiClick::PathPlaybackSwallowsPress(ClickInput(x, y), PathPlaybackOwnsView())) {
			m_playbackSwallowed[button] = true;
			if (MvmDebugLog_Active())
				MvmDebugLog_Linef("input.uiclick", "button=%d x=%d y=%d swallowed: camera path playing", button, x, y);
			return true;
		}
	}
	// Follow Camera reposition uses the same free-look + left-click placement model
	// as path-marker repositioning.
	if (button == 0 && IsDemoActive() && FollowCameraRef().Repositioning()) {
		if (down) EnqueueCmd("mirv_filmmaker follow repositionplace");
		return true;
	}
	// While repositioning a camera marker, left-click places the selected marker at
	// the current camera pose. Consume both down+up so MirvInput doesn't treat it as
	// a free-cam drag and the game doesn't switch spectator targets.
	if (button == 0 && IsDemoActive()
		&& CameraPathRef().GetMode() == CameraPath::Mode::Reposition) {
		if (down) EnqueueCmd("mirv_filmmaker marker repositionplace");
		return true;
	}
	// While the EXPERIMENTAL graph editor is open, swallow RIGHT-click: it drives the editor's
	// ease context menu through the cursor pipe (not Panorama events), so it must not reach CS2's
	// spectator switch. Left-click still passes through (the editor's panels/buttons need it; its
	// full-screen catcher absorbs strays).
	if (button == 1 && IsDemoActive() && GraphEditorExperiment_WantsCursor())
		return true;
	// LIVE Insert panel: clicks are PASSED THROUGH either way, on purpose.
	//
	// What stops a click from shooting is not this function -- it is the CCSGOInput::
	// MouseInputEnabled detour in main.cpp, which returns false while
	// LiveFxMenu_WantsGameMouseReleased() holds, so the engine stops consuming the mouse for
	// aim and Panorama gets the click. Swallowing here instead would keep the click away from
	// Panorama too, and the panel's buttons would go dead.
	//
	// (The branch this replaces gated on the since-removed TestHud panel,
	// so it never ran for the panel Insert actually opens.)
	if (ConfigHud_IsLive())
		return false;
	// While OUR free cam owns the view (camera editor, camera paths, follow camera), swallow
	// LMB/RMB so CS2's spectator doesn't yank the view to a different player. CS2's own chase
	// and free cameras are NOT gated here: there a held button is CS2's look-around drag.
	// EXCEPTIONS: when the marker edit menu is open, the camera timeline's UI-mouse mode is
	// on, OR the Customize modal is open, we must let the click reach Panorama -- those
	// surfaces draw a full-screen hit-test panel that absorbs stray clicks (so they still can't
	// switch players) while keeping their own buttons/sliders clickable. This is what makes the
	// timeline / curve editor / Customize modal usable. NOTE: consuming here (return true) makes
	// main.cpp's WndProc `return 0` BEFORE CallWindowProcW, so Panorama never sees the click --
	// that is exactly why the modal must be excepted, or every click in it is swallowed.
	if (IsDemoActive()
		&& CameraBridge_GetFreeCamEnabled()
		&& !CameraPathRef().MenuOpen() && !CameraTimeline_WantsCursor()
		&& !GraphEditorExperiment_WantsCursor() && !CameraEditor_CustomizeModalOpen()) {
		// The click is swallowed (no player switch), but it still means "I want to steer":
		// take the mouse, which the swallow would otherwise keep MouseCapture from ever seeing.
		if (down) MouseCapture_TakeForCamera("freecam click");
		return true;
	}
	// Otherwise never consume clicks: CS2 steps players or drags its camera with them, and the
	// native demo UI (demoui) needs clicks to reach Panorama.
	return false;
}

bool MovieMode::OnKey(int vkey, bool down) {
	if (vkey == kVK_CONTROL) {
		const bool changed = m_controlDown != down;
		m_controlDown = down;
		// CS2 binds Ctrl to the overview/map panel during demos. Consume both edges in
		// every director mode, while retaining the state for our own shortcuts (Ctrl+Z,
		// Ctrl+A, Ctrl+Shift+Z). Live play and menus still use their normal input path.
		const bool consumed = IsDemoActive();
		if (changed && MvmDebugLog_Active())
			MvmDebugLog_Linef("movie.input", "ctrl down=%d consumed=%d", down ? 1 : 0, consumed ? 1 : 0);
		return consumed;
	}
	// Track Shift for chord detection (Ctrl+Shift+Z = redo). Don't return -- Shift still drives
	// the free-cam slow modifier further below.
	if (vkey == kVK_SHIFT)
		m_shiftDown = down;

	// Insert belongs exclusively to the offline live-game test panel. During demos Config is
	// opened from the native demo bar, so swallow both key edges and never let an Insert bind
	// leak through to the game.
	if (vkey == kVK_INSERT) {
		if (IsDemoActive())
			return true;
		if (down && MvmTest_HandleInsertKey())
			return true;
	}

	// Config is also available in the offline mvm_test harness, so its typed fields must claim
	// keyboard input BEFORE the demo-only early return below. Otherwise digits reach the game in
	// test/live mode even though WM_CHAR is simultaneously filling Width or Height.
    // The light editor (Lighting > Edit lights) owns F and Esc while it is on.
    if (Relight::LightEditorKey(vkey, down))
        return true;
    // Placing or dragging a CS2 asset in the view: Esc drops the placement / puts the prop back.
    static bool propsEscapeHeld = false;
    if (vkey == kVK_ESC && (propsEscapeHeld || Props_EditorBusy())) {
        if (down && !propsEscapeHeld) EnqueueCmd("mirv_filmmaker props cancel");
        propsEscapeHeld = down;
        return true;
    }
    static bool environmentEscapeHeld = false;
    if (vkey == kVK_ESC && (environmentEscapeHeld || Environment::PositionEditing())) {
        if (down && !environmentEscapeHeld) EnqueueCmd("mirv_filmmaker environment edit cancel");
        environmentEscapeHeld = down;
        return true;
    }

	if (ConfigHud_TextCaptureActive())
		return true;

	// Config's Scene Objects: Delete removes the selected object, Ctrl+D duplicates it, Ctrl+C
	// copies it and Ctrl+V pastes the copy under the mouse to place. The panel's JS owns the
	// selection, so the key is handed to it. Once per press (auto-repeat would delete the next
	// selection too).
	if (ConfigHud_Enabled() && !GraphEditorExperiment_Enabled() &&
	    (vkey == kVK_DELETE || (m_controlDown && (vkey == kVK_D || vkey == kVK_C || vkey == kVK_V)))) {
		static bool editKeyHeld = false;
		if (down && !editKeyHeld)
			ConfigHud_PushKey(vkey == kVK_DELETE ? "delete" : vkey == kVK_D ? "duplicate" : vkey == kVK_C ? "copy" : "paste");
		editKeyHeld = down;
		return true;
	}
	// Ctrl+Z in Config takes back the last placement, paste, duplicate, paint stroke or removal
	// of a scene object (Props.cpp). Once per press, like the keys above.
	if (ConfigHud_Enabled() && !GraphEditorExperiment_Enabled() && m_controlDown && !m_shiftDown && vkey == kVK_Z) {
		static bool undoKeyHeld = false;
		if (down && !undoKeyHeld) EnqueueCmd("mirv_filmmaker props undo");
		undoKeyHeld = down;
		return true;
	}

	// Config also opens in the offline live-test harness, outside any demo: Esc closes it there too
	// (see the demo-side Esc handling below for the full rule).
	if (vkey == kVK_ESC && !IsDemoActive()) {
		static bool liveConfigEscapeHeld = false;
		if (!down && liveConfigEscapeHeld) { liveConfigEscapeHeld = false; return true; }
		if (down && ConfigHud_Enabled()) {
			if (!liveConfigEscapeHeld) EnqueueCmd("mirv_filmmaker config close");
			liveConfigEscapeHeld = true;
			return true;
		}
	}

	if (!IsDemoActive())
		return false; // director keys only act while a demo plays

	// Customize modal open: swallow EVERY key so NOTHING reaches the game -- no demo transport, no
	// weapon/movement binds, no spectator change, no pause menu. The modal is an exclusive input
	// surface like the editor's UI-mouse mode. Typed text is delivered separately: main.cpp forwards
	// WM_CHAR to the modal JS (CameraEditor_CustomizePushChar), because CS2 does not give an in-game
	// HUD Panorama TextEntry real keyboard focus -- so we drive the search boxes ourselves rather
	// than hoping the keystroke reaches Panorama. Consumed with NO game action here.
	if (CameraEditor_CustomizeModalOpen())
		return true;

	// A focused Render-workspace number field owns the keyboard for the same reason: the digits
	// are delivered as WM_CHAR to the JS, so letting the same physical key ALSO run a director
	// binding would (for example) pause the demo every time the user typed a "6" into an fps box.
	if (CameraEditor_TextCaptureActive())
		return true;
	// A running render owns the keyboard: any key reaching CS2 can draw into the capture (Shift+F2
	// opened the native demo bar over a render, user report 2026-10-13; Tab, binds, ...). The job's
	// own Cancel button is the way out.
	if (Render_IsJobActive())
		return true;
	// Swallow OS keyboard AUTO-REPEAT for Space: held Space streams WM_KEYDOWN (down=true)
	// with no intervening up, and every camera-path transport branch below toggles on down,
	// so a held key would start then immediately pause/restart. Act once per physical press;
	// repeats are consumed (return true) so the game never sees them.
	if (vkey == kVK_SPACE) {
		if (down && m_spaceDown) return true;
		m_spaceDown = down;
	}

	// TWO DIFFERENT QUESTIONS. `sceneKeys` = which INSPECTOR tab is open. `graphScene` = which
	// model the GRAPH DOCK is showing, which is now its own setting -- so the keys that act on the
	// dock (Ctrl+Z / Ctrl+A / Delete / Space) follow the dock, not the column beside it.
	const bool sceneKeys = FilmmakerSettings_UiString("editor.inspector", "path") == "scene";
	const bool graphScene = GraphEditorExperiment_Enabled()
		&& FilmmakerSettings_UiString("editor.graph", "camera") == "scene";
	if (vkey == kVK_Z && down && m_controlDown) {
		// Ctrl+Z = undo, Ctrl+Shift+Z = redo. Route to whichever curve surface is open: the
		// experimental graph editor owns it while enabled, otherwise the camera path/timeline.
		const bool redo = m_shiftDown;
		if (graphScene || (sceneKeys && !GraphEditorExperiment_Enabled())) {
			EnqueueCmd(redo ? "mirv_filmmaker scenekeys redo" : "mirv_filmmaker scenekeys undo"); return true;
		}
		if (GraphEditorExperiment_Enabled()) {
			EnqueueCmd(redo ? "mirv_filmmaker grapheditor redo" : "mirv_filmmaker grapheditor undo");
			return true;
		}
		if (CameraTimeline_Visible()) {
			EnqueueCmd(redo ? "mirv_filmmaker camtl redo" : "mirv_filmmaker camtl undo");
			return true;
		}
	}

	if (vkey == kVK_A && down && m_controlDown && GraphEditorExperiment_Enabled()) {
		// Ctrl+A selects every keyframe in the graph editor.
		EnqueueCmd(graphScene ? "mirv_filmmaker scenekeys graph selall" : "mirv_filmmaker grapheditor selall");
		return true;
	}

	if ((vkey == kVK_DELETE || vkey == kVK_BACKSPACE) && GraphEditorExperiment_Enabled()) {
		if (down) EnqueueCmd(graphScene ? "mirv_filmmaker scenekeys delsel" : "mirv_filmmaker grapheditor delsel");
		return true;
	}

	// T with Config open fires one weapon recoil test shot -- the keyboard twin of the WEAPON
	// RECOIL card's Test Shot button, so tuning is tap-and-watch without reaching for the mouse.
	// Once per physical press (auto-repeat swallowed), and never while a Config field is typing
	// (ConfigHud_TextCaptureActive returned above).
	if (vkey == kVK_T && ConfigHud_Enabled()) {
		static bool tHeld = false;
		if (down && !tHeld) EnqueueCmd("mirv_filmmaker weaponrecoil test");
		tHeld = down;
		return true;
	}


	using CPMode = CameraPath::Mode;
	const CPMode cpMode = CameraPathRef().GetMode();

	// J is the director's camera-path arm key. The path UI documents the two-step flow:
	// J jumps to the first key and arms a stable start frame, then Space starts the dolly. If the
	// path is already armed, J is a convenient second press to start it; while playing, J stops it.
	// This branch used to be missing entirely, so the visible J shortcut was a no-op and the FBM
	// state stayed `playing=0` after every press.
	if (vkey == kVK_J) {
		if (down) {
			if (cpMode == CPMode::PreviewPlaying)
				EnqueueCmd("mirv_filmmaker marker previewstop");
			else if (cpMode == CPMode::PreviewArmed)
				EnqueueCmd("mirv_filmmaker marker play");
			else
				EnqueueCmd("mirv_filmmaker marker preview");
		}
		return true;
	}

	// --- camera-path sub-modes take priority over the normal director keys ---
	if (FollowCameraRef().Repositioning()) {
		// Esc only. X is free-cam roll and the xray toggle, and repositioning is a free-look
		// mode -- binding cancel to it meant rolling the camera aborted the reposition.
		if (vkey == kVK_ESC) {
			if (down) EnqueueCmd("mirv_filmmaker follow repositioncancel");
			return true;
		}
		if (vkey == kVK_LEFT || vkey == kVK_RIGHT) return true;
	}
	if (cpMode == CPMode::Reposition) {
		// Free-look to position; left-click places (OnMouseButton); Esc cancels.
		if (vkey == kVK_ESC) {
			if (down) EnqueueCmd("mirv_filmmaker marker repositioncancel");
			return true;
		}
		if (vkey == kVK_LEFT || vkey == kVK_RIGHT) return true; // don't scrub the demo
	} else if (cpMode == CPMode::PreviewArmed) {
		// Armed camera-path preview: Space starts the dolly, Esc cancels.
		if (vkey == kVK_SPACE) { if (down) EnqueueCmd("mirv_filmmaker marker play"); return true; }
		if (vkey == kVK_ESC) { if (down) EnqueueCmd("mirv_filmmaker marker previewstop"); return true; }
		if (vkey == kVK_LEFT || vkey == kVK_RIGHT) return true;
	} else if (cpMode == CPMode::PreviewPlaying) {
		// Keep pause/stop routed through CameraPath so demo playback and pose pushing stay in sync.
		if (vkey == kVK_SPACE) { if (down) EnqueueCmd("mirv_filmmaker camtl pause"); return true; }
		if (vkey == kVK_ESC) { if (down) EnqueueCmd("mirv_filmmaker marker previewstop"); return true; }
		if (vkey == kVK_LEFT || vkey == kVK_RIGHT) return true;
	}

	// ESC closes the marker edit menu (and is consumed so CS2's pause menu doesn't
	// pop). Only acts while the menu is open; otherwise ESC passes through normally so
	// CS2's pause menu opens as usual -- the camera editor/timeline stays visible behind
	// it (the HUD no longer tears down while paused), which is the intended behaviour.
	if (vkey == kVK_ESC) {
		if (CameraPathRef().MenuOpen()) {
			if (down) EnqueueCmd("mirv_filmmaker marker close");
			return true;
		}
		// Esc leaves the movie-making UI (Config, Camera Editor, Render) for the plain game view and
		// is consumed, so CS2's pause menu does not open on the same press; the next Esc reaches CS2
		// and opens it as usual (user request 2026-10-13). Both edges are held so the release of a
		// consumed press never reaches the game either. A running render keeps its workspace: the
		// job's own Cancel is the way out, so a stray Esc cannot tear the UI down mid-capture.
		static bool uiEscapeHeld = false;
		if (!down && uiEscapeHeld) {
			uiEscapeHeld = false;
			return true;
		}
		if (down && (Render_IsModalOpen() || CameraEditor_Enabled() || ConfigHud_Enabled())) {
			if (!uiEscapeHeld && !Render_IsJobActive()) {
				if (Render_IsModalOpen()) EnqueueCmd("mirv_filmmaker render close");
				if (CameraEditor_Enabled()) EnqueueCmd("mirv_filmmaker editor close");
				if (ConfigHud_Enabled()) EnqueueCmd("mirv_filmmaker config close");
			}
			uiEscapeHeld = true;
			return true;
		}
		return false;
	}

	if (vkey == kVK_F8) {
		// CLEAR THE WHOLE SCREEN, not just the CONTROLS card. F8 used to toggle only the floating
		// director panel, which is a small part of what is actually drawn while directing -- the
		// timeline, the editor workspace, the Config sidebar, the native gameplay HUD (crosshair,
		// radar, health/ammo, spectator strip) and the native demo bar all stayed up. For framing
		// or recording a shot the useful action is "hide everything", so that is what it does.
		// Nothing is closed: see HudBlackout.h, it is a render-time flag, so a second press puts
		// the screen back exactly as it was.
		// A director panel that was switched off (outside a demo, or `mirv_filmmaker hud off`) comes
		// back on the next F8 instead: blackout alone never re-showed it, so it stayed gone for good.
		if (down) {
			if (!IsDemoActive()) MovieHudRef().Toggle(); // outside a demo there is no game HUD to clear
			else if (!MovieHud_Visible()) { MovieHud_Set(true); HudBlackout_Set(false); }
			else HudBlackout_Toggle();
		}
		return true;
	}

	// Space drives camera-path play ONLY when a path editor overlay is actually open (Camera
	// timeline or Graph). In the editor's Regular Timeline (native bottom) mode neither is open,
	// so Space must fall through to plain demo pause/resume below -- not trigger a path playtest
	// (which would just warn "need 2 camera markers"). Matches the gate used elsewhere here.
	// ...and NOT while the dock is showing scene strips. Scene keys are not a camera path: a
	// playtest seeks to the path's first marker and loops its range, so trying to watch a lighting
	// animation play while camera markers happened to be down just replayed the dolly over and over.
	// There, Space is plain demo play/pause, which is what "play the scene graph" means.
	if (vkey == kVK_SPACE && !graphScene && (CameraTimeline_Visible() || GraphEditorExperiment_Enabled()) && cpMode == CPMode::Editing) {
		// Skip re-issuing playtest while a start is PENDING (mode stays Editing during the
		// seek settle) -- a held / repeated Space would otherwise restart the seek. Swallow
		// the key regardless so it never leaks to the demo_pause fallback below.
		if (down && !CameraPathRef().PlaybackPending())
			EnqueueCmd("mirv_filmmaker camtl playtest");
		return true;
	}

	if (vkey == kVK_SPACE) {
		// Toggle demo pause. Read the LIVE pause state (not a cached bool) so we
		// stay in sync when the native demo bar / console also pauses.
		if (down) {
			bool paused = false;
			if (g_pEngineToClient) {
				if (auto pDemo = g_pEngineToClient->GetDemoFile())
					paused = pDemo->IsDemoPaused();
			}
			EnqueueCmd(paused ? "demo_resume" : "demo_pause");
		}
		return true; // consume up+down so the game never sees Space (no jump)
	}

	if (vkey == kVK_LEFT || vkey == kVK_RIGHT) {
		if (down) SkipSeconds(vkey == kVK_RIGHT ? +1 : -1);
		return true;
	}

	if (vkey == kVK_SHIFT) {
		// Slow/fine movement while held — only meaningful in free cam (our
		// camera). Always release-restore so sensitivity can't get stuck low.
		// Don't consume: Shift may be bound elsewhere and MirvInput ignores it.
		if (down) {
			if (CameraBridge_GetFreeCamEnabled())
				CameraBridge_SetFreeCamSlow(true);
		} else {
			CameraBridge_SetFreeCamSlow(false);
		}
		return false;
	}

	// Held X / Z roll CS2's own free cam (ApplyNativeCameraControls), like they roll ours. A release
	// always clears, so a camera change mid-hold cannot leave the roll running.
	if ((vkey == kVK_X || vkey == kVK_Z) && !CameraBridge_GetFreeCamEnabled()) {
		std::lock_guard<std::mutex> lk(m_mutex);
		bool& held = vkey == kVK_X ? m_rollRight : m_rollLeft;
		if (!down && held) { held = false; return true; }
		if (down && !m_controlDown && m_mode == Mode::FreeCam) { held = true; return true; }
	}

	if (vkey == kVK_X) {
		// In free cam, X is already roll (MirvInput) -> don't steal it.
		if (CameraBridge_GetFreeCamEnabled())
			return false;
		if (down) {
			bool nowOn;
			{
				std::lock_guard<std::mutex> lk(m_mutex);
				m_xray = !m_xray;
				nowOn = m_xray;
			}
			EnqueueCmd(nowOn ? "spec_show_xray 1" : "spec_show_xray 0");
		}
		return true;
	}

	// --- camera-marker / dolly path keys (normal editing mode only) ---
	// All marker mutation runs on the main thread via the mirv_filmmaker console
	// dispatch (FlushActions executes the queued command there). We only enqueue
	// here and read the atomic "aimed-at" marker index for L/F targeting.
	if (cpMode == CPMode::Editing) {
		if (vkey == kVK_K) {
			// Place a marker at the current camera pose (any camera mode).
			if (down) EnqueueCmd("mirv_filmmaker marker place");
			return true;
		}

		if (vkey == kVK_L) {
			// Delete the marker the camera is aimed at (consume only when targeting).
			int h = CameraPathRef().HoveredAtomic();
			if (h >= 0) {
				if (down) EnqueueCmd(std::string("mirv_filmmaker marker delete ") + std::to_string(h));
				return true;
			}
			return false;
		}

		if (vkey == kVK_F) {
			// IN THE CAMERA EDITOR, F does NOT open the standalone marker popup. That popup is a
			// second, separate editor for settings the Path Camera inspector on the right already
			// owns (ease, segment speed, curve, lens) -- opening it on top of the workspace meant
			// two UIs for one job. Here F just SELECTS the aimed-at camera, makes sure the Path
			// tab is the one showing, and hands over the UI cursor so the inspector is
			// immediately clickable. F again (or Escape) gives the mouse back to the camera.
			// Config owns the screen: F must do nothing here. It used to fall through to the
			// marker popup, whose "edit" path switches the workspace on -- so pressing F while
			// in Config threw you into the Cam Editor.
			if (ConfigHud_Enabled()) return true;
			if (CameraEditor_Enabled()) {
				if (!down) return true;
				if (CameraTimeline_WantsCursor()) {
					EnqueueCmd("mirv_filmmaker camtl cursor off");
					return true;
				}
				int hovered = CameraPathRef().HoveredAtomic();
				if (hovered >= 0) {
					EnqueueCmd(std::string("mirv_filmmaker camtl select ") + std::to_string(hovered));
					EnqueueCmd("mirv_filmmaker settings ui editor.inspector path");
					EnqueueCmd("mirv_filmmaker editor flashinspector");
				}
				EnqueueCmd("mirv_filmmaker camtl cursor on");
				return true;
			}
			// Outside the editor the popup remains the only way to edit a marker, so keep it.
			// F toggles the marker settings menu: close it if open, else open it for
			// the aimed-at marker. Otherwise defer so MirvInput keeps F = free-cam down.
			if (CameraPathRef().MenuOpen()) {
				if (down) EnqueueCmd("mirv_filmmaker marker close");
				return true;
			}
			int h = CameraPathRef().HoveredAtomic();
			if (h >= 0) {
				if (down) EnqueueCmd(std::string("mirv_filmmaker marker edit ") + std::to_string(h));
				return true;
			}
			return false;
		}
	}

	return false;
}

// Hand the view to one of CS2's demo cameras. Our own free cam (camera paths, the editor, the
// follow camera) lets go first, so the camera CS2 switches to is the one on screen.
void MovieMode::ApplyMode(Mode m) {
	CameraBridge_SetFreeCamEnabled(false);
	m_roll = 0.f;
	if (g_pEngineToClient)
		g_pEngineToClient->ExecuteClientCmd(0, SpecModeCommand(m), true);
}

namespace {
	std::atomic<float> g_nativeRoll{ 0.f }; // render thread reads it (MovieMode_NativeCameraRoll)
	std::atomic<float> g_nativeFov{ 0.f };  // Ctrl+wheel FOV in CS2's cameras; 0 = off (MovieMode_NativeCameraFov)

	// A float convar's value, 0 when it cannot be read.
	double ReadFloatCvar(const char* name) {
		if (!SOURCESDK::CS2::g_pCVar) return 0.0;
		SOURCESDK::CS2::ConVarHandle h = SOURCESDK::CS2::g_pCVar->FindConVar(name, false);
		if (!h.IsValid()) return 0.0;
		const SOURCESDK::CS2::Cvar_s* cv = SOURCESDK::CS2::g_pCVar->GetCvar(h.Get());
		if (!cv) return 0.0;
		if (cv->m_eVarType == SOURCESDK::CS2::EConVarType_Float32) return cv->m_Value.m_flValue;
		if (cv->m_eVarType == SOURCESDK::CS2::EConVarType_Float64) return cv->m_Value.m_dbValue;
		return 0.0;
	}
	// CS2's demo free cam flies at `sv_noclipspeed` units/s (sv_specspeed has no effect on it,
	// measured 2026-10-15). Shown and stepped as a multiple of the 1200 default, like our free cam.
	constexpr double kNoclipSpeedDefault = 1200.0;
	double ReadNativeFreeCamSpeed() { return ReadFloatCvar("sv_noclipspeed") / kNoclipSpeedDefault; }

	// mvm_debug: how fast CS2's free cam actually moves (units/s, measured from the rendered camera)
	// next to the convars that might drive it. Twice a second.
	void LogNativeFreeCamSpeed() {
		using Clock = std::chrono::steady_clock;
		static Clock::time_point s_t;
		static double s_o[3] = {};
		static bool s_have = false;
		const Clock::time_point now = Clock::now();
		if (s_have && now - s_t < std::chrono::milliseconds(500)) return;
		double o[3], a[3], fov = 0;
		CameraBridge_GetCurrentCamera(o, a, fov);
		if (s_have) {
			const double secs = std::chrono::duration<double>(now - s_t).count();
			const double dx = o[0] - s_o[0], dy = o[1] - s_o[1], dz = o[2] - s_o[2];
			const double ups = secs > 0 ? std::sqrt(dx * dx + dy * dy + dz * dz) / secs : 0;
			if (ups > 1.0)
				MvmDebugLog_Linef("movie.freecam.speed", "moved %.0f u/s  sv_specspeed=%g sv_noclipspeed=%g sv_specaccelerate=%g sv_maxspeed=%g",
					ups, ReadFloatCvar("sv_specspeed"), ReadFloatCvar("sv_noclipspeed"),
					ReadFloatCvar("sv_specaccelerate"), ReadFloatCvar("sv_maxspeed"));
		}
		s_t = now; s_o[0] = o[0]; s_o[1] = o[1]; s_o[2] = o[2]; s_have = true;
	}
}

float MovieMode_NativeCameraRoll() { return g_nativeRoll.load(std::memory_order_relaxed); }

bool MovieMode_NativeCameraFov(float& outFov) {
	const float fov = g_nativeFov.load(std::memory_order_relaxed);
	if (fov <= 0.f) return false;
	outFov = fov;
	return true;
}

double MovieMode::NativeFreeCamSpeed() const { return ReadNativeFreeCamSpeed(); }

// Shift+wheel speed, Ctrl+wheel FOV and held X / Z roll for CS2's own cameras (main thread). Our
// free cam keeps its own handling (MirvInput); this only runs while it is off.
//   * speed: CS2's free-cam speed, `sv_noclipspeed`, stepped on the same ladder as our free cam;
//   * FOV:   a zoom for the camera it was set in, 1 degree a notch, applied in the view hook
//            (MovieMode_NativeCameraFov). Leaving that camera hands the FOV back -- to the game,
//            or to a deliberate `mirv_fov` from Config or the console;
//   * roll:  added on top of CS2's free cam in the view hook (MovieMode_NativeCameraRoll); CS2's
//            camera has no roll of its own. Cleared whenever the camera changes.
void MovieMode::ApplyNativeCameraControls(Mode current, int speedSteps, int fovSteps, int roll) {
	using Clock = std::chrono::steady_clock;
	static Clock::time_point s_last = Clock::now();
	const Clock::time_point now = Clock::now();
	const double dt = (std::min)(0.1, std::chrono::duration<double>(now - s_last).count());
	s_last = now;

	// THE WHEEL FOV BELONGS TO ONE CAMERA. It used to be written into `mirv_fov` -- the global,
	// saved shot override -- so a zoom in the chase cam followed you into first person, and into
	// the next session through the settings file, with nothing to turn it off but Config's reset.
	// Now it ends the moment its camera does: the wheel, CS2's camera button, a player step, or our
	// free cam taking the view. The roll below follows the same rule.
	const bool native = IsDemoActive() && !CameraBridge_GetFreeCamEnabled();
	if (m_wheelFovOn && (!native || current != m_wheelFovMode)) {
		m_wheelFovOn = false;
		g_nativeFov.store(0.f, std::memory_order_relaxed);
		if (MvmDebugLog_Active())
			MvmDebugLog_Linef("movie.fov", "wheel fov released: %s (camera %s, native %d)",
				native ? "camera changed" : "our camera took the view", SpecModeCommand(current), native ? 1 : 0);
	}
	if (!native) {
		g_nativeRoll.store(0.f, std::memory_order_relaxed);
		return;
	}
	if (current == Mode::FreeCam && MvmDebugLog_Active())
		LogNativeFreeCamSpeed();

	if (speedSteps != 0 && current == Mode::FreeCam) {
		double speed = ReadNativeFreeCamSpeed();
		if (speed <= 0.0) speed = 1.0;
		const int dir = speedSteps > 0 ? +1 : -1;
		for (int i = 0; i < std::abs(speedSteps); ++i) speed = NextFreeCamSpeed(speed, dir);
		char cmd[64];
		snprintf(cmd, sizeof(cmd), "sv_noclipspeed %g", speed * kNoclipSpeedDefault);
		if (g_pEngineToClient) g_pEngineToClient->ExecuteClientCmd(0, cmd, true);
		CameraBridge_NoteFreeCamAdjust(1, dir);
	}

	if (fovSteps != 0) {
		// From what is on screen: this zoom, else the shot override, else the game's own FOV.
		const float base = m_wheelFovOn ? m_wheelFov
			: (MirvFov_Enabled() ? MirvFov_Value() : CameraBridge_GetGameCameraFov());
		float fov = base - (float)(fovSteps * kFreeCamFovStepDeg); // up = zoom in
		fov = (std::max)(1.f, (std::min)(170.f, std::round(fov)));
		m_wheelFov = fov;
		m_wheelFovOn = true;
		m_wheelFovMode = current;
		g_nativeFov.store(fov, std::memory_order_relaxed);
		CameraBridge_NoteFreeCamAdjust(2, fovSteps > 0 ? +1 : -1);
		if (MvmDebugLog_Active())
			MvmDebugLog_Linef("movie.fov", "wheel %+d -> %.0f in %s (base %.1f, mirv_fov %d)",
				fovSteps, fov, SpecModeCommand(current), base, MirvFov_Enabled() ? 1 : 0);
	}

	constexpr float kRollDegPerSec = 90.f;
	if (current == Mode::FreeCam) {
		m_roll += (float)(roll * kRollDegPerSec * dt);
		if (m_roll > 180.f) m_roll -= 360.f;
		if (m_roll < -180.f) m_roll += 360.f;
	} else {
		m_roll = 0.f;
	}
	g_nativeRoll.store(m_roll, std::memory_order_relaxed);
}

bool MovieMode::OwnsWheel() {
	return IsDemoActive() && !ConfigHud_IsLive();
}

void MovieMode::FlushActions() {
	std::vector<std::string> cmds;
	bool modeDirty = false;
	Mode pending = Mode::Default;
	int cycle = 0, speedSteps = 0, fovSteps = 0, roll = 0;
	{
		std::lock_guard<std::mutex> lk(m_mutex);
		cmds.swap(m_cmdQueue);
		modeDirty = m_modeDirty;
		pending = m_pendingMode;
		cycle = m_pendingCycle;
		speedSteps = m_pendingSpeed;
		fovSteps = m_pendingFov;
		roll = (m_rollRight ? 1 : 0) - (m_rollLeft ? 1 : 0);
		m_pendingCycle = 0;
		m_pendingSpeed = 0;
		m_pendingFov = 0;
		m_modeDirty = false;
	}

	// The camera CS2 is showing right now, read every frame: CS2's own camera button and its player
	// stepping (which leaves free cam for first person) change it without us. POV demos report it
	// through the local observer; GOTV demos report observer mode 0, so there the demo bar's own
	// camera label is the source (CameraTimelineJs publishes it). A mode we just asked for takes a few
	// frames to land, so for a short while after a switch it stands in for the stale read.
	static int s_holdFrames = 0;
	const uint8_t obs = IsDemoActive() ? AfxGetLocalObserverState(nullptr) : 0;
	const int label = CameraTimeline_NativeCameraMode();
	Mode current = GetMode();
	if (obs != 0 || label >= 0) {
		const Mode live = obs != 0 ? ModeFromObserver(obs) : (Mode)label;
		if (s_holdFrames > 0 && live != current) --s_holdFrames;
		else { s_holdFrames = 0; current = live; }
	}

	if (modeDirty) {
		ApplyMode(pending);
		current = pending;
		s_holdFrames = 30;
		if (MvmDebugLog_Active())
			MvmDebugLog_Linef("movie.mode", "apply %s (observer %d, label %d)", SpecModeCommand(pending), (int)obs, label);
	} else if (cycle != 0) {
		// Exactly what CS2's own camera button does (`spec_mode` with no argument steps CS2's real
		// camera), so the step is right even when we could not read the mode. Back is two steps
		// forward round the three cameras.
		constexpr int kModeCount = (int)Mode::FreeCam + 1;
		const int forward = ((cycle % kModeCount) + kModeCount) % kModeCount;
		CameraBridge_SetFreeCamEnabled(false);
		if (g_pEngineToClient)
			for (int i = 0; i < forward; ++i)
				g_pEngineToClient->ExecuteClientCmd(0, "spec_mode", true);
		current = (Mode)(((int)current + forward) % kModeCount);
		s_holdFrames = 30;
		m_roll = 0.f;
		if (MvmDebugLog_Active())
			MvmDebugLog_Linef("movie.mode", "wheel %+d -> spec_mode x%d, expect %s (observer %d, label %d)",
				cycle, forward, SpecModeCommand(current), (int)obs, label);
	}
	{
		std::lock_guard<std::mutex> lk(m_mutex);
		if (!m_modeDirty && m_pendingCycle == 0) m_mode = current;
	}

	ApplyNativeCameraControls(current, speedSteps, fovSteps, roll);

	ApplyXrayDefault();

	if (g_pEngineToClient)
		for (const std::string& c : cmds)
			g_pEngineToClient->ExecuteClientCmd(0, c.c_str(), true);
}

// X-ray OFF is the default for every demo (main thread, once per demo session).
//
// CS2 ships spec_show_xray at 1, so a freshly loaded demo paints team-coloured through-wall
// silhouettes over the whole shot -- noise for filming, and the first thing that had to be turned
// off by hand every single time. Forcing it off once per demo makes OFF the default without taking
// the toggle away: X here, and the Camera Editor's X-Ray setting, still turn it back on and are
// never fought again for the rest of that demo.
//
// Keyed on the resolved demo PATH, not on "is a demo active": IsPlayingDemo() blips false for a
// few frames during every demo_gototick seek (see DemoEndHold.cpp), so an active/inactive edge
// would re-force X-ray off mid-scrub and stomp a deliberate X press. For the same reason an empty
// path only counts as a real close after kXrayDemoGoneFrames consecutive frames -- long enough to
// outlast a seek blip, short enough that re-opening the SAME demo re-applies the default.
void MovieMode::ApplyXrayDefault() {
	constexpr int kXrayDemoGoneFrames = 120;

	std::wstring demoPath = ResolvePlayingDemoPath(); // cached per demo object; cheap per frame

	if (demoPath.empty()) {
		// No demo (or a transient seek blip). Only re-arm once it has been gone a while.
		if (m_xrayDemoGoneFrames < kXrayDemoGoneFrames && ++m_xrayDemoGoneFrames >= kXrayDemoGoneFrames)
			m_xrayDefaultPath.clear();
		return;
	}

	m_xrayDemoGoneFrames = 0;
	if (demoPath == m_xrayDefaultPath)
		return; // already defaulted for this demo -- the user's own toggle owns it from here

	m_xrayDefaultPath = demoPath;
	{
		std::lock_guard<std::mutex> lk(m_mutex);
		m_xray = false; // keep the X toggle and the HUD readout in sync with what we just wrote
	}
	if (g_pEngineToClient)
		g_pEngineToClient->ExecuteClientCmd(0, "spec_show_xray 0", true);

	if (MvmDebugLog_Active())
		MvmDebugLog_Linef("movie.xray", "demo default -> off");
}

MovieMode::Mode MovieMode::GetMode() { std::lock_guard<std::mutex> lk(m_mutex); return m_mode; }
bool MovieMode::GetXray() { std::lock_guard<std::mutex> lk(m_mutex); return m_xray; }
void MovieMode::SetMode(Mode mode) {
	Mode prev;
	{
		std::lock_guard<std::mutex> lk(m_mutex);
		prev = m_mode;
		m_mode = mode;
		m_pendingMode = mode;
		m_pendingCycle = 0;
		m_modeDirty = true;
	}
	// Logged OUTSIDE the lock (the log takes its own mutex and flushes per line).
	if (MvmDebugLog_Active())
		MvmDebugLog_Linef("movie.mode", "set %d -> %d", (int)prev, (int)mode);
}

// The same names CS2's demo bar shows for its camera button.
const char* MovieMode::ModeName() {
	switch (GetMode()) {
	case Mode::FreeCam: return "Free cam";
	case Mode::ThirdPerson: return "Chase cam";
	default: return "First person";
	}
}

} // namespace Filmmaker
