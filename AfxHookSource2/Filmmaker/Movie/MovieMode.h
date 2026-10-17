#pragma once

// MovieMode: the in-demo "camera director". It switches between CS2's own three demo cameras
// (first person, chase, free cam -- spec_mode 2 / 3 / 4) and owns the X-ray toggle, translating
// captured input (mouse wheel, LMB/RMB, X) into actions.
//
// CS2 itself handles looking around in its chase and free cameras: hold a mouse button and drag,
// let go and the cursor is yours again. So the director never captures the mouse for a camera
// and has no cursor toggle of its own; the wheel just steps CS2's camera, the same way CS2's own
// camera button on the demo bar does (`spec_mode`).
//
// Threading: input taps (On*) run on the WndProc/input thread and only ENQUEUE
// work (mode changes + console commands) under a mutex. FlushActions() runs on
// the main/UI thread (from Filmmaker::RunMainThreadFrame) and is the only place
// that touches the engine (ExecuteClientCmd, the observer mode).
//
// X-ray starts OFF in every demo: FlushActions forces spec_show_xray 0 once per demo session
// (ApplyXrayDefault), because CS2's own default of 1 paints through-wall silhouettes over every
// shot. X and the Camera Editor's X-Ray setting still turn it back on for that demo.

#include "UiClickMath.h"

#include <atomic>
#include <mutex>
#include <vector>
#include <string>

namespace Filmmaker {

class MovieMode {
public:
	// CS2's demo cameras: in-eye, chase, roaming.
	enum class Mode { Default = 0, ThirdPerson = 1, FreeCam = 2 };

	// --- input taps (WndProc / input thread). Return true if consumed. ---
	bool OnKey(int vkey, bool down);
	bool OnMouseButton(int button, bool down, int x = -1, int y = -1); // 0 = left, 1 = right; client px
	// The right button came up, from WM_RBUTTONUP or from raw input: while the game holds the
	// mouse no legacy release arrives. Ends a right-click look and gives the editor the mouse back.
	void OnRightButtonUp();
	bool OnMouseWheel(int delta, bool shiftDown, bool ctrlDown, int x = -1, int y = -1);
	// Left button down at client (x, y) inside a workspace. A click on a panel in GAME mode switches
	// to UI mode and returns true: the caller hands this press (and its release) straight to the
	// game window, skipping the camera's click handling. A click in the preview in UI mode switches
	// to GAME mode and returns false, so the click carries on to the camera. UiClickMath.h.
	bool OnUiClick(int x, int y);

	// --- main thread ---
	void FlushActions();
	// The director takes every wheel notch in a demo (camera cycle, speed, FOV, panel scroll), so
	// the raw-input wheel is kept from CS2's own binds while this is true. The live Insert panel and
	// a foreign UI (pause menu, overlay) keep theirs.
	bool OwnsWheel();
	// Once per frame, after the workspaces committed their preview: the preview rect as window
	// fractions (WorkspacePreview), for OnUiClick and the wheel.
	void SetUiClickRegions(bool previewValid, float px0, float py0, float px1, float py1);

	// --- state for the HUD (thread-safe reads) ---
	// The camera CS2 is actually showing, read back every frame, so CS2's own camera button and
	// player stepping keep it right.
	Mode GetMode();
	bool GetXray();
	const char* ModeName();
	// CS2's free-cam fly speed as a multiple of its default (`sv_noclipspeed` / 1200), 0 when it
	// cannot be read. Main thread.
	double NativeFreeCamSpeed() const;

	// Switch CS2 to that camera (and release our free cam / any path pose). Main-thread callers
	// and UI surfaces (Render camera source, Props placement, restoring after a render).
	void SetMode(Mode mode);

	// Demo skip by the shared step (5 s): direction < 0 back, > 0 forward. Any thread.
	// Used by the arrow keys and the native bar's -/+ buttons (`mirv_filmmaker skip`).
	void SkipSeconds(int direction);

private:
	void ApplyMode(Mode m);       // main thread only
	void ApplyXrayDefault();      // main thread only: forces X-ray off once per demo
	// main thread only: Shift/Ctrl+wheel and held X / Z for CS2's own cameras
	void ApplyNativeCameraControls(Mode current, int speedSteps, int fovSteps, int roll);
	void EnqueueCmd(const std::string& c);  // any thread (locks)
	UiClick::Input ClickInput(int x, int y); // any thread (locks)

	std::mutex m_mutex;
	Mode m_mode = Mode::Default;
	bool m_xray = false;
	bool m_controlDown = false; // input-thread modifier state for Ctrl+Z curve undo
	bool m_shiftDown = false;   // input-thread modifier state for Ctrl+Shift+Z curve redo
	bool m_spaceDown = false;   // input-thread Space state; swallows OS auto-repeat so transport toggles once per press
	bool m_modeDirty = false;
	Mode m_pendingMode = Mode::Default;
	int m_pendingCycle = 0;     // wheel notches not applied yet: + next camera, - previous
	int m_pendingSpeed = 0;     // Shift+wheel notches for CS2's free-cam speed, not applied yet
	int m_pendingFov = 0;       // Ctrl+wheel notches for the FOV, not applied yet
	bool m_rollLeft = false;    // Z held in CS2's free cam
	bool m_rollRight = false;   // X held in CS2's free cam
	float m_roll = 0.f;         // main thread only: roll added to CS2's free cam (degrees)
	// main thread only: the Ctrl+wheel zoom in one of CS2's cameras, and the camera it belongs to
	bool m_wheelFovOn = false;
	float m_wheelFov = 90.f;
	Mode m_wheelFovMode = Mode::Default;
	std::vector<std::string> m_cmdQueue;
	// The workspace preview (guarded by m_mutex; written by SetUiClickRegions).
	bool m_uiPreviewValid = false;
	float m_uiPreview[4] = {};
	// A press swallowed because a camera path was playing (UiClick::PathPlaybackSwallowsPress); its
	// release is swallowed too, even if playback ended in between. Input thread only.
	bool m_playbackSwallowed[2] = { false, false };
	// A right press in the ENVIRONMENT tab's preview took the mouse for looking around
	// (UiClick::RightPressLooks); its release hands the mouse back to the editor.
	std::atomic<bool> m_rightLook{ false };

	// X-ray default-off bookkeeping (main thread only -- ApplyXrayDefault):
	std::wstring m_xrayDefaultPath;  // demo we have already forced spec_show_xray 0 for
	int m_xrayDemoGoneFrames = 0;    // consecutive demo-less frames; outlasts seek blips
};

// Singleton accessor (constructed on first use).
MovieMode& MovieModeRef();

// Roll (degrees) the view hook adds on top of CS2's own free cam; 0 in every other camera. Any
// thread.
float MovieMode_NativeCameraRoll();
// The Ctrl+wheel zoom for the CS2 camera that is showing, if one is set. Lasts only while that
// camera does (MovieMode::ApplyNativeCameraControls) and is never saved. Any thread.
bool MovieMode_NativeCameraFov(float& outFov);

} // namespace Filmmaker
