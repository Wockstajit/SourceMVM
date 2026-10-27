#include "stdafx.h"

#include "SourceWindow.h"
#include "SourceEngine.h"

#include "Ui/DirectorMath.h"

#include "WrpVEngineClient.h"

#include <AfxHookSource2/Filmmaker/Debug/MvmDebugLog.h>
#include <AfxHookSource2/Filmmaker/Window/AspectRatioMath.h>
#include <AfxHookSource2/Filmmaker/Window/WindowGeometry.h>
#include <AfxHookSource2/Filmmaker/Window/WindowPrecisionGuard.h>
#include <AfxHookSource2/Filmmaker/Window/WindowSizeOverlay.h>
#include <AfxHookSource2/Filmmaker/Window/WindowSnapMath.h>
#include <Shared/AfxConsole.h>

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <string>

extern WrpVEngineClient* g_VEngineClient;

namespace SourceFilmmaker {

namespace {

using Filmmaker::AspectMatch;
using Filmmaker::PrecisionGuardState;
using Filmmaker::WindowSnapMode;

bool s_enabled = true;        // the sizing border while windowed
bool s_overlayEnabled = true; // the size / aspect card while resizing
WindowSnapMode s_snapMode = WindowSnapMode::Both;

HWND s_hwnd = nullptr;
int s_clientW = 0;
int s_clientH = 0;

// The modal move-or-resize loop. WM_ENTERSIZEMOVE starts it for a title-bar move as well as an edge
// drag; only WM_SIZING, or a WM_SIZE that changes the size, makes it a resize.
bool s_inModalLoop = false;
bool s_resizedInModalLoop = false;
int s_modalEnterW = 0;
int s_modalEnterH = 0;
// The card's note of the last WM_SIZING, which the WM_SIZE after it re-shows the card with.
char s_lastNote[64] = { 0 };

// A Shift drag owns its rect against Windows Snap during the drag, and for a moment after it
// (WindowPrecisionGuard.h; CS2's WindowResize.cpp says what each part is for).
PrecisionGuardState s_precision;
PrecisionGuardState s_precisionHold;
int s_precisionHoldFrames = 0;
int s_precisionDefends = 0;

// A size for the engine, from a finished drag, a command or a resize from outside a drag.
bool s_commitPending = false;
int s_commitW = 0;
int s_commitH = 0;
// Where the client area's top-left stays: the engine centres the window when it sets a mode, and
// the drag (or the command) left it here. Held for the commit's cooldown: after a drag the window
// already has the new size, so the engine's move can come after it reads as followed.
int s_keepPlaceFrames = 0;
POINT s_keepClient = { 0, 0 };
// The engine's own resize after a commit arrives as WM_SIZE; these mark it as ours.
int s_lastCommitW = 0;
int s_lastCommitH = 0;
int s_commitCooldownFrames = 0;
int s_verifyFramesLeft = 0;
unsigned s_commits = 0;
std::string s_lastResult = "no resize yet";

// The one-tick step of a paused demo after a resize (PumpVerify): it plays slowly until the next
// tick, then the pause and the demo's own speed go back. Measured 2026-10-27: frames of play that
// read no new tick (0.001x for 30 frames) leave the panels up; the tick they read hides them.
bool s_hudStep = false;
int s_hudStepFromTick = 0;
int s_hudStepFrames = 0;
double s_hudTimescale = 1.0;
constexpr int kHudStepMaxFrames = 600; // a tick at 0.05x is about 0.3 s

// SourceTV's spectator bar along the top keeps the old width after a mode change until the observer
// mode changes, and only a change with frames in between lays it out again (measured 2026-10-27: a
// switch there and back in one command line does not). So first person and chase step to the other
// for kSpecStepFrames; roaming and fixed cameras would lose their place, and keep the short bar.
int s_specRestoreMode = 0;
int s_specFrames = 0;
constexpr int kSpecStepFrames = 4;

bool s_applyingStyle = false; // our own frame change, which keeps the client size: not a resize
bool s_frameStarted = false;  // before the first frame the sizes are the engine's startup, not a resize
unsigned long long s_lastStyleCheckMs = 0;
int s_styleChanges = 0;

constexpr unsigned kOverlayHoldMs = 900;
constexpr int kMinClientW = 640;
constexpr int kMinClientH = 360;
constexpr int kVerifyFrames = 240;         // ~2 s at 120 fps before calling it "not followed"
constexpr int kCommitCooldownFrames = 240; // the engine's own resize lands inside this
constexpr int kPrecisionHoldFrames = 150;
constexpr unsigned long long kStyleCheckIntervalMs = 500;

// The window rect whose client area is `client`, for a window of `style` (and the window's extended
// style and DPI).
RECT WindowRectForClient(HWND hwnd, RECT client, LONG_PTR style) {
	typedef BOOL(WINAPI * AdjustForDpiFn)(LPRECT, DWORD, BOOL, DWORD, UINT);
	typedef UINT(WINAPI * DpiForWindowFn)(HWND);
	static const AdjustForDpiFn adjustForDpi =
		(AdjustForDpiFn)GetProcAddress(GetModuleHandleW(L"user32.dll"), "AdjustWindowRectExForDpi");
	static const DpiForWindowFn dpiForWindow = (DpiForWindowFn)GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow");
	const DWORD exStyle = (DWORD)GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
	if (adjustForDpi && dpiForWindow)
		adjustForDpi(&client, (DWORD)style, FALSE, exStyle, dpiForWindow(hwnd));
	else
		AdjustWindowRectEx(&client, (DWORD)style, FALSE, exStyle);
	return client;
}

// A game window with a title bar. Fullscreen and -noborder windows have none, and are left alone.
bool Windowed(HWND hwnd) { return hwnd && (GetWindowLongPtrW(hwnd, GWL_STYLE) & WS_CAPTION) == WS_CAPTION; }

bool ClientOrigin(HWND hwnd, POINT& out) {
	out = { 0, 0 };
	return hwnd && ClientToScreen(hwnd, &out);
}

// Moves and sizes the window so its client area is `width` x `height` at `origin`.
void PlaceClient(HWND hwnd, POINT origin, int width, int height) {
	const RECT client = { origin.x, origin.y, origin.x + width, origin.y + height };
	const RECT window = WindowRectForClient(hwnd, client, GetWindowLongPtrW(hwnd, GWL_STYLE));
	SetWindowPos(hwnd, nullptr, window.left, window.top, window.right - window.left, window.bottom - window.top,
		SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
}

void ShowOverlayAt(HWND hwnd, int centerX, int centerY, int width, int height, const char* note) {
	if (!s_overlayEnabled || width <= 0 || height <= 0)
		return;
	Filmmaker::WindowSizeOverlay_Show(hwnd, centerX, centerY, Filmmaker::FormatWindowSize(width, height),
		Filmmaker::FormatWindowReadout(width, height, note));
}

void ShowOverlayCentered(HWND hwnd, int width, int height, const char* note) {
	int centerX = 0, centerY = 0;
	if (Filmmaker::WindowClientCenter(hwnd, centerX, centerY))
		ShowOverlayAt(hwnd, centerX, centerY, width, height, note);
}

// The card's aspect line for CS:S's console, which shows no UTF-8: the middle dot becomes a comma and
// "almost equal" a tilde.
std::string ConsoleAspect(int width, int height) {
	std::string text = Filmmaker::FormatWindowAspect(Filmmaker::AspectRatio_Match(width, height));
	const auto swap = [&text](const std::string& from, const std::string& to) {
		for (size_t at = text.find(from); at != std::string::npos; at = text.find(from, at + to.size()))
			text.replace(at, from.size(), to);
	};
	swap(std::string(" ") + Filmmaker::kWindowMiddleDot + " ", ", ");
	swap(std::string(Filmmaker::kWindowAlmostEqual) + " ", "~");
	return text;
}

void Log(const char* what, int width, int height) {
	if (!Filmmaker::MvmDebugLog_Active())
		return;
	const AspectMatch match = Filmmaker::AspectRatio_Match(width, height);
	Filmmaker::MvmDebugLog_Linef("window.resize", "%s client=%dx%d ratio=%.4f closest=%s modal=%d resized=%d", what, width, height,
		match.valid ? match.ratio : 0.0, (match.valid && match.entry) ? match.entry->label : "?", s_inModalLoop ? 1 : 0,
		s_resizedInModalLoop ? 1 : 0);
}

// Asks for `width` x `height` (made even, at least the minimum) on the next frame, with the client
// area's top-left kept where it is now.
void QueueCommit(int width, int height) {
	width = Filmmaker::WindowSnap_MakeEven(width < kMinClientW ? kMinClientW : width);
	height = Filmmaker::WindowSnap_MakeEven(height < kMinClientH ? kMinClientH : height);
	s_commitPending = true;
	s_commitW = width;
	s_commitH = height;
	ClientOrigin(s_hwnd, s_keepClient);
}

// Adds (or takes away) the sizing border, keeping the client area exactly where and how big it is:
// the engine renders at its mode's size, and a client area a pixel off would be scaled.
void ApplyStyle(HWND hwnd, bool resizable) {
	const LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
	const LONG_PTR want = resizable ? (style | WS_THICKFRAME) : (style & ~(LONG_PTR)WS_THICKFRAME);
	if (want == style)
		return;
	int width = 0, height = 0;
	POINT origin;
	if (!Filmmaker::WindowClientSize(hwnd, width, height) || !ClientOrigin(hwnd, origin))
		return;
	s_applyingStyle = true;
	SetWindowLongPtrW(hwnd, GWL_STYLE, want);
	const RECT client = { origin.x, origin.y, origin.x + width, origin.y + height };
	const RECT window = WindowRectForClient(hwnd, client, want);
	SetWindowPos(hwnd, nullptr, window.left, window.top, window.right - window.left, window.bottom - window.top,
		SWP_FRAMECHANGED | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
	s_applyingStyle = false;
	++s_styleChanges;
	int afterW = 0, afterH = 0;
	Filmmaker::WindowClientSize(hwnd, afterW, afterH);
	if (Filmmaker::MvmDebugLog_Active())
		Filmmaker::MvmDebugLog_Linef("window.resize", "sizing border %s, client %dx%d (was %dx%d), change #%d", resizable ? "on" : "off",
			afterW, afterH, width, height, s_styleChanges);
}

// The engine may set the window's style again (a mode change does), so the border is checked on a
// slow cadence and put back, never during a drag or while a commit is in flight.
void PumpStyle() {
	if (s_verifyFramesLeft > 0)
		return;
	const unsigned long long now = GetTickCount64();
	if (now - s_lastStyleCheckMs < kStyleCheckIntervalMs)
		return;
	s_lastStyleCheckMs = now;
	const bool resizable = (GetWindowLongPtrW(s_hwnd, GWL_STYLE) & WS_THICKFRAME) != 0;
	const bool want = s_enabled && Windowed(s_hwnd);
	if (resizable != want)
		ApplyStyle(s_hwnd, want);
}

void PumpCommit() {
	if (!s_commitPending || !g_VEngineClient)
		return;
	s_commitPending = false;
	int screenW = 0, screenH = 0;
	g_VEngineClient->GetScreenSize(screenW, screenH);
	int clientW = 0, clientH = 0;
	Filmmaker::WindowClientSize(s_hwnd, clientW, clientH);
	if (screenW == s_commitW && screenH == s_commitH && clientW == screenW && clientH == screenH)
		return; // dragged back to the size the engine renders
	if (!Engine_SetWindowedModeSize(s_commitW, s_commitH)) {
		// `mat_setvideomode` would fall back to 640x480: give the window back the size the engine
		// renders, so the picture is not scaled.
		char text[192];
		std::snprintf(text, sizeof(text), "%dx%d NOT applied: the engine's video mode %s", s_commitW, s_commitH,
			Engine_VideoModeText().c_str());
		s_lastResult = text;
		advancedfx::Warning("mirv_filmmaker window: %s. The window goes back to %dx%d.\n", text, screenW, screenH);
		if (screenW > 0 && screenH > 0) {
			s_lastCommitW = screenW;
			s_lastCommitH = screenH;
			s_commitCooldownFrames = kCommitCooldownFrames;
			PlaceClient(s_hwnd, s_keepClient, screenW, screenH);
		}
		return;
	}
	s_lastCommitW = s_commitW;
	s_lastCommitH = s_commitH;
	s_commitCooldownFrames = kCommitCooldownFrames;
	s_verifyFramesLeft = kVerifyFrames;
	s_keepPlaceFrames = kCommitCooldownFrames;
	++s_commits;
	char command[64];
	std::snprintf(command, sizeof(command), "mat_setvideomode %d %d 1", s_commitW, s_commitH);
	g_VEngineClient->ClientCmd_Unrestricted(command);
	if (Filmmaker::MvmDebugLog_Active())
		Filmmaker::MvmDebugLog_Linef("window.resize", "commit %s (engine was %dx%d, client %dx%d)", command, screenW, screenH, clientW,
			clientH);
}

// A move of the engine's that the WINDOWPOSCHANGING answer missed: the client area goes back.
void PumpKeepPlace() {
	if (s_keepPlaceFrames <= 0)
		return;
	--s_keepPlaceFrames;
	POINT origin;
	int width = 0, height = 0;
	if (!ClientOrigin(s_hwnd, origin) || !Filmmaker::WindowClientSize(s_hwnd, width, height))
		return;
	if (origin.x == s_keepClient.x && origin.y == s_keepClient.y)
		return;
	PlaceClient(s_hwnd, s_keepClient, width, height);
	if (Filmmaker::MvmDebugLog_Active())
		Filmmaker::MvmDebugLog_Linef("window.resize", "moved back: client at %ld,%ld, kept at %ld,%ld", origin.x, origin.y, s_keepClient.x,
			s_keepClient.y);
}

// Did the engine follow? Its screen size and the window's client area both read the committed size.
void PumpVerify() {
	if (s_verifyFramesLeft <= 0 || !g_VEngineClient)
		return;
	int screenW = 0, screenH = 0, clientW = 0, clientH = 0;
	g_VEngineClient->GetScreenSize(screenW, screenH);
	Filmmaker::WindowClientSize(s_hwnd, clientW, clientH);
	char text[160];
	if (screenW == s_lastCommitW && screenH == s_lastCommitH && clientW == screenW && clientH == screenH) {
		s_verifyFramesLeft = 0;
		// The mode change lays the HUD out again and shows panels that are hidden until used (the
		// freeze cam's, the round's win panel, an achievement); the next tick the demo reads hides
		// them, and a paused demo reads none. So a paused demo steps one tick.
		const DemoState demo = Demo_State();
		if (demo.playing && demo.paused) {
			s_hudTimescale = demo.timescale > 0.0 ? demo.timescale : 1.0;
			s_hudStepFromTick = demo.tick;
			s_hudStepFrames = 0;
			s_hudStep = true;
			g_VEngineClient->ClientCmd_Unrestricted("demo_timescale 0.05; demo_resume");
		}
		const int mode = Players_ObserverMode();
		if (Engine_SourceTvPlayback() && (mode == Ui::DirectorMath::kInEye || mode == Ui::DirectorMath::kChase)) {
			char command[32];
			std::snprintf(command, sizeof(command), "spec_mode %d", mode == Ui::DirectorMath::kChase ? Ui::DirectorMath::kInEye : Ui::DirectorMath::kChase);
			g_VEngineClient->ClientCmd_Unrestricted(command);
			s_specRestoreMode = mode;
			s_specFrames = kSpecStepFrames;
		}
		std::snprintf(text, sizeof(text), "%dx%d, the engine renders at it", screenW, screenH);
		s_lastResult = text;
		if (Filmmaker::MvmDebugLog_Active())
			Filmmaker::MvmDebugLog_Linef("window.resize", "FOLLOWED engine=%dx%d client=%dx%d", screenW, screenH, clientW, clientH);
		return;
	}
	if (--s_verifyFramesLeft > 0)
		return;
	s_keepPlaceFrames = 0;
	std::snprintf(text, sizeof(text), "%dx%d asked, the engine renders %dx%d and the window is %dx%d", s_lastCommitW, s_lastCommitH,
		screenW, screenH, clientW, clientH);
	s_lastResult = text;
	advancedfx::Warning("mirv_filmmaker window: %s.\n", text);
	if (Filmmaker::MvmDebugLog_Active())
		Filmmaker::MvmDebugLog_Linef("window.resize", "NOT FOLLOWED %s", text);
}

void PrintUsage() {
	advancedfx::Message(
		"mirv_filmmaker window status              - window size, aspect, what the engine renders.\n"
		"mirv_filmmaker window resizable on|off    - let you drag the CS:S window border (default on, windowed only).\n"
		"mirv_filmmaker window size <w> <h>        - ask the engine for an exact size.\n"
		"mirv_filmmaker window aspect <r>          - keep the width, set the height for a ratio (2.39, 16:9, ...).\n"
		"mirv_filmmaker window ratios              - list the aspect ratios the readout can name.\n"
		"mirv_filmmaker window snap both|ratio|resolution|off - what holding SHIFT while dragging snaps to.\n"
		"mirv_filmmaker window overlay on|off|test - the size/aspect card shown while resizing.\n");
}

} // namespace

void Window_OnMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
	if (!hwnd)
		return;
	s_hwnd = hwnd;

	switch (msg) {
	case WM_ACTIVATE:
		if (LOWORD(wParam) == WA_INACTIVE)
			Filmmaker::WindowSizeOverlay_Hide();
		break;
	case WM_GETMINMAXINFO: {
		MINMAXINFO* info = (MINMAXINFO*)lParam;
		const LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
		if (!info || !(style & WS_THICKFRAME))
			break;
		const RECT smallest = WindowRectForClient(hwnd, { 0, 0, kMinClientW, kMinClientH }, style);
		info->ptMinTrackSize.x = smallest.right - smallest.left;
		info->ptMinTrackSize.y = smallest.bottom - smallest.top;
		break;
	}
	case WM_ENTERSIZEMOVE:
		s_inModalLoop = true;
		s_keepPlaceFrames = 0; // you are moving it yourself
		s_resizedInModalLoop = false;
		s_modalEnterW = 0;
		s_modalEnterH = 0;
		s_lastNote[0] = 0;
		Filmmaker::PrecisionGuard_Release(s_precision);
		s_precisionDefends = 0;
		Filmmaker::WindowClientSize(hwnd, s_modalEnterW, s_modalEnterH);
		Log("modal enter", s_modalEnterW, s_modalEnterH);
		break;
	case WM_SIZING: {
		RECT* proposed = (RECT*)lParam;
		if (!proposed)
			break;
		const Filmmaker::WindowSizingStep step = Filmmaker::WindowSizing(hwnd, wParam, *proposed, s_snapMode, kMinClientW, kMinClientH);
		if (!step.valid)
			break;
		s_clientW = step.width;
		s_clientH = step.height;
		s_resizedInModalLoop = true;
		Filmmaker::PrecisionGuard_Approve(s_precision, step.shiftHeld, (int)proposed->left, (int)proposed->top,
			(int)(proposed->right - proposed->left), (int)(proposed->bottom - proposed->top), step.width, step.height);
		ShowOverlayAt(hwnd, step.centerX, step.centerY, step.width, step.height, step.note);
		std::snprintf(s_lastNote, sizeof(s_lastNote), "%s", step.note);
		Log(step.shiftHeld ? "sizing (snap)" : "sizing", step.width, step.height);
		break;
	}
	case WM_WINDOWPOSCHANGING: {
		WINDOWPOS* pos = (WINDOWPOS*)lParam;
		if (!pos)
			break;
		// Windows Snap's one chance to take a Shift drag: answered with the rect the drag approved.
		const bool shiftHeld = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
		const bool sizeChanging = (pos->flags & SWP_NOSIZE) == 0;
		const bool moveChanging = (pos->flags & SWP_NOMOVE) == 0;
		Filmmaker::PrecisionGuardDecision guard = Filmmaker::PrecisionGuard_Evaluate(s_precision, s_inModalLoop, shiftHeld, sizeChanging,
			moveChanging, pos->x, pos->y, pos->cx, pos->cy);
		if (!guard.clampSize && s_precisionHoldFrames > 0)
			guard = Filmmaker::PrecisionGuard_Evaluate(s_precisionHold, true, true, sizeChanging, moveChanging, pos->x, pos->y, pos->cx,
				pos->cy);
		if (guard.clampSize) {
			++s_precisionDefends;
			if (Filmmaker::MvmDebugLog_Active())
				Filmmaker::MvmDebugLog_Linef("window.snapguard", "refused %dx%d at %d,%d -> kept %dx%d (defends=%d)", (int)pos->cx,
					(int)pos->cy, (int)pos->x, (int)pos->y, guard.width, guard.height, s_precisionDefends);
			pos->cx = guard.width;
			pos->cy = guard.height;
			if (guard.clampMove) {
				pos->x = guard.x;
				pos->y = guard.y;
			}
			break;
		}
		// The engine centres the window on the main monitor when it sets a mode: after a commit, the
		// client area stays where the drag left it.
		if (s_keepPlaceFrames > 0 && !s_inModalLoop && moveChanging) {
			const RECT client = { s_keepClient.x, s_keepClient.y, s_keepClient.x + 1, s_keepClient.y + 1 };
			const RECT window = WindowRectForClient(hwnd, client, GetWindowLongPtrW(hwnd, GWL_STYLE));
			pos->x = window.left;
			pos->y = window.top;
		}
		break;
	}
	case WM_SIZE: {
		if (wParam == SIZE_MINIMIZED)
			break;
		const int width = (int)(short)LOWORD((DWORD)lParam);
		const int height = (int)(short)HIWORD((DWORD)lParam);
		if (width <= 0 || height <= 0)
			break;
		const bool changed = width != s_clientW || height != s_clientH;
		s_clientW = width;
		s_clientH = height;
		if (!changed || s_applyingStyle)
			break;
		if (s_inModalLoop) {
			s_resizedInModalLoop = true;
			ShowOverlayCentered(hwnd, width, height, s_lastNote);
			Log("size (modal)", width, height);
			break;
		}
		// Windows Snap applying the edge it was dragged to after the drag ended: refused.
		if (s_precisionHoldFrames > 0 && (width != s_precisionHold.clientW || height != s_precisionHold.clientH)) {
			const int keepW = s_precisionHold.clientW, keepH = s_precisionHold.clientH;
			advancedfx::Warning("mirv_filmmaker window: Windows Snap resized the window to %dx%d; keeping the %dx%d you dragged.\n", width,
				height, keepW, keepH);
			++s_precisionDefends;
			s_precisionHoldFrames = 0;
			Filmmaker::PrecisionGuard_Release(s_precisionHold);
			QueueCommit(keepW, keepH);
			break;
		}
		const bool ours = (width == s_lastCommitW && height == s_lastCommitH) || s_commitCooldownFrames > 0;
		Log(ours ? "size (our commit)" : "size", width, height);
		if (ours || !s_frameStarted)
			break;
		// From outside a drag (Win+Arrow, another program): the engine is asked for it too, unless
		// it is the engine's own mode change (the game's video settings), which it already renders.
		int screenW = 0, screenH = 0;
		if (g_VEngineClient)
			g_VEngineClient->GetScreenSize(screenW, screenH);
		if (width == screenW && height == screenH)
			break;
		ShowOverlayCentered(hwnd, width, height, nullptr);
		Filmmaker::WindowSizeOverlay_HideAfter(kOverlayHoldMs);
		QueueCommit(width, height);
		break;
	}
	case WM_MOVING: {
		// Carries a card that is already up along with a title-bar drag; a move never shows one.
		const RECT* proposed = (const RECT*)lParam;
		if (!proposed || !Filmmaker::WindowSizeOverlay_Visible())
			break;
		Filmmaker::FrameInsets insets;
		int width = 0, height = 0;
		if (!Filmmaker::WindowFrameInsets(hwnd, insets) || !Filmmaker::WindowClientSize(hwnd, width, height))
			break;
		Filmmaker::WindowSizeOverlay_MoveTo(proposed->left + insets.left + width / 2, proposed->top + insets.top + height / 2);
		break;
	}
	case WM_MOVE: {
		if (!Filmmaker::WindowSizeOverlay_Visible())
			break;
		int centerX = 0, centerY = 0;
		if (Filmmaker::WindowClientCenter(hwnd, centerX, centerY))
			Filmmaker::WindowSizeOverlay_MoveTo(centerX, centerY);
		break;
	}
	case WM_EXITSIZEMOVE: {
		Filmmaker::WindowSizeOverlay_HideAfter(kOverlayHoldMs);
		const bool wasResize = s_resizedInModalLoop;
		s_inModalLoop = false;
		s_resizedInModalLoop = false;
		int width = 0, height = 0;
		if (Filmmaker::WindowClientSize(hwnd, width, height)) {
			s_clientW = width;
			s_clientH = height;
		}
		if (!wasResize || (width == s_modalEnterW && height == s_modalEnterH)) {
			Log("modal exit (no resize)", width, height);
			break;
		}
		// A snap that landed the finished rect somewhere the Shift drag never approved: the approved
		// size is what is committed.
		if (Filmmaker::PrecisionGuard_FinalSizeHijacked(s_precision, width, height)) {
			advancedfx::Warning("mirv_filmmaker window: Windows Snap resized the drag to %dx%d; keeping the %dx%d you dragged.\n", width,
				height, s_precision.clientW, s_precision.clientH);
			width = s_precision.clientW;
			height = s_precision.clientH;
		}
		if (s_precision.active) {
			s_precisionHold = s_precision;
			s_precisionHoldFrames = kPrecisionHoldFrames;
		}
		Filmmaker::PrecisionGuard_Release(s_precision);
		Log("modal exit", width, height);
		QueueCommit(width, height);
		break;
	}
	default:
		break;
	}
}

void Window_Frame() {
	s_frameStarted = true;
	Filmmaker::WindowSizeOverlay_Pump();
	if (!s_hwnd || s_inModalLoop)
		return;
	if (s_precisionHoldFrames > 0 && --s_precisionHoldFrames == 0)
		Filmmaker::PrecisionGuard_Release(s_precisionHold);
	if (s_commitCooldownFrames > 0)
		--s_commitCooldownFrames;
	if (s_hudStep && g_VEngineClient) {
		const DemoState demo = Demo_State();
		if (!demo.playing || demo.tick != s_hudStepFromTick || ++s_hudStepFrames >= kHudStepMaxFrames) {
			s_hudStep = false;
			char command[64];
			std::snprintf(command, sizeof(command), "demo_pause; demo_timescale %.4f", s_hudTimescale);
			g_VEngineClient->ClientCmd_Unrestricted(command);
			if (Filmmaker::MvmDebugLog_Active())
				Filmmaker::MvmDebugLog_Linef("window.resize", "paused demo stepped from tick %d to %d for the HUD (%d frames)",
					s_hudStepFromTick, demo.tick, s_hudStepFrames);
		}
	}
	if (s_specFrames > 0 && --s_specFrames == 0 && g_VEngineClient) {
		char command[32];
		std::snprintf(command, sizeof(command), "spec_mode %d", s_specRestoreMode);
		g_VEngineClient->ClientCmd_Unrestricted(command);
	}
	PumpCommit();
	PumpVerify();
	PumpKeepPlace();
	PumpStyle();
}

void Window_Shutdown() {
	s_commitPending = false;
	s_verifyFramesLeft = 0;
	s_inModalLoop = false;
	Filmmaker::WindowSizeOverlay_Shutdown();
}

void Window_PrintHealth() {
	int clientW = 0, clientH = 0, screenW = 0, screenH = 0;
	Filmmaker::WindowClientSize(s_hwnd, clientW, clientH);
	if (g_VEngineClient)
		g_VEngineClient->GetScreenSize(screenW, screenH);
	const bool resizable = s_hwnd && (GetWindowLongPtrW(s_hwnd, GWL_STYLE) & WS_THICKFRAME) != 0;
	advancedfx::Message("  window:     %s, %dx%d (%s), engine renders %dx%d; %u resizes, last %s; video mode %s\n",
		resizable ? "resizable" : (!s_enabled ? "resizing off" : (Windowed(s_hwnd) ? "not resizable yet" : "not windowed")), clientW,
		clientH, ConsoleAspect(clientW, clientH).c_str(), screenW, screenH, s_commits,
		s_lastResult.c_str(), Engine_VideoModeText().c_str());
}

void Window_Command(int argc, advancedfx::ICommandArgs* args) {
	const char* sub = argc >= 3 ? args->ArgV(2) : "status";

	if (0 == _stricmp(sub, "status")) {
		int clientW = 0, clientH = 0, screenW = 0, screenH = 0;
		Filmmaker::WindowClientSize(s_hwnd, clientW, clientH);
		if (g_VEngineClient)
			g_VEngineClient->GetScreenSize(screenW, screenH);
		const AspectMatch match = Filmmaker::AspectRatio_Match(clientW, clientH);
		advancedfx::Message("mirv_filmmaker window:\n"
			"  resizable      : %s (border changes this session %d)\n"
			"  overlay        : %s\n"
			"  shift snap     : %s\n"
			"  snaps refused  : %d (last Shift drag)\n"
			"  client size    : %dx%d (%.4f)\n"
			"  aspect         : %s\n"
			"  engine renders : %dx%d%s\n"
			"  video mode     : %s\n"
			"  last resize    : %s\n",
			s_enabled ? "on" : "off", s_styleChanges, s_overlayEnabled ? "on" : "off", Filmmaker::WindowSnapModeName(s_snapMode),
			s_precisionDefends, clientW, clientH, match.valid ? match.ratio : 0.0, ConsoleAspect(clientW, clientH).c_str(), screenW,
			screenH, (screenW == clientW && screenH == clientH) ? " (matches the window)" : " (does NOT match the window)",
			Engine_VideoModeText().c_str(), s_lastResult.c_str());
		return;
	}
	if (0 == _stricmp(sub, "resizable")) {
		bool want = !s_enabled;
		if (argc >= 4 && 0 != _stricmp(args->ArgV(3), "toggle"))
			want = 0 != atoi(args->ArgV(3)) || 0 == _stricmp(args->ArgV(3), "on");
		s_enabled = want;
		s_lastStyleCheckMs = 0; // now, not up to half a second later
		advancedfx::Message("mirv_filmmaker window: resizable %s.\n", s_enabled ? "on" : "off");
		return;
	}
	if (0 == _stricmp(sub, "size") || 0 == _stricmp(sub, "aspect")) {
		int width = 0, height = 0;
		if (0 == _stricmp(sub, "size")) {
			if (argc < 5) {
				advancedfx::Message("usage: mirv_filmmaker window size <width> <height>\n");
				return;
			}
			width = atoi(args->ArgV(3));
			height = atoi(args->ArgV(4));
			if (width < kMinClientW || height < kMinClientH) {
				advancedfx::Warning("mirv_filmmaker window: size must be at least %dx%d.\n", kMinClientW, kMinClientH);
				return;
			}
		} else {
			const double ratio = argc >= 4 ? Filmmaker::ResolveWindowRatioArgument(args->ArgV(3)) : 0.0;
			if (!(ratio > 0.0) || !Filmmaker::WindowClientSize(s_hwnd, width, height)) {
				advancedfx::Warning("mirv_filmmaker window: usage: aspect <2.39 | 16:9 | 4:3 ...> (keeps the width); see "
					"\"mirv_filmmaker window ratios\".\n");
				return;
			}
			height = Filmmaker::AspectRatio_HeightForWidth(width, ratio);
			if (height < kMinClientH) {
				height = kMinClientH;
				width = Filmmaker::AspectRatio_WidthForHeight(height, ratio);
			}
		}
		if (!Windowed(s_hwnd)) {
			advancedfx::Warning("mirv_filmmaker window: the game is not windowed; set Display Mode to Windowed in the video options.\n");
			return;
		}
		QueueCommit(width, height);
		advancedfx::Message("mirv_filmmaker window: asking the engine for %dx%d.\n", s_commitW, s_commitH);
		return;
	}
	if (0 == _stricmp(sub, "ratios")) {
		std::size_t count = 0;
		const Filmmaker::AspectRatioEntry* table = Filmmaker::AspectRatio_Table(count);
		advancedfx::Message("mirv_filmmaker window ratios (closest match is what the resize card reports):\n");
		for (std::size_t i = 0; i < count; ++i) {
			const char* family = table[i].family == Filmmaker::AspectFamily::Cinema ? "cinema"
				: (table[i].family == Filmmaker::AspectFamily::Social ? "social" : "standard");
			advancedfx::Message("  %-8s %-8s %.4f  %s\n", table[i].label, family, table[i].ratio, table[i].name);
		}
		return;
	}
	if (0 == _stricmp(sub, "snap")) {
		if (argc >= 4 && !Filmmaker::ParseWindowSnapMode(args->ArgV(3), s_snapMode)) {
			advancedfx::Warning("mirv_filmmaker window: snap takes both | ratio | resolution | off.\n");
			return;
		}
		advancedfx::Message("mirv_filmmaker window: hold SHIFT while dragging to snap -- mode %s.\n", Filmmaker::WindowSnapModeName(s_snapMode));
		return;
	}
	if (0 == _stricmp(sub, "overlay")) {
		if (argc >= 4 && 0 == _stricmp(args->ArgV(3), "test")) {
			int width = 0, height = 0;
			if (!Filmmaker::WindowClientSize(s_hwnd, width, height)) {
				advancedfx::Warning("mirv_filmmaker window: no game window yet.\n");
				return;
			}
			ShowOverlayCentered(s_hwnd, width, height, "SHIFT to snap");
			Filmmaker::WindowSizeOverlay_HideAfter(2000);
			advancedfx::Message("mirv_filmmaker window: overlay shown for 2s.\n");
			return;
		}
		s_overlayEnabled = argc >= 4 ? (0 != atoi(args->ArgV(3)) || 0 == _stricmp(args->ArgV(3), "on")) : !s_overlayEnabled;
		if (!s_overlayEnabled)
			Filmmaker::WindowSizeOverlay_Hide();
		advancedfx::Message("mirv_filmmaker window: resize overlay %s.\n", s_overlayEnabled ? "on" : "off");
		return;
	}
	PrintUsage();
}

} // namespace SourceFilmmaker
