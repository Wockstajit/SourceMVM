#include "DeadlockInput.h"
#include "DeadlockBoot.h"
#include "DeadlockCursorClip.h"
#include "DeadlockCameraRuntime.h"
#include "DeadlockMouseWheel.h"
#include "DeadlockUi.h"
#include "DeadlockEngine.h"
#include "../../Shared/MirvInput.h"
#include "../../Shared/AfxConsole.h"
#include "../Filmmaker/MouseCapture.h"
#include "../Filmmaker/Movie/CameraBridge.h"
#include "../Filmmaker/Debug/MvmDebugLog.h"
#include <windows.h>

namespace Deadlock {
namespace {
HWND window = nullptr;
bool initialized = false, focused = false;
bool workspaceOpen = false;
// The game's cursor mode as last observed. Not a mode we set -- see Input_UiMouse.
bool cursorShowing = false;
unsigned long long samples = 0;
bool keys[256] = {};
// Last frame's CameraRuntime_MirvInputSuspended(): the edge Input_RunFrame re-arms held keys on.
// See the comment at that call site for why this exists.
bool wasMirvSuspended = false;
void ResetKeys() {
    if (MirvInput* input = CameraRuntime_Input()) {
        for (int key = 0; key < 256; ++key) input->Supply_KeyEvent(MirvInput::KS_UP, key, 0);
    }
}
BOOL CALLBACK FindWindowForProcess(HWND hwnd, LPARAM) {
    DWORD pid = 0; GetWindowThreadProcessId(hwnd, &pid);
    if (pid == GetCurrentProcessId()) {
        wchar_t cls[128] = {}, title[256] = {};
        GetClassNameW(hwnd, cls, 128);
        GetWindowTextW(hwnd, title, 256);
        char line[640];
        wsprintfA(line, "input candidate: hwnd=%p visible=%d owner=%p class=%ls title=%ls",
                  hwnd, IsWindowVisible(hwnd), GetWindow(hwnd, GW_OWNER), cls, title);
        DL_BOOT(line);
        if (IsWindowVisible(hwnd) && !GetWindow(hwnd, GW_OWNER)) {
            window = hwnd;
            DL_BOOT("input: selected visible top-level process window");
            return FALSE;
        }
    }
    return TRUE;
}
}
bool Input_Focused() { return focused; }
bool Input_OwnsCamera() {
    return focused && Engine_IsPlayingDemo() && Filmmaker::CameraBridge_GetFreeCamEnabled()
        && !Ui_ConsoleVisible() && !Filmmaker::MouseCapture_IsFree() && !Ui_AnyPanelWantsCursor();
}
void Input_SetWorkspaceOpen(bool open) {
	workspaceOpen = open;
	Ui_SetPanelWantsCursor(UiPanel::CameraEditor, Input_UiMouse());
}

// ASK THE GAME TO CHANGE MODE. We do not set the mode ourselves -- see Input_UiMouse below.
//
// Deadlock toggles its own cursor on J. That key is the only way in, so a request to change mode
// is a synthesised J press, and the change arrives through the same observation as a real one. The
// alternative -- flipping a flag of our own -- is what produced a panel that said UI while the
// game was in mouse-look.
//
// The press only lands if the game has focus, which is exactly when a mode change is meaningful.
void Input_SetUiMouse(bool enabled) {
	if (!workspaceOpen || !focused) return;
	if (Input_UiMouse() == enabled) return;
	keybd_event('J', 0, 0, 0);
	keybd_event('J', 0, KEYEVENTF_KEYUP, 0);
}

// THE MODE IS THE GAME'S, AND THIS READS IT. It is not a flag we own.
//
// It used to be one, toggled by our own J handler while Deadlock's J toggled the real thing. Two
// owners of one concept, so they drifted the moment anything moved one and not the other -- a J
// press with the workspace closed (our handler was gated on that, the game's was not), or the game
// changing mode by itself. The symptom was the whole point of the flag inverted: the panel said
// UI and nothing was clickable, or said GAME and everything was.
//
// A hidden cursor cannot click a Panorama panel and a visible one can, so the OS cursor IS the
// answer to "can the UI be used", exactly and by construction. Held across focus loss because
// GetCursorInfo is meaningless when another application owns the cursor -- and the mode does not
// matter then either.
bool Input_UiMouse() { return workspaceOpen && cursorShowing; }
void Input_RunFrame() {
    if (wcsstr(GetCommandLineW(), L"-mvmNoInput")) return;
    if (!initialized) {
        EnumWindows(FindWindowForProcess, 0);
        if (!window) return;
		initialized = true;
		DL_BOOT("input: safe per-frame polling initialized; SDL window left untouched");
		// Installed here because it needs the window, which is what this block just found.
		CursorClip_Install();
		// Its OWN message-only window, not a hook on Deadlock's -- see DeadlockMouseWheel.h.
		MouseWheel_Initialize();
        focused = GetForegroundWindow() == window;
        CameraRuntime_OnFocus(focused);
    }

	const bool nowFocused = GetForegroundWindow() == window;
	if (nowFocused != focused) {
		focused = nowFocused;
		if (!focused) ResetKeys();
		CameraRuntime_OnFocus(focused);
	}

	// The game's real mode, sampled once a frame. Only while focused: with another application in
	// front, the cursor on screen is ITS cursor and says nothing about Deadlock's input mode, so
	// the last known value is held instead of being overwritten with someone else's state.
	// Applied after the mode is known and before anything reads it, so a mode change and the
	// confinement that follows from it land in the same frame.
	CursorClip_RunFrame();

	if (focused) {
		const bool showing = Input_CursorShowing();
		if (showing != cursorShowing) {
			cursorShowing = showing;
			Ui_SetPanelWantsCursor(UiPanel::CameraEditor, Input_UiMouse());
		}
	}

	// Drains the wheel listener's own message queue -- see DeadlockMouseWheel.h. Unconditional:
	// it costs nothing when nothing scrolled, and skipping it while unfocused would let notches
	// queue up and then land all at once the moment focus returns.
	MouseWheel_PumpMessages();

    if (MirvInput* input = CameraRuntime_Input()) {
        const bool mirvSuspended = CameraRuntime_MirvInputSuspended();
        // Poll movement continuously: key edges can be swallowed during UI/focus changes or
        // before freecam is enabled. Refresh sensitivity before supplying held movement keys.
        Filmmaker::CameraBridge_SetFreeCamSlow(Input_OwnsCamera()
            && (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0);
        for (int key = 0; key < 256; ++key) {
            const bool down = focused && (GetAsyncKeyState(key) & 0x8000) != 0;
            const bool movement = key == 'W' || key == 'A' || key == 'S' || key == 'D'
                || key == 'X' || key == 'Z' || key == VK_UP || key == VK_DOWN
                || key == VK_LEFT || key == VK_RIGHT || key == VK_PRIOR || key == VK_NEXT
                || key == VK_DECIMAL || key == VK_NUMPAD0 || key == VK_NUMPAD1
                || key == VK_NUMPAD2 || key == VK_NUMPAD3 || key == VK_NUMPAD4
                || key == VK_NUMPAD6 || key == VK_NUMPAD7 || key == VK_NUMPAD8 || key == VK_NUMPAD9;
            const bool changed = down != keys[key];
            keys[key] = down;
            // Ctrl belongs to FOV scrolling. The legacy Ctrl-up handler arms IgnoreNextKey,
            // swallowing the next movement/roll press until its release.
            if (key == 'J' || key == 'C' || key == VK_CONTROL) continue;
            if (changed && !movement)
                input->Supply_KeyEvent(down ? MirvInput::KS_DOWN : MirvInput::KS_UP, key, 0);
            if (changed && movement && Filmmaker::MvmDebugLog_Active())
                Filmmaker::MvmDebugLog_LinefAlways("deadlock.input.key",
                    "key=%d down=%d focused=%d freecam=%d suspended=%d forward=%.3f roll=%.3f",
                    key, down, focused, CameraRuntime_FreeCam(), mirvSuspended,
                    input->GetCamDForward(), input->GetCamDRoll());
        }
        // Each alias pair writes one velocity slot. Sending an UP for an unused numpad alias
        // after a held letter would erase that letter's velocity, so combine aliases first.
        const int moveKeys[][2] = {
            { 'W', VK_NUMPAD8 }, { 'S', VK_NUMPAD2 }, { 'A', VK_NUMPAD4 }, { 'D', VK_NUMPAD6 },
            { 'X', VK_DECIMAL }, { 'Z', VK_NUMPAD0 }, { VK_PRIOR, VK_NUMPAD7 }, { VK_NEXT, VK_NUMPAD1 },
            { VK_NUMPAD9, VK_NUMPAD9 }, { VK_NUMPAD3, VK_NUMPAD3 },
            { VK_UP, VK_UP }, { VK_DOWN, VK_DOWN }, { VK_LEFT, VK_LEFT }, { VK_RIGHT, VK_RIGHT }
        };
        for (const auto& pair : moveKeys)
            input->Supply_KeyEvent((keys[pair[0]] || keys[pair[1]]) ? MirvInput::KS_DOWN : MirvInput::KS_UP,
                pair[0], 0);
        wasMirvSuspended = mirvSuspended;

		// CTRL+SCROLL = FOV, SHIFT+SCROLL = fly speed -- CS2 parity (MovieMode.cpp's OnMouseWheel:
		// "Ctrl+scroll = FOV zoom ... Shift+scroll = move SPEED"), routed straight to the
		// CameraBridge_AdjustFreeCam{Fov,Speed} this port already had defined but nothing had ever
		// called (no wheel notch reached this file before DeadlockMouseWheel.h). ALWAYS drained,
		// even unfocused or with free cam off, so notches scrolled elsewhere never queue up and
		// land as one large jump the moment focus returns or free cam is enabled.
		const int wheelNotches = MouseWheel_ConsumeDelta();
		if (wheelNotches != 0 && Input_OwnsCamera()) {
			const bool ctrlDown = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
			const bool shiftDown = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
			if (ctrlDown)
				Filmmaker::CameraBridge_AdjustFreeCamFov(wheelNotches);
			else if (shiftDown)
                for (int n = 0; n < abs(wheelNotches); ++n)
                    Filmmaker::CameraBridge_AdjustFreeCamSpeed(wheelNotches > 0 ? +1 : -1);
            if (Filmmaker::MvmDebugLog_Active())
                Filmmaker::MvmDebugLog_LinefAlways("deadlock.input.wheel",
                    "notches=%d ctrl=%d shift=%d speed=%.3f", wheelNotches, ctrlDown, shiftDown,
                    input->GetCamSpeed());
			// Plain scroll (neither modifier): CS2 cycles First/Third/Free person here, a mode
			// Deadlock's tool has no equivalent of, so a plain notch is left for the game itself --
			// exactly what already happens, since this hook only OBSERVES wheel input and never
			// consumes/blocks the real WM_MOUSEWHEEL Deadlock's own window still receives.
		}

		POINT screen{};
		if (GetCursorPos(&screen)) {
			POINT client = screen;
			ScreenToClient(window, &client);
			++samples;
			const bool newLeft = focused && (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
			const bool newRight = focused && (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
			CameraRuntime_SetUiCursorSample(client.x, client.y, newLeft, newRight);
			input->Supply_GetCursorPos(&screen);
			input->Supply_SetCursorPos(0, 0);
		}
        input->Supply_SetCursorPos(0, 0);
        input->Supply_MouseFrameEnd();
    }
	static bool tracedFrameEnd = false;
	if (!tracedFrameEnd) { tracedFrameEnd = true; DL_BOOT("input: first frame end complete"); }
    if (Filmmaker::MvmDebugLog_Active()) {
        static unsigned frame = 0;
        if (++frame % 64 == 0) Filmmaker::MvmDebugLog_LinefAlways("deadlock.input",
            "window=%p focused=%d owned=%d samples=%llu", window, focused, Input_OwnsCamera(), samples);
    }
}
void Input_Detach() {
	ResetKeys();
    initialized = false; window = nullptr;
}
// Is the OS cursor currently on screen?
//
// This is the only honest answer to "is the game in cursor mode or mouse-look mode", and it is the
// one our own uiMouse flag has to agree with. A flag we toggle ourselves can drift from what the
// game is actually doing -- and did.
bool Input_CursorShowing() {
	CURSORINFO ci = {};
	ci.cbSize = sizeof(ci);
	if (!GetCursorInfo(&ci)) return false;
	return 0 != (ci.flags & CURSOR_SHOWING);
}

// True windowed, as opposed to borderless or exclusive fullscreen. A real window has a caption or
// a sizing frame; a borderless one is a bare popup covering the monitor. Only a true window should
// ever let the cursor leave -- on a borderless fullscreen display there is nowhere for it to go.
bool Input_IsWindowedMode() {
	if (!window) return false;
	const LONG_PTR style = GetWindowLongPtrW(window, GWL_STYLE);
	return 0 != (style & (WS_CAPTION | WS_THICKFRAME));
}

void Input_Report() {
	advancedfx::Message("  input: polling=%d focused=%d owned=%d uiMouse=%d samples=%llu\n",
		initialized, focused, Input_OwnsCamera(), Input_UiMouse(), samples);

	// Every term of GetSuspendMirvInput(), broken out: while ANY of these three reads true, WASD
	// and mouse-look are both refused inside MirvInput itself (Shared/MirvInput.cpp), which reads
	// as "free cam won't move" or "the camera won't respond at all" with nothing in Deadlock's own
	// hotkey/focus state to explain it. See CameraRuntime_MirvInputSuspended's declaration.
	advancedfx::Message(
		"  mirv:      suspended=%d (console=%d freeMouse=%d panelWantsCursor=%d)  wasSuspended=%d\n",
		CameraRuntime_MirvInputSuspended() ? 1 : 0, Ui_ConsoleVisible() ? 1 : 0,
		Filmmaker::MouseCapture_IsFree() ? 1 : 0, Ui_AnyPanelWantsCursor() ? 1 : 0,
		wasMirvSuspended ? 1 : 0);

	// The three things that have to agree, side by side. They are printed together because the bug
	// they diagnose is precisely a DISAGREEMENT: the panel says UI while the game is in mouse-look,
	// so the panels are hit-testing and nothing can reach them.
	RECT clip = {};
	const bool haveClip = 0 != GetClipCursor(&clip);
	RECT wr = {};
	if (window) GetWindowRect(window, &wr);
	const LONG_PTR style = window ? GetWindowLongPtrW(window, GWL_STYLE) : 0;
	advancedfx::Message(
		"         cursorShowing=%d (the game's real mode)  uiMouse=%d (ours)  %s\n",
		Input_CursorShowing() ? 1 : 0, Input_UiMouse() ? 1 : 0,
		(Input_CursorShowing() == Input_UiMouse()) ? "agree" : "DISAGREE");
	advancedfx::Message(
		"         windowed=%d style=0x%llx  window=%ld,%ld %ldx%ld  clip=%ld,%ld %ldx%ld%s\n",
		Input_IsWindowedMode() ? 1 : 0, (unsigned long long)style,
		wr.left, wr.top, wr.right - wr.left, wr.bottom - wr.top,
		clip.left, clip.top, clip.right - clip.left, clip.bottom - clip.top,
		haveClip ? "" : "  (GetClipCursor failed)");
}
}
