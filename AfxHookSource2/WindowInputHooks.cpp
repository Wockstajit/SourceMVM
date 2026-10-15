#include "WindowInputHooks.h"

#include "stdafx.h"
#include "../Shared/MirvInput.h"
#include "../Shared/AfxDetours.h"
#include "Filmmaker/Filmmaker.h"
#include "Filmmaker/Debug/MvmDebugLog.h"
#include "Filmmaker/MouseCapture.h"
#include "Filmmaker/Movie/CameraRuntimeInternal.h"
#include "Filmmaker/Panorama/ConfigHud.h"
#include "Filmmaker/Window/WindowResize.h"
#include "Filmmaker/EasterEggs/EasterEggs.h"
#include "Filmmaker/EasterEggs/Osu/OsuGame.h"
#include "Filmmaker/EasterEggs/EasterEggSnake.h"
#include "UiInputGate.h"
#include "Filmmaker/Movie/CameraBridge.h"

// A camera is being steered with the mouse -- the free cam flown, or the third-person orbit -- the
// mouse is the game's, and no filmmaker surface (timeline / editor UI mode, marker menu, graph
// editor, Customize, console) wants a cursor. Both re-centre the pointer every frame to measure the
// movement, so a visible arrow sits jittering in the middle of the shot.
static bool MouseSteeredCamera() {
	return (Filmmaker::CameraBridge_GetFreeCamEnabled() || Filmmaker::CameraBridge_GetThirdPersonOrbitEnabled())
		&& !Filmmaker::MouseCapture_IsFree()
		&& !Filmmaker::LiveFxMenu_WantsGameMouseReleased()
		&& !Filmmaker::CameraTimeline_WantsCursor()
		&& !Filmmaker::MarkerMenu_WantsCursor()
		&& !Filmmaker::GraphEditorExperiment_WantsCursor()
		&& !Filmmaker::CameraEditor_CustomizeModalOpen()
		&& !Filmmaker::UiInputGate_Active();
}

AfxUiCursorProbe g_UiCursorProbe;
// A left press went to the UI as a click-to-UI-mode click (MovieInput_OnUiClick): its release
// follows it to the panel even though UI mode only switches on with the next frame.
static bool s_uiClickHeld = false;
AfxClientRectOverride g_AfxClientRectOverride;
bool g_afxWindowProcSet = false;
WNDPROC g_NextWindProc;

namespace {
bool IsResizeHitTest(LPARAM lParam) {
	switch (LOWORD(lParam)) {
	case HTLEFT: case HTRIGHT: case HTTOP: case HTBOTTOM:
	case HTTOPLEFT: case HTTOPRIGHT: case HTBOTTOMLEFT: case HTBOTTOMRIGHT:
		return true;
	default: return false;
	}
}

bool IsResizeCursor(HCURSOR cursor) {
	return cursor == LoadCursor(nullptr, IDC_SIZEWE)
		|| cursor == LoadCursor(nullptr, IDC_SIZENS)
		|| cursor == LoadCursor(nullptr, IDC_SIZENWSE)
		|| cursor == LoadCursor(nullptr, IDC_SIZENESW)
		|| cursor == LoadCursor(nullptr, IDC_SIZEALL);
}
}

// MEASURED 2026-08-01 -- do not resurrect the startup arming that used to live here.
//
// Spoofing the client rect from startup DOES reach the engine, but it only moves the VIEWPORT,
// not the scene render target: the engine then rendered a 2560x1440 viewport into a 1600x1200
// target, i.e. the visible image was cropped/zoomed with the HUD running off the right edge,
// while `render.capsize` still reported captured=1600x1200. The scene render target is sized
// from the engine's VIDEO MODE (the -w/-h launch size), which this layer cannot reach.
// See Filmmaker/Render/RenderResolution.h for the full size-ownership map.
BOOL WINAPI new_GetClientRect(
	__in HWND hWnd,
	__out LPRECT lpRect
)
{
	BOOL result = GetClientRect(hWnd, lpRect);

	// Only the GAME window is ever lied to about, and only while the render-resolution override
	// is engaged. Every other window (and the override at rest) sees the truth.
	// During STARTUP the WndProc has not captured the game HWND yet, and that is exactly when the
	// engine sizes its render targets -- so before the probe knows the window, accept any window
	// rather than miss the creation-time queries this experiment exists to influence.
	const bool windowMatches = hWnd
		&& (g_UiCursorProbe.hwnd == nullptr || hWnd == g_UiCursorProbe.hwnd);
	if (result && lpRect
		&& g_AfxClientRectOverride.active
		&& g_AfxClientRectOverride.width > 0 && g_AfxClientRectOverride.height > 0
		&& windowMatches)
	{
		lpRect->right = lpRect->left + g_AfxClientRectOverride.width;
		lpRect->bottom = lpRect->top + g_AfxClientRectOverride.height;
	}

	return result;
}

LRESULT CALLBACK new_Afx_WindowProc(
	__in HWND hwnd,
	__in UINT uMsg,
	__in WPARAM wParam,
	__in LPARAM lParam
)
{
	g_UiCursorProbe.hwnd = hwnd;

	// Free window resizing + the size/aspect readout. OBSERVES ONLY -- it never consumes a message,
	// so it cannot change what the game sees, and it must run before the switch below because the
	// resize messages arrive from inside Windows' modal resize loop (where the game's own frame
	// loop is parked and nothing else of ours runs). See Filmmaker/Window/WindowResize.h.
	Filmmaker::WindowResize_OnWindowMessage(hwnd, uMsg, (unsigned long long)wParam, (long long)lParam);
	// Easter eggs: Konami detector, Clippy's idle timer, sparkle click bursts. Observe only.
	Filmmaker::EasterEggs_OnWindowMessage(uMsg, (unsigned long long)wParam);
	// The osu easter egg owns the keyboard and mouse buttons while it is open: nothing it uses may
	// reach the demo (or the free cam) underneath.
	// Long-render Snake: arrows / W A S D / Esc are its own while the board is up.
	if (Filmmaker::Snake_OnWindowMessage(uMsg, (unsigned long long)wParam))
		return 0;
	if (Filmmaker::Osu_OnWindowMessage(uMsg, (unsigned long long)wParam, (long long)lParam))
		return 0;
	if (uMsg == WM_SETCURSOR && Filmmaker::Osu_HidesOsCursor() && LOWORD(lParam) == HTCLIENT) {
		SetCursor(nullptr); // the game draws its own cursor
		return 1;
	}

	// Track releases before the foreign-UI gate: a button can be pressed in the
	// game and released over a menu (or outside the window after Alt+Tab).
	if (uMsg == WM_LBUTTONUP) { g_UiCursorProbe.lmb = false; g_UiCursorProbe.seq++; }
	if (uMsg == WM_RBUTTONUP) { g_UiCursorProbe.rmb = false; g_UiCursorProbe.seq++; }
	if (uMsg == WM_KILLFOCUS
		|| (uMsg == WM_ACTIVATE && LOWORD(wParam) == WA_INACTIVE)
		|| (uMsg == WM_ACTIVATEAPP && !wParam)) {
		g_UiCursorProbe.lmb = g_UiCursorProbe.rmb = false;
		g_UiCursorProbe.seq++;
		Filmmaker::MouseCapture_OnActivate(false);
		g_MirvInputEx.m_MirvInput->Supply_Focus(false);
		if (Filmmaker::MvmDebugLog_Active())
			Filmmaker::MvmDebugLog_Linef("input.capture", "focus/cancel msg=%u: cleared button state", uMsg);
	}

	// While a foreign UI owns the screen (CS2's pause menu, the ReShade overlay), EVERY
	// filmmaker input tap below stands down: no wheel camera-mode cycle, no Space pause
	// toggle, no click player-switching, no director hotkeys, no MirvInput free-cam look.
	// Input passes straight through; the menu/overlay handles it itself. See UiInputGate.h.
	if (uMsg == WM_MOUSEWHEEL && Filmmaker::MvmDebugLog_Active())
		Filmmaker::MvmDebugLog_Linef("config.input", "wheel delta=%d gated=%d config=%d cursor=%d", GET_WHEEL_DELTA_WPARAM(wParam), int(Filmmaker::UiInputGate_Active()), int(Filmmaker::ConfigHud_Enabled()), int(Filmmaker::CameraTimeline_WantsCursor()));
	if (uMsg == WM_KEYDOWN)
		Filmmaker::UiInputGate_OnKeyDown((unsigned)wParam, (long long)lParam);
	switch (uMsg) {
	case WM_KEYDOWN: case WM_KEYUP: case WM_CHAR: case WM_MOUSEWHEEL:
	case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
	case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_RBUTTONDBLCLK:
	case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MBUTTONDBLCLK:
	case WM_MOUSEMOVE:
		if (Filmmaker::UiInputGate_Active())
			return CallWindowProcW(g_NextWindProc, hwnd, uMsg, wParam, lParam);
		break;
	default:
		break;
	}

	switch(uMsg)
	{
	case WM_SETCURSOR:
		// CS2's relative-input path can immediately reinstall its invisible cursor after
		// the sidebar has released the mouse. Keep the normal arrow while any filmmaker
		// cursor surface owns input; Panorama still receives the actual mouse messages.
		if (!IsResizeHitTest(lParam)
			&& (Filmmaker::CameraTimeline_WantsCursor() || Filmmaker::MouseCapture_IsFree())) {
			SetCursor(LoadCursor(nullptr, IDC_ARROW));
			return 1;
		}
		// STEERING A CAMERA WITH THE MOUSE (free cam, third-person orbit): no cursor. While
		// spectating a demo CS2 does not consume the mouse itself, so it never hides the pointer,
		// and the re-centring left an arrow parked (and jittering) in the middle of the shot.
		// Hidden only while nothing of ours wants a cursor -- G back to UI mode shows it again
		// through the branch above.
		if (!IsResizeHitTest(lParam) && MouseSteeredCamera()) {
			SetCursor(nullptr);
			return 1;
		}
		break;
	case WM_ACTIVATE:
		{
			const bool active = LOWORD(wParam) != WA_INACTIVE;
			Filmmaker::MouseCapture_OnActivate(active);
			g_MirvInputEx.m_MirvInput->Supply_Focus(active);
		}
		break;
	case WM_CHAR:
		// Same queue for the Customize search boxes and the Render workspace's typed number
		// fields: an in-game HUD Panorama TextEntry never gets real keyboard focus, so we
		// capture the character here and hand it to the JS ourselves.
		if(Filmmaker::ConfigHud_TextCaptureActive()) {
			Filmmaker::ConfigHud_PushChar((unsigned)wParam);
			return 0;
		}
		if(Filmmaker::CameraEditor_CustomizeModalOpen() || Filmmaker::CameraEditor_TextCaptureActive()) {
			Filmmaker::CameraEditor_CustomizePushChar((unsigned)wParam);
			return 0;
		}
		if(g_MirvInputEx.m_MirvInput->Supply_CharEvent(wParam, lParam))
			return 0;
		break;
	case WM_KEYDOWN:
		// Typing in CS2's console is not a director key: Space there toggled the demo (user report
		// 2026-10-13). The release still goes through below so held-key state stays balanced.
		if(!(g_pGameUIService && g_pGameUIService->Con_IsVisible()) && Filmmaker::MovieInput_OnKey((int)wParam, true))
			return 0;
		if(g_MirvInputEx.m_MirvInput->Supply_KeyEvent(MirvInput::KS_DOWN, wParam, lParam))
			return 0;
		break;
	case WM_KEYUP:
		if(Filmmaker::MovieInput_OnKey((int)wParam, false))
			return 0;
		if(g_MirvInputEx.m_MirvInput->Supply_KeyEvent(MirvInput::KS_UP,wParam, lParam))
			return 0;
		break;
	case WM_MOUSEWHEEL:
		{
			POINT pt = { (int)(short)LOWORD(lParam), (int)(short)HIWORD(lParam) };
			ScreenToClient(hwnd, &pt);
			if(Filmmaker::MovieInput_OnMouseWheel(GET_WHEEL_DELTA_WPARAM(wParam), (GetKeyState(VK_SHIFT) & 0x8000) != 0, (GetKeyState(VK_CONTROL) & 0x8000) != 0, pt.x, pt.y))
				return 0;
		}
		if (g_MirvInputEx.m_MirvInput->Supply_MouseEvent(uMsg, wParam, lParam))
			return 0;
		break;
	case WM_LBUTTONDOWN:
		g_UiCursorProbe.lmb = true; g_UiCursorProbe.x = (short)LOWORD(lParam); g_UiCursorProbe.y = (short)HIWORD(lParam); g_UiCursorProbe.seq++;
		// GAME mode, and the click is on our UI: switch to UI mode and let the
		// panel have the click (press AND release) instead of the camera grabbing the mouse.
		{
			RECT client = {};
			GetClientRect(hwnd, &client);
			if (Filmmaker::MovieInput_OnUiClick((short)LOWORD(lParam), (short)HIWORD(lParam), client.right, client.bottom)) {
				s_uiClickHeld = true;
				break;
			}
		}
		if(Filmmaker::MovieInput_OnMouseButton(0, true)) return 0;
		if (g_MirvInputEx.m_MirvInput->Supply_MouseEvent(uMsg, wParam, lParam)) return 0;
		Filmmaker::MouseCapture_OnGameClick();
		break;
	case WM_LBUTTONUP:
		g_UiCursorProbe.lmb = false; g_UiCursorProbe.x = (short)LOWORD(lParam); g_UiCursorProbe.y = (short)HIWORD(lParam); g_UiCursorProbe.seq++;
		if (s_uiClickHeld) { s_uiClickHeld = false; break; } // the release of a click-to-UI-mode press
		if(Filmmaker::MovieInput_OnMouseButton(0, false)) return 0;
		if (g_MirvInputEx.m_MirvInput->Supply_MouseEvent(uMsg, wParam, lParam)) return 0;
		break;
	case WM_RBUTTONDOWN:
		g_UiCursorProbe.rmb = true; g_UiCursorProbe.x = (short)LOWORD(lParam); g_UiCursorProbe.y = (short)HIWORD(lParam); g_UiCursorProbe.seq++;
		if(Filmmaker::MovieInput_OnMouseButton(1, true)) return 0;
		if (g_MirvInputEx.m_MirvInput->Supply_MouseEvent(uMsg, wParam, lParam)) return 0;
		Filmmaker::MouseCapture_OnGameClick();
		break;
	case WM_RBUTTONUP:
		g_UiCursorProbe.rmb = false; g_UiCursorProbe.x = (short)LOWORD(lParam); g_UiCursorProbe.y = (short)HIWORD(lParam); g_UiCursorProbe.seq++;
		if(Filmmaker::MovieInput_OnMouseButton(1, false)) return 0;
		if (g_MirvInputEx.m_MirvInput->Supply_MouseEvent(uMsg, wParam, lParam)) return 0;
		break;
	case WM_LBUTTONDBLCLK:
	case WM_MBUTTONDBLCLK:
	case WM_MBUTTONDOWN:
	case WM_MBUTTONUP:
	case WM_RBUTTONDBLCLK:
	case WM_MOUSEMOVE:
		if (uMsg == WM_MOUSEMOVE) { g_UiCursorProbe.x = (short)LOWORD(lParam); g_UiCursorProbe.y = (short)HIWORD(lParam); g_UiCursorProbe.seq++; }
		if (g_MirvInputEx.m_MirvInput->Supply_MouseEvent(uMsg, wParam, lParam))
			return 0;
		break;
	}
	return CallWindowProcW(g_NextWindProc, hwnd, uMsg, wParam, lParam);
}

LONG_PTR WINAPI new_GetWindowLongPtrW(
  __in HWND hWnd,
  __in int  nIndex
)
{
	if(nIndex == GWLP_WNDPROC)
	{
		if(g_afxWindowProcSet)
		{
			return (LONG_PTR)g_NextWindProc;
		}
	}

	return GetWindowLongPtrW(hWnd, nIndex);
}

LONG_PTR WINAPI new_SetWindowLongPtrW(
  __in HWND     hWnd,
  __in int      nIndex,
  __in LONG_PTR dwNewLong
)
{
	if(nIndex == GWLP_WNDPROC)
	{
		LONG_PTR lResult = SetWindowLongPtrW(hWnd, nIndex, (LONG_PTR)new_Afx_WindowProc);

		if(!g_afxWindowProcSet)
		{
			g_afxWindowProcSet = true;
		}
		else
		{
			lResult = (LONG_PTR)g_NextWindProc;
		}

		g_NextWindProc = (WNDPROC)dwNewLong;

		return lResult;
	}

	return SetWindowLongPtrW(hWnd, nIndex, dwNewLong);
}

BOOL WINAPI new_GetCursorPos(
	__out LPPOINT lpPoint
)
{
	BOOL result = GetCursorPos(lpPoint);

	g_MirvInputEx.m_MirvInput->Supply_GetCursorPos(lpPoint);

	return result;
}

BOOL WINAPI new_SetCursorPos(
	__in int X,
	__in int Y
)
{
	if (Filmmaker::MvmDebugLog_Active()) {
		static unsigned s_setCursorPosCount = 0;
		if ((s_setCursorPosCount++ % 15) == 0)
			Filmmaker::MvmDebugLog_Linef("input.capture",
				"SetCursorPos x=%d y=%d free=%d liveUi=%d action=%s",
				X, Y, Filmmaker::MouseCapture_IsFree() ? 1 : 0,
				Filmmaker::LiveFxMenu_WantsGameMouseReleased() ? 1 : 0,
				(Filmmaker::MouseCapture_IsFree() || Filmmaker::LiveFxMenu_WantsGameMouseReleased())
					? "swallow" : "forward");
	}

	// While HLAE explicitly owns the mouse (the live mvm_test Config sidebar or global
	// free-mouse mode), SWALLOW the engine's cursor warps. CS2's relative-input path re-centres
	// the cursor every frame; Panorama meanwhile tracks the real pointer, so the two fight and
	// the UI cursor visibly oscillates between where the mouse is and screen centre -- the
	// "two mice stuck in the middle" symptom, and why the cursor sat pinned at centre after
	// closing the panel.
	//
	// Reported as success so the caller does not retry or treat it as an input-system failure,
	// and MirvInput is NOT told about a warp that never happened (feeding it a phantom centre
	// position is what made the free cam jerk on the next real mouse delta).
	//
	// UiInputGate_Active covers the ReShade overlay / CS2 pause menu: the engine's re-centre
	// warps fighting the overlay's pointer is the "two mouses on my screen" symptom.
	if (Filmmaker::LiveFxMenu_WantsGameMouseReleased() || Filmmaker::MouseCapture_IsFree()
		|| Filmmaker::UiInputGate_Active())
		return TRUE;

	BOOL result = SetCursorPos(X, Y);
	if(result) g_MirvInputEx.m_MirvInput->Supply_SetCursorPos(X, Y);
	return result;
}

HCURSOR WINAPI new_SetCursor(__in_opt HCURSOR hCursor)
{
	// Same rule as WM_SETCURSOR: the engine re-shows its arrow while spectating; not while steering.
	if (MouseSteeredCamera() && !IsResizeCursor(hCursor)) return SetCursor(nullptr);
	const bool releaseMouse = Filmmaker::LiveFxMenu_WantsGameMouseReleased()
		|| Filmmaker::MouseCapture_IsFree();
	if (Filmmaker::MvmDebugLog_Active())
		Filmmaker::MvmDebugLog_Linef("input.capture", "SetCursor cursor=%p free=%d liveUi=%d",
			hCursor, Filmmaker::MouseCapture_IsFree() ? 1 : 0,
			Filmmaker::LiveFxMenu_WantsGameMouseReleased() ? 1 : 0);
	return SetCursor(releaseMouse && !IsResizeCursor(hCursor) ? LoadCursor(nullptr, IDC_ARROW) : hCursor);
}

int WINAPI new_ShowCursor(BOOL bShow)
{
	if (Filmmaker::MvmDebugLog_Active())
		Filmmaker::MvmDebugLog_Linef("input.capture", "ShowCursor show=%d free=%d liveUi=%d",
			bShow ? 1 : 0, Filmmaker::MouseCapture_IsFree() ? 1 : 0,
			Filmmaker::LiveFxMenu_WantsGameMouseReleased() ? 1 : 0);
	return ShowCursor(bShow);
}

HWND WINAPI new_SetCapture(__in HWND hWnd)
{
	if (Filmmaker::MvmDebugLog_Active()) {
		static unsigned s_setCaptureCount = 0;
		if ((s_setCaptureCount++ % 15) == 0)
			Filmmaker::MvmDebugLog_Linef("input.capture",
				"SetCapture hwnd=%p free=%d liveUi=%d action=%s",
				hWnd, Filmmaker::MouseCapture_IsFree() ? 1 : 0,
				Filmmaker::LiveFxMenu_WantsGameMouseReleased() ? 1 : 0,
				(Filmmaker::MouseCapture_IsFree() || Filmmaker::LiveFxMenu_WantsGameMouseReleased())
					? "swallow" : "forward");
	}

	// SDL/CS2 may try to reacquire the gameplay capture immediately after a click or
	// activation. In free-mouse mode, report success without installing that capture;
	// the main-thread release pass is still kept as a second line of defence.
	if (Filmmaker::LiveFxMenu_WantsGameMouseReleased() || Filmmaker::MouseCapture_IsFree()
		|| Filmmaker::UiInputGate_Active())
		return hWnd;
	return SetCapture(hWnd);
}

BOOL WINAPI new_ReleaseCapture()
{
	return ReleaseCapture();
}

BOOL WINAPI new_ClipCursor(__in_opt const RECT* lpRect)
{
	if (Filmmaker::MvmDebugLog_Active()) {
		static unsigned s_clipCursorCount = 0;
		if ((s_clipCursorCount++ % 15) == 0) {
			const long left = lpRect ? lpRect->left : 0;
			const long top = lpRect ? lpRect->top : 0;
			const long right = lpRect ? lpRect->right : 0;
			const long bottom = lpRect ? lpRect->bottom : 0;
			Filmmaker::MvmDebugLog_Linef("input.capture",
				"ClipCursor rect=%p bounds=%ld,%ld,%ld,%ld free=%d liveUi=%d action=%s",
				lpRect, left, top, right, bottom,
				Filmmaker::MouseCapture_IsFree() ? 1 : 0,
				Filmmaker::LiveFxMenu_WantsGameMouseReleased() ? 1 : 0,
				(Filmmaker::MouseCapture_IsFree() || Filmmaker::LiveFxMenu_WantsGameMouseReleased())
					? "swallow" : "forward");
		}
	}

	// Releasing a clip must always reach Windows, including during focus loss.
	// Swallowing nullptr preserved the old gameplay clip while the UI was free.
	if (!lpRect) return ClipCursor(nullptr);

	// Returning success prevents CS2 from re-clipping the desktop cursor between
	// main-thread release passes.
	if (Filmmaker::LiveFxMenu_WantsGameMouseReleased() || Filmmaker::MouseCapture_IsFree()
		|| Filmmaker::UiInputGate_Active())
		return TRUE;
	return ClipCursor(lpRect);
}
