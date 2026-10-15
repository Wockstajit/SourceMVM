#include "../Config/FilmmakerSettingsStore.h"
#include "../Environment/EnvironmentScene.h"
#include "../Props/Props.h"
#include "MovieMode.h"
#include "../MouseCapture.h" // MouseCapture_TakeForCamera
#include "UiClickMath.h"     // OnUiClick: is this game-mode click on our UI

#include "CameraBridge.h"
#include "CameraPath.h"
#include "FollowCamera.h"
#include "ThirdPersonCamera.h"
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

#include "../../../Deps/release/prop/AfxHookSource/SourceSdkShared.h"
#include "../../../Deps/release/prop/cs2/sdk_src/public/cdll_int.h"

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
	constexpr int kVK_G = 0x47;     // toggle UI-mouse for the camera timeline panel
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

	// CS2 OBS_MODE_* enum: FIXED=1, IN_EYE=2, CHASE=3, ROAMING=4. The user
	// confirmed `spec_mode 4` is roaming (free fly), so first person (in-eye) = 2
	// and third person (chase) = 3. Centralised so it's a one-line tweak.
	const char* const kSpecModeFirstPerson = "spec_mode 2"; // in-eye

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

	if (GetMode() == Mode::ThirdPerson && shiftDown) {
		ThirdPersonCameraState s = ThirdPersonCameraRef().State();
		ThirdPersonCameraRef().SetDistance(s.distance + (delta > 0 ? -10.0 : 10.0));
		return true;
	}

	// Plain-wheel routing depends on whether the UI mouse is active:
	//   * Camera Editor Mode: the G key flips UI<->GAME. In UI mouse (cursor on) the wheel
	//     stays a no-op so editing never "throws you back into the game"; in GAME mouse
	//     (cursor off) it falls through to the POV cycle below, so you can scroll between
	//     First/Third/Free while controlling the game.
	//   * Standalone camera timeline / experimental graph editor are always UI surfaces, so
	//     the plain wheel is swallowed there (it used to step the selected camera / keyframes).
	if (CameraEditor_Active() || ConfigHud_Enabled()) {
		if (CameraTimeline_WantsCursor()) {
			// UI mouse. The wheel must never reach the game here (it would switch the spectated
			// player behind the panel), but "no-op" left the Config panel's inspector column
			// unscrollable over EVERY control -- Panorama itself never sees the wheel in this
			// context, so its native overflow:scroll could not help either. Consume it and hand
			// the notch to the panel, which scrolls its own column (same contract as the
			// Customize modal above).
			//
			// The Camera Editor used to be left as a no-op here, which is why its inspector could
			// not be scrolled with the wheel at all while Config could -- reported as "I can't put
			// my mouse in the middle of the camera editor and scroll". Both workspaces now do the
			// same thing. Config wins the tie when both flags somehow read true; they are mutually
			// exclusive in practice (CameraEditor_Set closes Config).
			if (ConfigHud_Enabled())
				ConfigHud_PushWheel(delta > 0 ? +1 : -1);
			else
				CameraEditor_PushWheel(delta > 0 ? +1 : -1);
			return true;
		}
		// GAME mouse: fall through to the camera-mode cycle below.
	} else if (CameraTimeline_Visible() || GraphEditorExperiment_Enabled()) {
		return true;
	}

	// Plain scroll cycles the camera mode, wrapping around at the ends:
	// up   = Default -> Third -> Free -> Default ...
	// down = Default -> Free  -> Third -> Default ...
	std::lock_guard<std::mutex> lk(m_mutex);
	constexpr int kModeCount = (int)Mode::FreeCam + 1;
	int next = ((int)m_mode + (delta > 0 ? +1 : -1) + kModeCount) % kModeCount;
	m_mode = (Mode)next;
	m_pendingMode = m_mode;
	m_modeDirty = true;
	return true;
}

void MovieMode::SetUiClickRegions(bool previewValid, float px0, float py0, float px1, float py1,
	bool barValid, float bx0, float by0, float bx1, float by1) {
	std::lock_guard<std::mutex> lk(m_mutex);
	m_uiPreviewValid = previewValid;
	m_uiPreview[0] = px0; m_uiPreview[1] = py0; m_uiPreview[2] = px1; m_uiPreview[3] = py1;
	m_uiBarValid = barValid;
	m_uiBar[0] = bx0; m_uiBar[1] = by0; m_uiBar[2] = bx1; m_uiBar[3] = by1;
}

bool MovieMode::OnUiClick(int x, int y, int clientW, int clientH) {
	if (!IsDemoActive() || ConfigHud_IsLive())
		return false;
	// Surfaces that place something with the click, or own every click while they are up, are left
	// to OnMouseButton exactly as before.
	if (FollowCameraRef().Repositioning() || CameraPathRef().GetMode() == CameraPath::Mode::Reposition
		|| CameraPathRef().MenuOpen() || GraphEditorExperiment_WantsCursor() || CameraEditor_CustomizeModalOpen())
		return false;

	UiClick::Input in;
	in.uiModeOn = CameraTimeline_WantsCursor();
	in.workspace = CameraEditor_Active() || ConfigHud_Enabled();
	const Mode mode = GetMode();
	in.uiModeAvailable = in.workspace || mode == Mode::ThirdPerson || mode == Mode::FreeCam;
	in.x = (float)x; in.y = (float)y;
	{
		std::lock_guard<std::mutex> lk(m_mutex);
		if (m_uiPreviewValid)
			in.preview = UiClick::FromFractions(m_uiPreview[0], m_uiPreview[1], m_uiPreview[2], m_uiPreview[3],
				(float)clientW, (float)clientH);
		in.demoBar.valid = m_uiBarValid;
		in.demoBar.x0 = m_uiBar[0]; in.demoBar.y0 = m_uiBar[1]; in.demoBar.x1 = m_uiBar[2]; in.demoBar.y1 = m_uiBar[3];
	}
	const bool toUi = UiClick::ClickSwitchesToUiMode(in);
	if (MvmDebugLog_Active())
		MvmDebugLog_Linef("input.uiclick",
			"x=%d y=%d client=%dx%d uiOn=%d avail=%d workspace=%d preview=%d(%.0f,%.0f,%.0f,%.0f) "
			"bar=%d(%.0f,%.0f,%.0f,%.0f) -> %s",
			x, y, clientW, clientH, in.uiModeOn ? 1 : 0, in.uiModeAvailable ? 1 : 0,
			in.workspace ? 1 : 0, in.preview.valid ? 1 : 0, in.preview.x0, in.preview.y0, in.preview.x1, in.preview.y1,
			in.demoBar.valid ? 1 : 0, in.demoBar.x0, in.demoBar.y0, in.demoBar.x1, in.demoBar.y1,
			toUi ? "UI mode" : "unchanged");
	if (!toUi)
		return false;
	EnqueueCmd("mirv_filmmaker camtl cursor on");
	return true;
}

bool MovieMode::OnMouseButton(int button, bool down) {
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
	// In FREE CAM, swallow LMB/RMB so CS2's spectator doesn't yank the view to a
	// different player while the director is flying the camera. Gate on the ACTUAL free-cam
	// state (MirvInput camera control), not the tracked spectator mode: free cam is enabled
	// from many places (camera editor, camera paths, ...) that don't run through the scroll
	// mode-cycle, so GetMode() can still read First/Third while the cam is really flying --
	// and an unswallowed click there silently switches the spectated player.
	// EXCEPTIONS: when the marker edit menu is open, the camera timeline's UI-mouse mode is
	// on (G), OR the Customize modal is open, we must let the click reach Panorama -- those
	// surfaces draw a full-screen hit-test panel that absorbs stray clicks (so they still can't
	// switch players) while keeping their own buttons/sliders clickable. This is what makes the
	// timeline / curve editor / Customize modal usable. NOTE: consuming here (return true) makes
	// main.cpp's WndProc `return 0` BEFORE CallWindowProcW, so Panorama never sees the click --
	// that is exactly why the modal must be excepted, or every click in it is swallowed.
	// THIRD PERSON deliberately does NOT swallow clicks (reversed 2026-08-28 on report: "in third
	// person, orbiting somebody, you can't switch players"). The orbit is driven by mouse MOVEMENT
	// deltas (CameraBridge_ConsumeThirdPersonOrbit) and never needs a button held, so eating
	// LMB/RMB bought nothing except removing the spectator's next/prev-player bind from the one
	// mode where you are looking straight at a player and most likely to want a different one.
	// Free cam and camera-path modes still swallow: there a stray click yanks the shot to another
	// player mid-take, and the camera is not attached to anyone to begin with.
	if (IsDemoActive()
		&& (CameraBridge_GetFreeCamEnabled() || GetMode() == Mode::FreeCam)
		&& !CameraPathRef().MenuOpen() && !CameraTimeline_WantsCursor()
		&& !GraphEditorExperiment_WantsCursor() && !CameraEditor_CustomizeModalOpen()) {
		// The click is swallowed (no player switch), but it still means "I want to steer":
		// take the mouse, which the swallow would otherwise keep MouseCapture from ever seeing.
		if (down) MouseCapture_TakeForCamera("freecam click");
		return true;
	}
	// Otherwise never consume clicks: the first-person spectator switches players on
	// LMB/RMB and the native demo UI (demoui) needs clicks to reach Panorama.
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

	// G toggles the native-demo-bar UI cursor only in regular viewing mode, and only
	// from third-person/freecam. The camera timeline / curve editor is always a UI
	// surface, so G is swallowed there and cannot turn the cursor off -- EXCEPT in
	// Camera Editor Mode, where G must keep flipping UI<->GAME so the user can fly the
	// free cam to frame a shot (the editor decouples the timeline's forced cursor).
	// The Config panel gets the same priority: while it is open, G always flips UI<->GAME
	// regardless of camera mode (in first person the regular-mode branch below would force
	// the cursor OFF and the user could never get back into the panel).
	if (vkey == kVK_G) {
		// LIVE Insert panel first: it owns its own cursor flag, so G flips that directly and
		// never goes near `camtl cursor`. Routing it through the camera timeline was the bug --
		// on a live map the timeline's spectator-mode gate refuses the toggle and responds by
		// forcing the cursor OFF, so G could only take the mouse away and never give it back.
		if (ConfigHud_IsLive()) {
			if (down) ConfigHud_ToggleLiveCursor();
			return true;
		}
		if (CameraEditor_Active() || ConfigHud_Enabled()) {
			if (down) EnqueueCmd("mirv_filmmaker camtl cursor toggle");
			return true;
		}
		if (CameraTimeline_Visible())
			return true;
		const Mode mode = GetMode();
		if (mode != Mode::ThirdPerson && mode != Mode::FreeCam) {
			if (down) EnqueueCmd("mirv_filmmaker camtl cursor off");
			return true;
		}
		if (down) EnqueueCmd("mirv_filmmaker camtl cursor toggle");
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

void MovieMode::ApplyMode(Mode m, bool keepCursor) {
	switch (m) {
	case Mode::FreeCam:
		// Release the orbit camera (and its mouse capture) before handing the view to
		// HLAE freecam -- the two both ride MirvInput and must not overlap.
		ThirdPersonCameraRef().Exit();
		CameraBridge_SetFreeCamEnabled(true);
		// Free cam is steered with the mouse: take it now instead of waiting for a game click that
		// free cam swallows anyway (a click here must not switch the spectated player).
		if (!keepCursor) MouseCapture_TakeForCamera("mode freecam");
		break;
	case Mode::ThirdPerson:
		CameraBridge_SetFreeCamEnabled(false);
		ThirdPersonCameraRef().Enter();
		// The orbit follows mouse movement: without the mouse, third person cannot look around.
		if (!keepCursor) MouseCapture_TakeForCamera("mode thirdperson");
		if (g_pEngineToClient && !keepCursor) {
			// Leaving free cam: drop the UI cursor so it can't linger into a normal
			// spectator view (and stays in sync for the next time you enter free cam).
			g_pEngineToClient->ExecuteClientCmd(0, "mirv_filmmaker camtl cursor off", true);
		}
		break;
	case Mode::Default:
		CameraBridge_SetFreeCamEnabled(false);
		ThirdPersonCameraRef().Exit();
		if (g_pEngineToClient) {
			g_pEngineToClient->ExecuteClientCmd(0, kSpecModeFirstPerson, true);
			if (!keepCursor)
				g_pEngineToClient->ExecuteClientCmd(0, "mirv_filmmaker camtl cursor off", true);
		}
		break;
	}
}

void MovieMode::FlushActions() {
	std::vector<std::string> cmds;
	bool modeDirty = false;
	Mode pending = Mode::Default;
	bool keepCursor = false;
	{
		std::lock_guard<std::mutex> lk(m_mutex);
		cmds.swap(m_cmdQueue);
		modeDirty = m_modeDirty;
		pending = m_pendingMode;
		keepCursor = m_pendingKeepCursor;
		m_pendingKeepCursor = false;
		m_modeDirty = false;
	}

	if (modeDirty)
		ApplyMode(pending, keepCursor);

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
bool MovieMode::GetCursor() { std::lock_guard<std::mutex> lk(m_mutex); return m_cursor; }

void MovieMode::SetMode(Mode mode, bool keepCursor) {
	Mode prev;
	{
		std::lock_guard<std::mutex> lk(m_mutex);
		prev = m_mode;
		m_mode = mode;
		m_pendingMode = mode;
		m_pendingKeepCursor = keepCursor;
		m_modeDirty = true;
	}
	// Logged OUTSIDE the lock (the log takes its own mutex and flushes per line). keepCursor
	// is the new render-workspace path: true skips ApplyMode's `camtl cursor off`, so a
	// workspace that goes unclickable after a camera-source click shows up here as keepCursor=0.
	if (MvmDebugLog_Active())
		MvmDebugLog_Linef("movie.mode", "set %d -> %d keepCursor=%d",
			(int)prev, (int)mode, keepCursor ? 1 : 0);
}

const char* MovieMode::ModeName() {
	switch (GetMode()) {
	case Mode::FreeCam: return "Free cam";
	case Mode::ThirdPerson: return "Third person";
	default: return "First person";
	}
}

void MovieMode::SyncFromFreeCam(bool freeCamEnabled) {
	std::lock_guard<std::mutex> lk(m_mutex);

	// Don't fight an in-flight transition: a scroll may have set a new pending
	// mode that FlushActions hasn't applied yet, so the live free-cam state still
	// reflects the OLD mode. Reconciling here would clobber the user's choice and
	// cause the intermittent first/third/free mislabel. Only correct the mode
	// once the queue is drained (m_modeDirty == false), i.e. for genuine external
	// (console) free-cam toggles.
	if (m_modeDirty)
		return;

	if (freeCamEnabled) {
		m_mode = Mode::FreeCam;
	} else if (ThirdPersonCameraRef().State().active) {
		m_mode = Mode::ThirdPerson;
	} else if (m_mode == Mode::FreeCam) {
		// Free cam was turned off elsewhere; fall back to third person.
		m_mode = Mode::ThirdPerson;
	}
}

} // namespace Filmmaker
