#include "WindowResize.h"

#include "AspectRatioMath.h"
#include "EngineVideoMode.h"
#include "Sdl3Window.h"
#include "WindowGeometry.h" // client size, frame insets, the Shift snap and the card's text, shared with CS:S
#include "WindowSizeOverlay.h"
#include "WindowSnapMath.h"
#include "WindowPrecisionGuard.h" // Shift-drag vs Windows Snap: whose rect wins

#include "../Debug/MvmDebugLog.h" // MvmDebugLog_* (window.resize probe)
#include "../Render/RenderResolution.h"    // RenderResolution_SwapChainSize -- did the engine follow?

#include "../../WindowInputHooks.h"        // g_UiCursorProbe.hwnd -- the game window we hook

#include "../../../Shared/AfxConsole.h"

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>

namespace Filmmaker {

namespace {

// Feature state. Written from the game message thread (WndProc) and the game main thread; in CS2
// those are the same thread, and the render thread never touches any of it. Plain scalars, same
// convention as AfxUiCursorProbe.
bool s_enabled = true;          // make the window user-resizable
bool s_overlayEnabled = true;   // show the size / aspect card while resizing
bool s_probe = false;           // per-message logging (needs "mvm_debug start")
// SDL, not VideoMode. MEASURED 2026-08-07: the video-mode route cannot set a size on this build
// (EngineVideoMode.cpp says why) and used to snap the window back to its old size, while an
// SDL-issued resize makes the engine recreate the swapchain and rebuild every scene render target
// at the new size. See Docs/features/filmmaker_window_resize.md.
WindowResizeCommit s_commit = WindowResizeCommit::Sdl;
WindowSnapMode s_snapMode = WindowSnapMode::Both; // what holding Shift while dragging does

HWND s_hwnd = nullptr;
int s_clientW = 0;
int s_clientH = 0;

// Modal move-or-resize session state. WM_ENTERSIZEMOVE fires for BOTH moving the window by its
// title bar and resizing it by an edge, so entering the loop tells us nothing on its own. Only
// WM_SIZING (and a WM_SIZE that actually changes the size) means "the user is resizing", and the
// readout card must not appear for a plain move.
// SDL must never pump/synchronize a resize while SDL's WndProc is still dispatching it.
int s_messageDepth = 0;
bool s_mainThreadStarted = false;
bool s_sdlPending = false;
int s_sdlPendingW = 0, s_sdlPendingH = 0;
struct WindowMessageScope {
	WindowMessageScope() { ++s_messageDepth; }
	~WindowMessageScope() { --s_messageDepth; }
};
bool s_inModalLoop = false;
bool s_resizedInModalLoop = false;
int s_modalEnterW = 0;
int s_modalEnterH = 0;

// Shift (precision) drag ownership: the rect WM_SIZING approved, so a Windows Snap that tries to
// replace it mid-drag can be answered with it instead. See WindowPrecisionGuard.h.
PrecisionGuardState s_precision;
int s_precisionDefends = 0; // snaps refused during this drag -- reported by "window status"

// ...and the same rect held for a moment AFTER the drag ends. Windows applies "dragged to the top
// edge" as a MAXIMISE when the button comes up, which arrives after WM_EXITSIZEMOVE and therefore
// outside the guard's in-loop gate. Measured on this machine: the drag settled at 1440x1152 and
// the window was then maximised to 2560x1351, which the external-resize path duly committed to the
// engine -- exactly the reported bug. The hold is short on purpose: it defends the drag you just
// finished, not the window forever, so deliberately maximising a second later still works.
PrecisionGuardState s_precisionHold;
int s_precisionHoldFrames = 0;
constexpr int kPrecisionHoldFrames = 150; // ~2.5s at 60fps

// The note last written on the card ("SNAP 1080p" / "SHIFT to snap"). WM_SIZE arrives immediately
// after the WM_SIZING that produced it and re-shows the same card; re-showing it without the note
// makes the third line blink out and back on every step of a drag.
char s_lastNote[64] = { 0 };

// "Did the engine follow?" check, armed when a resize finishes.
bool s_verifyPending = false;
int s_verifyW = 0;
int s_verifyH = 0;
int s_verifyFramesLeft = 0;
bool s_verifyReasserted = false;

// A commit is in flight: the engine's own video-mode change resizes the window itself, which
// produces another WM_SIZE. Without this the new WM_SIZE would arm another commit, and the two
// would drive each other forever.
int s_commitCooldownFrames = 0;
int s_lastCommitW = 0;
int s_lastCommitH = 0;
int s_videoModeRetryFrames = 0; // waiting for the Panorama bridge to become usable

// Style bookkeeping. Checked on a slow cadence, not every frame: if something in the engine keeps
// resetting the style, a per-frame re-apply is a SetWindowPos storm, and the old "give up after N
// attempts" rule made the window permanently un-resizable a few seconds into a session -- which is
// the "after a little bit it just stops working" report. There is no give-up any more.
unsigned long long s_lastStyleCheckMs = 0;
int s_styleReapplies = 0;

// A size was asked for while the window was fullscreen. SDL cannot size a fullscreen window, so the
// engine is asked to go Windowed first (its own Display Mode setting) and the size is applied
// once SDL reports the window is no longer fullscreen.
bool s_leaveFullscreenPending = false;
int s_leaveW = 0, s_leaveH = 0;
unsigned long long s_leaveDeadlineMs = 0;
int s_leaveSettleFrames = 0;

constexpr unsigned long long kLeaveFullscreenTimeoutMs = 8000;
constexpr int kLeaveFullscreenSettleFrames = 30; // let the engine finish its own swapchain rebuild
constexpr unsigned kOverlayHoldMs = 900;
constexpr int kMinClientW = 640;
constexpr int kMinClientH = 360;
constexpr int kVerifyFrames = 120;          // ~2s at 60fps before calling it "not followed"
constexpr int kVerifyFramesAfterReassert = 90;
constexpr int kCommitCooldownFrames = 240;  // ~4s: a video-mode change is not instant
constexpr int kVideoModeRetryFrames = 180;  // ~3s waiting for a UI context before falling back
constexpr unsigned long long kStyleCheckIntervalMs = 500;

HWND GameWindow() {
	if (s_hwnd)
		return s_hwnd;
	return g_UiCursorProbe.hwnd;
}

// `note` is appended to the aspect line: the live snap state, or the hint that Shift can snap.
void ShowOverlayAt(HWND hwnd, int centerX, int centerY, int width, int height, const char* note) {
	if (!s_overlayEnabled || width <= 0 || height <= 0)
		return;
	WindowSizeOverlay_Show(hwnd, centerX, centerY, FormatWindowSize(width, height), FormatWindowReadout(width, height, note));
}

// For the paths where the window already IS the size being reported (maximise, restore, the
// engine's own resize, the console test): centre on the live client rect.
void ShowOverlayCentered(HWND hwnd, int width, int height, const char* note) {
	int centerX = 0, centerY = 0;
	if (!WindowClientCenter(hwnd, centerX, centerY))
		return;
	ShowOverlayAt(hwnd, centerX, centerY, width, height, note);
}

void LogSize(const char* what, int width, int height) {
	if (!s_probe || !MvmDebugLog_Active())
		return;
	const AspectMatch match = AspectRatio_Match(width, height);
	MvmDebugLog_Linef("window.resize", "%s client=%dx%d ratio=%.4f closest=%s err=%+.2f%% modal=%d resized=%d",
		what, width, height, match.valid ? match.ratio : 0.0,
		(match.valid && match.entry) ? match.entry->label : "?",
		match.valid ? match.errorPercent : 0.0,
		s_inModalLoop ? 1 : 0, s_resizedInModalLoop ? 1 : 0);
}

// Drive the finished client size through SDL. This is the commit: SDL owns the window, and an
// SDL-issued resize is what makes the engine recreate its swapchain and rebuild every scene render
// target at the new size.
//
// MEASURED 2026-08-07 (CS2 build 2000879), both halves of this matter:
//   * SDL_SetWindowSize to a size SDL does NOT already hold -> "Successfully created dx11 swap
//     chain" plus a full 65-target Scratch RT rebuild at the new size. The engine follows.
//   * SDL_SetWindowSize to the size it ALREADY holds -> nothing at all. No swapchain, no rebuild.
//
// The second one is the trap, because it is the normal case here: after a drag Windows has already
// applied the size and SDL updated its own record from WM_SIZE, so the obvious "re-assert the size
// we ended on" is a no-op and the engine is left rendering into stale targets -- which is what
// black patches in the frame actually are. So when SDL is already holding the target, step two
// pixels off it first to guarantee there is a change for SDL to act on, then land on the real size.
//
// The step is 2px and on the height so it stays even (odd sizes are rejected downstream) and costs
// the smallest possible aspect wobble for the frame it is visible.
//
// The nudge SYNCS as well, deliberately. Skipping the sync would be cheaper -- one engine rebuild
// instead of two -- but only if SDL compares the next request against what was last *requested*.
// If it compares against the size actually applied, an unsynced nudge changes nothing and the real
// call is a no-op all over again, which is the exact bug this function exists to avoid. Paying for
// a second rebuild once per drag-release is the right side to be wrong on.
bool ReassertThroughSdl(int width, int height) {
	if (s_messageDepth > 0 || s_inModalLoop) {
		s_sdlPendingW = width;
		s_sdlPendingH = height;
		s_sdlPending = true;
		if (MvmDebugLog_Active())
			MvmDebugLog_LinefAlways("window.resize", "queued SDL commit %dx%d messageDepth=%d", width, height, s_messageDepth);
		return true;
	}
	const auto started = GetTickCount64();
	if (MvmDebugLog_Active())
		MvmDebugLog_LinefAlways("window.resize", "SDL commit begin %dx%d messageDepth=%d", width, height, s_messageDepth);
	Sdl3Window* window = Sdl3_WindowForHwnd(GameWindow());
	if (!window)
		return false;

	// NEVER ask SDL for a size while the window is fullscreen. A fullscreen window has no size for
	// SDL to set; the request is taken as a mode change, the engine answers it by rebuilding the
	// swapchain on its render thread (IDXGISwapChain::SetFullscreenState, which sends the window a
	// message and waits) while this thread is blocked inside SDL_SetWindowSize/SyncWindow, which has
	// pumped the engine's resize handler into rendersystemdx11 -> dxgi. Measured 2026-10-13: the game
	// went "Not Responding" with the main thread in that SDL call and the render thread in
	// SetFullscreenState, and the main thread logged nothing after. Windowed resizes with the RenoDX
	// DLSS add-on loaded completed every time (5 commands + a mouse drag), so the add-on is not what
	// hangs a plain resize. The engine's own fullscreen transition also lands here as an
	// "external" resize, so this is the one place to refuse it. Fullscreen sizing belongs to the
	// engine's Display Mode / Resolution settings; RequestSize leaves fullscreen the proper way first.
	if ((Sdl3_GetWindowFlags(window) & kSdlWindowFullscreen) != 0) {
		if (MvmDebugLog_Active())
			MvmDebugLog_LinefAlways("window.resize", "SDL commit %dx%d skipped: window is fullscreen", width, height);
		return true;
	}

	// Every SetWindowSize below produces a WM_SIZE of our own making (the nudge guarantees at
	// least one), and SDL_SyncWindow can deliver it RE-ENTRANTLY, from inside this function. The
	// WM_SIZE handler treats any size change outside the modal loop as external and commits it --
	// which would call back into here, meet a window already holding the target, nudge it again,
	// and never stop. Recording the commit and arming the cooldown BEFORE the first call is what
	// breaks that cycle; the cooldown also covers the nudge size, which is not the recorded one.
	s_lastCommitW = width;
	s_lastCommitH = height;
	s_commitCooldownFrames = kCommitCooldownFrames;

	int currentW = 0, currentH = 0;
	if (WindowClientSize(GameWindow(), currentW, currentH) && currentW == width && currentH == height) {
		if (!Sdl3_SetWindowSize(window, width, height + 2))
			return false;
		Sdl3_SyncWindow(window);
	}

	if (!Sdl3_SetWindowSize(window, width, height))
		return false;
	Sdl3_SyncWindow(window);
	if (MvmDebugLog_Active())
		MvmDebugLog_LinefAlways("window.resize", "SDL commit end %dx%d elapsedMs=%llu", width, height, GetTickCount64() - started);
	return true;
}

// Even dimensions only: odd render sizes are a persistent source of trouble downstream (yuv420p
// encoders reject them outright), and a video mode is not the place to introduce one.
int MakeEven(int value) {
	return WindowSnap_MakeEven(value);
}

void ArmVerify(int width, int height, const char* trigger, bool external) {
	if (width <= 0 || height <= 0)
		return;

	s_verifyW = width;
	s_verifyH = height;
	s_verifyFramesLeft = kVerifyFrames;
	s_verifyReasserted = false;
	s_verifyPending = s_commit != WindowResizeCommit::Off;
	s_videoModeRetryFrames = 0;

	if (MvmDebugLog_Active())
		MvmDebugLog_LinefAlways("window.resize", "resize finished (%s) client=%dx%d commit=%s",
			trigger, width, height,
			s_commit == WindowResizeCommit::VideoMode ? "videomode"
				: (s_commit == WindowResizeCommit::Auto ? "auto"
					: (s_commit == WindowResizeCommit::Sdl ? "sdl" : "off")));

	if (s_commit == WindowResizeCommit::Sdl) {
		// A resize from outside the drag loop is often the ENGINE'S OWN video-mode change: startup
		// applying the saved Display Mode, a switch to or from fullscreen, the Video settings. The
		// engine rebuilds its swapchain for those itself, so re-asserting at once (with its +2px
		// nudge) only adds two more rebuilds on top of a mode change still in flight -- and an SDL
		// size call that lands while the render thread is inside SetFullscreenState is the measured
		// "Not Responding" hang described in ReassertThroughSdl. Wait instead: PumpVerify
		// re-asserts only if the swapchain has not followed by the end of the verify window, which
		// still covers a maximise or a tool's SetWindowPos that the engine ignores.
		if (external) {
			if (MvmDebugLog_Active())
				MvmDebugLog_LinefAlways("window.resize", "external %dx%d: waiting for the engine before any SDL commit",
					width, height);
			return;
		}
		const bool ok = ReassertThroughSdl(width, height);
		s_verifyReasserted = true;
		if (MvmDebugLog_Active())
			MvmDebugLog_LinefAlways("window.resize", "commit=sdl reassert %dx%d ok=%d", width, height, ok ? 1 : 0);
		return;
	}

	if (s_commit == WindowResizeCommit::VideoMode) {
		// The engine does the resize. Arm the retry window; PumpCommit issues it as soon as the
		// Panorama bridge is usable (it may not be during a map/demo load).
		s_videoModeRetryFrames = kVideoModeRetryFrames;
	}
}

// Issues the video-mode change once the Panorama bridge is ready, or falls back after waiting.
void PumpCommit() {
	if (s_commitCooldownFrames > 0)
		--s_commitCooldownFrames;

	if (s_videoModeRetryFrames <= 0)
		return;

	const int width = MakeEven(s_verifyW);
	const int height = MakeEven(s_verifyH);

	std::string error;
	if (EngineVideoMode_Apply(width, height, error)) {
		s_videoModeRetryFrames = 0;
		s_lastCommitW = width;
		s_lastCommitH = height;
		s_commitCooldownFrames = kCommitCooldownFrames;
		s_verifyW = width;
		s_verifyH = height;
		s_verifyFramesLeft = kVerifyFrames;
		return;
	}

	if (--s_videoModeRetryFrames > 0)
		return;

	// Waited long enough for a UI context. Fall back to the SDL re-assert so the window at least
	// stops disagreeing with itself, and say plainly that the engine was never asked.
	const bool ok = ReassertThroughSdl(s_verifyW, s_verifyH);
	s_verifyReasserted = true;
	if (MvmDebugLog_Active())
		MvmDebugLog_LinefAlways("window.resize",
			"videomode commit unavailable (%s) -- fell back to an SDL re-assert (ok=%d). "
			"The engine keeps its old video mode, so the picture may be wrong until you change "
			"the resolution in the video settings.",
			error.c_str(), ok ? 1 : 0);
	advancedfx::Warning("mirv_filmmaker window: could not ask the engine to change video mode (%s). "
		"Resize again once the UI is up, or set the resolution in the video settings.\n", error.c_str());
}

// One step of the "did the engine follow?" check.
//
// Reads the swapchain DESCRIPTION, never a backbuffer. RenderResolution_Sample() calls GetBuffer,
// which takes a reference, and `ResizeBuffers` FAILS while any backbuffer reference is
// outstanding -- a per-frame sampler from the main thread would intermittently break the engine's
// own swapchain resize, which is a very good way to produce black patches. Polled every few frames
// for the same reason.
void PumpVerify() {
	if (!s_verifyPending)
		return;

	unsigned swapWidth = 0, swapHeight = 0;
	const bool haveSwap = ((s_verifyFramesLeft % 10) == 0)
		&& RenderResolution_SwapChainSize(swapWidth, swapHeight);

	if (haveSwap && (int)swapWidth == s_verifyW && (int)swapHeight == s_verifyH) {
		s_verifyPending = false;
		if (MvmDebugLog_Active())
			MvmDebugLog_LinefAlways("window.resize",
				"FOLLOWED swapchain=%ux%u matches window %dx%d (reasserted=%d)",
				swapWidth, swapHeight, s_verifyW, s_verifyH, s_verifyReasserted ? 1 : 0);
		return;
	}

	if (--s_verifyFramesLeft > 0)
		return;

	// Also covers videomode: its ladder handles "the apply could not run", but since the settings
	// write is guarded (it cannot land on this build) the apply can now run and do nothing at all.
	// Falling back to the SDL re-assert is what actually gets the size applied.
	if (!s_verifyReasserted && s_commit != WindowResizeCommit::Off) {
		const bool ok = ReassertThroughSdl(s_verifyW, s_verifyH);
		s_verifyReasserted = true;
		s_verifyFramesLeft = kVerifyFramesAfterReassert;
		if (MvmDebugLog_Active())
			MvmDebugLog_LinefAlways("window.resize",
				"not followed yet -- re-asserting %dx%d through SDL (ok=%d), swapchain was %ux%u",
				s_verifyW, s_verifyH, ok ? 1 : 0, swapWidth, swapHeight);
		return;
	}

	s_verifyPending = false;
	if (MvmDebugLog_Active())
		MvmDebugLog_LinefAlways("window.resize",
			"NOT FOLLOWED window=%dx%d swapchain=%ux%u -- the engine kept its video mode; "
			"see Docs/features/filmmaker_window_resize.md 'if the engine does not follow'",
			s_verifyW, s_verifyH, swapWidth, swapHeight);
}

// Applies (or removes) the resizable style, on a slow cadence.
//
// Deliberately NOT every frame and deliberately without a give-up counter. The engine reasserts
// its window configuration (the render contract records it doing exactly that to SetWindowPos),
// so the style can be cleared out from under us at any time and has to be restorable at any time.
// A per-frame contest would be a SetWindowPos storm; a give-up rule made the feature die a few
// seconds into every session.
void PumpApplyStyle() {
	HWND hwnd = GameWindow();
	if (!hwnd)
		return;

	// Never touch the style while the user is inside the modal loop -- changing the frame mid-drag
	// is how a drag gets dropped.
	if (s_inModalLoop)
		return;

	const unsigned long long now = GetTickCount64();
	if (now - s_lastStyleCheckMs < kStyleCheckIntervalMs)
		return;
	s_lastStyleCheckMs = now;

	Sdl3Window* window = Sdl3_WindowForHwnd(hwnd);
	if (!window)
		return;

	const unsigned long long flags = Sdl3_GetWindowFlags(window);
	const bool fullscreen = (flags & kSdlWindowFullscreen) != 0;
	const bool isResizable = (flags & kSdlWindowResizable) != 0;
	const bool wantResizable = s_enabled && !fullscreen;

	if (wantResizable == isResizable)
		return;

	const bool ok = Sdl3_SetWindowResizable(window, wantResizable);
	if (wantResizable && ok)
		Sdl3_SetWindowMinimumSize(window, kMinClientW, kMinClientH);

	++s_styleReapplies;
	if (MvmDebugLog_Active())
		MvmDebugLog_LinefAlways("window.resize",
			"set resizable=%d ok=%d fullscreen=%d reapply#=%d flags=0x%llx",
			wantResizable ? 1 : 0, ok ? 1 : 0, fullscreen ? 1 : 0, s_styleReapplies,
			(unsigned long long)flags);
}

const char* CommitName(WindowResizeCommit commit) {
	switch (commit) {
	case WindowResizeCommit::VideoMode: return "videomode (BROKEN on this build -- falls back to sdl)";
	case WindowResizeCommit::Auto: return "auto";
	case WindowResizeCommit::Sdl: return "sdl";
	default: return "off";
	}
}

// Shared by "window size" and "window aspect": ask the engine for a size directly, without a drag.
bool RequestSize(int width, int height, const char* cmd) {
	width = MakeEven(width);
	height = MakeEven(height);

	Sdl3Window* window = Sdl3_WindowForHwnd(GameWindow());
	if (!window) {
		advancedfx::Warning("%s window: no SDL window yet (%s).\n", cmd, Sdl3_StatusText());
		return false;
	}
	if ((Sdl3_GetWindowFlags(window) & kSdlWindowFullscreen) != 0) {
		// Ask the engine to leave fullscreen through its own Display Mode setting, then size the
		// window from PumpLeaveFullscreen once it has. See the guard in ReassertThroughSdl.
		std::string error;
		if (!EngineVideoMode_LeaveFullscreen(error)) {
			advancedfx::Warning("%s window: the window is fullscreen and the engine could not be asked to "
				"leave it (%s). Set Display Mode to Windowed in Settings > Video, then try again.\n",
				cmd, error.c_str());
			return false;
		}
		s_leaveFullscreenPending = true;
		s_leaveW = width;
		s_leaveH = height;
		s_leaveDeadlineMs = GetTickCount64() + kLeaveFullscreenTimeoutMs;
		s_leaveSettleFrames = 0;
		if (MvmDebugLog_Active())
			MvmDebugLog_LinefAlways("window.resize", "fullscreen: asked the engine for Windowed, then %dx%d", width, height);
		advancedfx::Message("%s window: fullscreen -- switching to Windowed first, then %dx%d.\n", cmd, width, height);
		return true;
	}
	// An explicit command applies the size through the route that works, whatever the commit mode
	// is -- commit mode governs what happens after a DRAG. ReassertThroughSdl also handles the case
	// where the window is already this size, which a bare SDL_SetWindowSize would silently ignore.
	if (!ReassertThroughSdl(width, height)) {
		advancedfx::Warning("%s window: SDL_SetWindowSize(%d, %d) failed.\n", cmd, width, height);
		return false;
	}

	// Verify, but do not commit again: the size has just been applied, and ArmVerify would only ask
	// SDL for it a second time.
	s_verifyW = width;
	s_verifyH = height;
	s_verifyFramesLeft = kVerifyFrames;
	s_verifyReasserted = true;
	s_verifyPending = s_commit != WindowResizeCommit::Off;
	s_videoModeRetryFrames = 0;
	if (MvmDebugLog_Active())
		MvmDebugLog_LinefAlways("window.resize", "command requested %dx%d through SDL", width, height);

	advancedfx::Message("%s window: requested %dx%d.\n", cmd, width, height);
	return true;
}

// Second half of a size request made from fullscreen: wait for the engine to actually be windowed,
// give it a moment to finish its own swapchain rebuild, then apply the size the normal way.
void PumpLeaveFullscreen() {
	if (!s_leaveFullscreenPending)
		return;

	Sdl3Window* window = Sdl3_WindowForHwnd(GameWindow());
	if (window && (Sdl3_GetWindowFlags(window) & kSdlWindowFullscreen) == 0) {
		if (++s_leaveSettleFrames < kLeaveFullscreenSettleFrames)
			return;
		s_leaveFullscreenPending = false;
		RequestSize(s_leaveW, s_leaveH, "mirv_filmmaker");
		return;
	}
	s_leaveSettleFrames = 0;

	if (GetTickCount64() < s_leaveDeadlineMs)
		return;
	s_leaveFullscreenPending = false;
	if (MvmDebugLog_Active())
		MvmDebugLog_LinefAlways("window.resize", "fullscreen: engine did not leave fullscreen within %llu ms", kLeaveFullscreenTimeoutMs);
	advancedfx::Warning("mirv_filmmaker window: the engine did not leave fullscreen, so %dx%d was not applied. "
		"Set Display Mode to Windowed in Settings > Video (or the Custom Resolution row on that page).\n",
		s_leaveW, s_leaveH);
}

} // namespace

void WindowResize_OnWindowMessage(void* hwndRaw, unsigned msg, unsigned long long wParam, long long lParam) {
	HWND hwnd = (HWND)hwndRaw;
	if (!hwnd)
		return;
	s_hwnd = hwnd;
	WindowMessageScope messageScope;

	switch (msg) {
	case WM_ACTIVATEAPP:
	case WM_ACTIVATE:
		if ((msg == WM_ACTIVATEAPP && !wParam)
			|| (msg == WM_ACTIVATE && LOWORD(wParam) == WA_INACTIVE)) {
			WindowSizeOverlay_Hide();
			if (MvmDebugLog_Active())
				MvmDebugLog_LinefAlways("window.resize", "focus lost: readout hidden");
		}
		break;
	case WM_ENTERSIZEMOVE: {
		// MOVE or RESIZE -- Windows does not say which, and this fires the moment you grab the
		// title bar. Record the entry size and show NOTHING; only a WM_SIZING/WM_SIZE that
		// actually changes the size promotes this session to a resize.
		s_inModalLoop = true;
		s_resizedInModalLoop = false;
		s_modalEnterW = 0;
		s_modalEnterH = 0;
		s_lastNote[0] = 0;
		PrecisionGuard_Release(s_precision);
		s_precisionDefends = 0;
		WindowClientSize(hwnd, s_modalEnterW, s_modalEnterH);
		LogSize("modal enter", s_modalEnterW, s_modalEnterH);
		break;
	}
	case WM_SIZING: {
		// Only ever sent while resizing, never while moving. This is the promotion signal.
		RECT* proposed = (RECT*)(INT_PTR)lParam;
		if (!proposed)
			break;
		// Shift-to-snap edits the proposed rect in place; the message still goes on to SDL and
		// DefWindowProc, which apply their own minimum-size clamp on top, so SDL's bookkeeping stays
		// correct.
		const WindowSizingStep step = WindowSizing(hwnd, wParam, *proposed, s_snapMode, kMinClientW, kMinClientH);
		if (!step.valid)
			break;
		const int width = step.width;
		const int height = step.height;
		const bool shiftHeld = step.shiftHeld;
		const char* note = step.note;

		s_clientW = width;
		s_clientH = height;
		s_resizedInModalLoop = true;

		// Precision mode claims the window for the rest of this drag step: the rect below is the
		// only one we will accept until Shift comes up. Windows Snap gets answered with it in
		// WM_WINDOWPOSCHANGING. Shift up => Approve() disarms, and snap is a feature again.
		PrecisionGuard_Approve(s_precision, shiftHeld,
			(int)proposed->left, (int)proposed->top,
			(int)(proposed->right - proposed->left), (int)(proposed->bottom - proposed->top),
			width, height);

		// Centre on the rect Windows is ABOUT to apply, not the one it still has: dragging a left
		// or top edge moves the origin too, so the live rect would leave the card lagging.
		ShowOverlayAt(hwnd, step.centerX, step.centerY, width, height, note);
		snprintf(s_lastNote, sizeof(s_lastNote), "%s", note);
		LogSize(shiftHeld ? "sizing (snap)" : "sizing", width, height);
		break;
	}
	case WM_WINDOWPOSCHANGING: {
		// WINDOWS SNAP'S ONE CHANCE TO STEAL A PRECISION DRAG. Aero Snap does not go through
		// WM_SIZING -- it decides a rect and applies it -- but it must still ASK here first, and a
		// handler may edit the WINDOWPOS in place. That is the same "constrain by editing the
		// proposal" contract WM_SIZING uses above, and nothing is consumed, so SDL and
		// DefWindowProc still see the (now corrected) message and keep their bookkeeping.
		//
		// Only while a Shift-held resize is in progress. Everything else -- plain drags, maximise,
		// restore, SDL, the engine's own video-mode change -- passes through untouched.
		WINDOWPOS* pos = (WINDOWPOS*)(INT_PTR)lParam;
		if (!pos)
			break;
		const bool shiftStillHeld = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
		const bool sizeChanging = (pos->flags & SWP_NOSIZE) == 0;
		const bool moveChanging = (pos->flags & SWP_NOMOVE) == 0;
		PrecisionGuardDecision guard = PrecisionGuard_Evaluate(s_precision,
			s_inModalLoop, shiftStillHeld, sizeChanging, moveChanging,
			pos->x, pos->y, pos->cx, pos->cy);
		if (!guard.clampSize && s_precisionHoldFrames > 0) {
			// The drag is over (so no modal loop, and Shift is long since up), but the rect it
			// settled on is still ours for a moment. Same decision, same tested function.
			guard = PrecisionGuard_Evaluate(s_precisionHold,
				/*inModalLoop*/ true, /*shiftHeld*/ true, sizeChanging, moveChanging,
				pos->x, pos->y, pos->cx, pos->cy);
		}
		if (!guard.clampSize)
			break;
		++s_precisionDefends;
		if (MvmDebugLog_Active())
			MvmDebugLog_LinefAlways("window.snapguard",
				"refused %dx%d at %d,%d -> kept %dx%d at %d,%d (defends=%d)",
				(int)pos->cx, (int)pos->cy, (int)pos->x, (int)pos->y,
				guard.width, guard.height,
				guard.clampMove ? guard.x : (int)pos->x,
				guard.clampMove ? guard.y : (int)pos->y,
				s_precisionDefends);
		pos->cx = guard.width;
		pos->cy = guard.height;
		if (guard.clampMove) {
			pos->x = guard.x;
			pos->y = guard.y;
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
		const bool changed = (width != s_clientW || height != s_clientH);
		s_clientW = width;
		s_clientH = height;
		if (!changed || !s_mainThreadStarted)
			break;

		if (s_inModalLoop) {
			s_resizedInModalLoop = true;
			// WM_SIZING has already placed the card on the rect being applied; re-centring here
			// on the same rect is harmless and covers a resize that arrives without WM_SIZING.
			// It has to carry the SAME note, or the snap line blinks out on every drag step.
			ShowOverlayCentered(hwnd, width, height, s_lastNote);
			LogSize("size (modal)", width, height);
			break;
		}

		// Outside the modal loop: maximise / restore / a programmatic resize. No WM_EXITSIZEMOVE
		// will arrive, so the card has to arm its own hide and the commit has to arm here.
		//
		// The engine's own video-mode change also lands here. Committing again for a size WE just
		// asked for would ping-pong forever, so a matching size (or anything inside the cooldown)
		// is recorded and otherwise ignored.
		// PRECISION DEFENCE, PART 2. A snap that lands after the drag loop shows up right here as
		// an "external" resize, and the branch below would commit it to the engine as a video mode.
		// While the hold is live, refuse it instead: drop the maximised state if Windows set one
		// (a maximised window ignores a plain resize) and put the dragged size back.
		if (s_precisionHoldFrames > 0 && (width != s_precisionHold.clientW || height != s_precisionHold.clientH)) {
			const int keepW = s_precisionHold.clientW;
			const int keepH = s_precisionHold.clientH;
			if (MvmDebugLog_Active())
				MvmDebugLog_LinefAlways("window.snapguard",
					"post-drag %s to %dx%d refused; restoring %dx%d",
					(wParam == SIZE_MAXIMIZED) ? "maximise" : "resize", width, height, keepW, keepH);
			advancedfx::Warning(
				"mirv_filmmaker window: Windows Snap %s the window to %dx%d; keeping the %dx%d you dragged.\n",
				(wParam == SIZE_MAXIMIZED) ? "maximised" : "resized", width, height, keepW, keepH);
			++s_precisionDefends;
			// Disarm before ShowWindow: restore dispatches WM_SIZE recursively too.
			s_precisionHoldFrames = 0;
			PrecisionGuard_Release(s_precisionHold);
			if (IsZoomed(hwnd))
				ShowWindow(hwnd, SW_RESTORE);
			ReassertThroughSdl(keepW, keepH);
			s_clientW = keepW;
			s_clientH = keepH;
			s_precisionHoldFrames = 0; // one refusal per drag; do not fight the user afterwards
			PrecisionGuard_Release(s_precisionHold);
			break;
		}

		const bool isOurCommit = (width == s_lastCommitW && height == s_lastCommitH)
			|| s_commitCooldownFrames > 0;
		LogSize(isOurCommit ? "size (our commit)" : "size", width, height);
		if (isOurCommit)
			break;

		ShowOverlayCentered(hwnd, width, height, nullptr);
		WindowSizeOverlay_HideAfter(kOverlayHoldMs);
		ArmVerify(width, height, "external", true);
		break;
	}
	case WM_MOVING: {
		// Sent continuously while the window is dragged by its title bar. The card is NEVER shown
		// for a move -- that was a reported defect and WindowSizeOverlay_MoveTo no-ops when hidden.
		// This only carries an already-visible card along with the window, so the numbers stay in
		// the middle of the window instead of being left behind on the desktop when you finish a
		// resize and immediately reposition.
		const RECT* proposed = (const RECT*)(INT_PTR)lParam;
		if (!proposed || !WindowSizeOverlay_Visible())
			break;
		FrameInsets insets;
		int width = 0, height = 0;
		if (!WindowFrameInsets(hwnd, insets) || !WindowClientSize(hwnd, width, height))
			break;
		// A move does not change the size, so the live client size is the right one; only the
		// origin comes from the rect Windows is about to apply.
		WindowSizeOverlay_MoveTo(proposed->left + insets.left + width / 2,
			proposed->top + insets.top + height / 2);
		break;
	}
	case WM_MOVE: {
		// The settled position (and any programmatic move). By WM_MOVE the window HAS moved, so
		// the live client rect is the truth -- and deliberately not lParam: its packed coordinates
		// are 16-bit, which silently corrupts positions on a desktop wider than 32767px.
		if (!WindowSizeOverlay_Visible())
			break;
		int centerX = 0, centerY = 0;
		if (!WindowClientCenter(hwnd, centerX, centerY))
			break;
		WindowSizeOverlay_MoveTo(centerX, centerY);
		break;
	}
	case WM_EXITSIZEMOVE: {
		// Every exit must dismiss the readout, including a cancelled drag or a return
		// to the entry size. Those paths intentionally skip the size commit below.
		WindowSizeOverlay_HideAfter(kOverlayHoldMs);
		const bool wasResize = s_resizedInModalLoop;
		s_inModalLoop = false;
		s_resizedInModalLoop = false;

		int width = 0, height = 0;
		if (WindowClientSize(hwnd, width, height)) {
			s_clientW = width;
			s_clientH = height;
		}

		if (!wasResize || (width == s_modalEnterW && height == s_modalEnterH)) {
			// The user moved the window, or dragged an edge and put it back. Nothing happened.
			LogSize("modal exit (no resize)", width, height);
			break;
		}

		// A snap can still land the finished rect somewhere we never approved (it can arrive
		// without a WINDOWPOSCHANGING we were able to answer, e.g. a keyboard Win+Arrow mid-drag).
		// Committing THAT size to the engine is the reported bug, so precision mode hands the
		// commit the size it actually settled on and puts the window back to match.
		if (PrecisionGuard_FinalSizeHijacked(s_precision, width, height)) {
			const int keepW = s_precision.clientW;
			const int keepH = s_precision.clientH;
			advancedfx::Warning(
				"mirv_filmmaker window: Windows Snap resized the drag to %dx%d; keeping the %dx%d you dragged.\n",
				width, height, keepW, keepH);
			if (MvmDebugLog_Active())
				MvmDebugLog_LinefAlways("window.snapguard",
					"exit hijacked: final %dx%d, re-asserting approved %dx%d", width, height, keepW, keepH);
			ReassertThroughSdl(keepW, keepH);
			width = keepW;
			height = keepH;
			s_clientW = keepW;
			s_clientH = keepH;
			ShowOverlayCentered(hwnd, width, height, s_lastNote);
		}
		if (s_precision.active) {
			s_precisionHold = s_precision;
			s_precisionHoldFrames = kPrecisionHoldFrames;
		}
		PrecisionGuard_Release(s_precision);

		WindowSizeOverlay_HideAfter(kOverlayHoldMs);
		ArmVerify(width, height, "drag", false);
		break;
	}
	default:
		break;
	}
}

void WindowResize_PumpMainThread() {
	s_mainThreadStarted = true;
	WindowSizeOverlay_Pump();
	if (s_messageDepth > 0 || s_inModalLoop)
		return;
	if (s_sdlPending) {
		const int width = s_sdlPendingW, height = s_sdlPendingH;
		s_sdlPending = false; // clear BEFORE SDL can deliver re-entrant messages
		unsigned swapW = 0, swapH = 0;
		if (!RenderResolution_SwapChainSize(swapW, swapH) || swapW != width || swapH != height)
			ReassertThroughSdl(width, height);
	}
	if (s_precisionHoldFrames > 0 && --s_precisionHoldFrames == 0)
		PrecisionGuard_Release(s_precisionHold);
	PumpLeaveFullscreen();
	PumpApplyStyle();
	PumpCommit();
	PumpVerify();
}

void WindowResize_Shutdown() {
	s_sdlPending = false;
	s_leaveFullscreenPending = false;
	s_mainThreadStarted = false;
	WindowSizeOverlay_Shutdown();
	s_verifyPending = false;
	s_videoModeRetryFrames = 0;
	s_inModalLoop = false;
	s_resizedInModalLoop = false;
}

bool WindowResize_Enabled() {
	return s_enabled;
}

bool WindowResize_GetClientSize(int& outWidth, int& outHeight) {
	return WindowClientSize(GameWindow(), outWidth, outHeight);
}

void WindowResize_SetEnabled(bool enabled) {
	if (s_enabled == enabled)
		return;
	s_enabled = enabled;
	s_lastStyleCheckMs = 0; // apply now rather than up to half a second later
	PumpApplyStyle();
}

std::string WindowResize_Describe() {
	std::ostringstream out;

	HWND hwnd = GameWindow();
	Sdl3Window* window = Sdl3_WindowForHwnd(hwnd);
	const unsigned long long flags = window ? Sdl3_GetWindowFlags(window) : 0ull;

	int clientW = 0, clientH = 0;
	WindowClientSize(hwnd, clientW, clientH);
	const AspectMatch match = AspectRatio_Match(clientW, clientH);

	out << "mirv_filmmaker window:\n";
	out << "  resizable      : " << (s_enabled ? "ON" : "off")
		<< " (style re-applied " << s_styleReapplies << " times this session)\n";
	out << "  overlay        : " << (s_overlayEnabled ? "ON" : "off") << "\n";
	out << "  shift snap     : " << WindowSnapModeName(s_snapMode) << "\n";
	// Only interesting after a drag, and 0 is the answer you want: it means Windows Snap never
	// tried to take a precision drag away (or you never held Shift near a screen edge).
	out << "  snaps refused  : " << s_precisionDefends << " (last Shift drag)" << "\n";
	out << "  commit mode    : " << CommitName(s_commit) << "\n";
	out << "  engine commit  : " << (EngineVideoMode_Available()
		? "available (Panorama bridge ready)" : "NOT available (no Panorama UI context yet)") << "\n";
	out << "  SDL binding    : " << Sdl3_StatusText() << "\n";
	out << "  game window    : " << (hwnd ? "found" : "NOT found yet (no window message seen)") << "\n";
	out << "  SDL window     : " << (window ? "found" : "not found") << "\n";
	if (window) {
		out << "  SDL flags      : 0x" << std::hex << flags << std::dec
			<< ((flags & kSdlWindowFullscreen) ? " FULLSCREEN" : "")
			<< ((flags & kSdlWindowResizable) ? " RESIZABLE" : "")
			<< ((flags & kSdlWindowBorderless) ? " BORDERLESS" : "") << "\n";
	}
	if (clientW > 0) {
		char line[192];
		snprintf(line, sizeof(line), "  client size    : %dx%d (%.4f)\n", clientW, clientH, match.ratio);
		out << line;
		out << "  aspect         : " << FormatWindowAspect(match) << "\n";
	}

	unsigned swapWidth = 0, swapHeight = 0;
	if (RenderResolution_SwapChainSize(swapWidth, swapHeight)) {
		char line[224];
		snprintf(line, sizeof(line), "  swapchain      : %ux%u%s\n",
			swapWidth, swapHeight,
			((int)swapWidth == clientW && (int)swapHeight == clientH)
				? " (matches the window)"
				: " (DOES NOT match the window -- expect black patches until the video mode changes)");
		out << line;
		out << "  (full D3D picture: \"" << "mirv_filmmaker render res report" << "\")\n";
	}
	if (s_videoModeRetryFrames > 0) {
		char line[128];
		snprintf(line, sizeof(line), "  commit         : waiting for the UI, %d frames left\n",
			s_videoModeRetryFrames);
		out << line;
	}
	if (s_verifyPending) {
		char line[128];
		snprintf(line, sizeof(line), "  verifying      : %dx%d, %d frames left\n",
			s_verifyW, s_verifyH, s_verifyFramesLeft);
		out << line;
	}
	return out.str();
}

void WindowResize_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd) {
	const char* sub = (argc >= 3) ? args->ArgV(2) : "status";

	if (0 == _stricmp(sub, "status")) {
		advancedfx::Message("%s", WindowResize_Describe().c_str());
		return;
	}

	if (0 == _stricmp(sub, "resizable")) {
		bool want = !s_enabled;
		if (argc >= 4) {
			const char* value = args->ArgV(3);
			if (0 == _stricmp(value, "toggle")) want = !s_enabled;
			else want = (0 != atoi(value)) || 0 == _stricmp(value, "on") || 0 == _stricmp(value, "true");
		}
		WindowResize_SetEnabled(want);
		advancedfx::Message("%s window: resizable %s.\n", cmd, s_enabled ? "ON" : "off");
		return;
	}

	if (0 == _stricmp(sub, "size")) {
		if (argc < 5) {
			advancedfx::Message("usage: %s window size <width> <height>\n", cmd);
			return;
		}
		const int width = atoi(args->ArgV(3));
		const int height = atoi(args->ArgV(4));
		if (width < kMinClientW || height < kMinClientH) {
			advancedfx::Warning("%s window: size must be at least %dx%d.\n", cmd, kMinClientW, kMinClientH);
			return;
		}
		RequestSize(width, height, cmd);
		return;
	}

	if (0 == _stricmp(sub, "aspect")) {
		if (argc < 4) {
			advancedfx::Message("usage: %s window aspect <2.39 | 16:9 | 4:3 ...> - keeps the current "
				"width and sets the height.\n", cmd);
			return;
		}
		const double ratio = ResolveWindowRatioArgument(args->ArgV(3));
		if (!(ratio > 0.0)) {
			advancedfx::Warning("%s window: could not read an aspect ratio from \"%s\". "
				"Try \"%s window ratios\".\n", cmd, args->ArgV(3), cmd);
			return;
		}
		int clientW = 0, clientH = 0;
		if (!WindowClientSize(GameWindow(), clientW, clientH)) {
			advancedfx::Warning("%s window: no game window yet.\n", cmd);
			return;
		}
		int height = AspectRatio_HeightForWidth(clientW, ratio);
		int width = clientW;
		if (height < kMinClientH) {
			height = kMinClientH;
			width = AspectRatio_WidthForHeight(height, ratio);
		}
		if (RequestSize(width, height, cmd))
			advancedfx::Message("%s window: %dx%d is %.4f:1.\n", cmd, MakeEven(width), MakeEven(height), ratio);
		return;
	}

	if (0 == _stricmp(sub, "ratios")) {
		std::size_t count = 0;
		const AspectRatioEntry* table = AspectRatio_Table(count);
		advancedfx::Message("%s window ratios (closest match is what the resize card reports):\n", cmd);
		for (std::size_t i = 0; i < count; ++i) {
			const char* family = table[i].family == AspectFamily::Cinema ? "cinema"
				: (table[i].family == AspectFamily::Social ? "social" : "standard");
			advancedfx::Message("  %-8s %-8s %.4f  %s\n", table[i].label, family, table[i].ratio, table[i].name);
		}
		return;
	}

	if (0 == _stricmp(sub, "overlay")) {
		if (argc >= 4 && 0 == _stricmp(args->ArgV(3), "test")) {
			int clientW = 0, clientH = 0;
			WindowClientSize(GameWindow(), clientW, clientH);
			if (clientW > 0) {
				ShowOverlayCentered(GameWindow(), clientW, clientH, "SHIFT to snap");
				WindowSizeOverlay_HideAfter(2000);
				advancedfx::Message("%s window: overlay shown for 2s.\n", cmd);
			} else {
				advancedfx::Warning("%s window: no game window yet.\n", cmd);
			}
			return;
		}
		if (argc >= 4)
			s_overlayEnabled = (0 != atoi(args->ArgV(3))) || 0 == _stricmp(args->ArgV(3), "on");
		else
			s_overlayEnabled = !s_overlayEnabled;
		if (!s_overlayEnabled)
			WindowSizeOverlay_Hide();
		advancedfx::Message("%s window: resize overlay %s.\n", cmd, s_overlayEnabled ? "ON" : "off");
		return;
	}

	if (0 == _stricmp(sub, "snap")) {
		if (argc >= 4) {
			if (!ParseWindowSnapMode(args->ArgV(3), s_snapMode)) {
				advancedfx::Warning("%s window: snap takes both | ratio | resolution | off.\n", cmd);
				return;
			}
		}
		advancedfx::Message("%s window: hold SHIFT while dragging to snap -- mode %s.\n",
			cmd, WindowSnapModeName(s_snapMode));
		return;
	}

	if (0 == _stricmp(sub, "commit")) {
		if (argc >= 4) {
			const char* value = args->ArgV(3);
			if (0 == _stricmp(value, "videomode")) s_commit = WindowResizeCommit::VideoMode;
			else if (0 == _stricmp(value, "auto")) s_commit = WindowResizeCommit::Auto;
			else if (0 == _stricmp(value, "sdl")) s_commit = WindowResizeCommit::Sdl;
			else if (0 == _stricmp(value, "off") || 0 == _stricmp(value, "none")) s_commit = WindowResizeCommit::Off;
			else {
				advancedfx::Warning("%s window: commit takes sdl | auto | videomode | off.\n", cmd);
				return;
			}
			if (s_commit == WindowResizeCommit::VideoMode)
				advancedfx::Warning("%s window: videomode cannot set a size on this CS2 build -- "
					"'setting.defaultres' is not a writable ui preference, so the apply is skipped and "
					"this falls back to the SDL commit. See Docs/features/filmmaker_window_resize.md.\n", cmd);
		}
		advancedfx::Message("%s window: commit mode %s.\n", cmd, CommitName(s_commit));
		return;
	}

	if (0 == _stricmp(sub, "probe")) {
		if (argc >= 4)
			s_probe = (0 != atoi(args->ArgV(3)));
		else
			s_probe = !s_probe;
		advancedfx::Message("%s window: window.resize probe %s (needs \"mvm_debug start\" to write "
			"anything).\n", cmd, s_probe ? "ON" : "off");
		return;
	}

	advancedfx::Message(
		"%s window status              - SDL binding, window size, aspect, engine agreement.\n"
		"%s window resizable on|off    - let you drag the CS2 window border (default ON, windowed only).\n"
		"%s window size <w> <h>        - ask the engine for an exact size.\n"
		"%s window aspect <r>          - keep the width, set the height for a ratio (2.39, 16:9, ...).\n"
		"%s window ratios              - list the aspect ratios the readout can name.\n"
		"%s window snap both|ratio|resolution|off - what holding SHIFT while dragging snaps to.\n"
		"%s window overlay on|off|test - the size/aspect card shown while resizing.\n"
		"%s window commit sdl|auto|videomode|off - how the finished size reaches the engine\n"
		"                              (default sdl; videomode cannot set a size on this build).\n"
		"%s window probe 0|1           - log every resize message to the mvm_debug log.\n",
		cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd);
}

} // namespace Filmmaker
