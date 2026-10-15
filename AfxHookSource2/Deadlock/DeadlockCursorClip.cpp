#include "DeadlockCursorClip.h"

#include "DeadlockInput.h"

#include "../../Shared/AfxConsole.h"
#include "../../Shared/AfxDetours.h"

namespace Deadlock {

namespace {

bool g_installed = false;

typedef BOOL(WINAPI* ClipCursor_t)(const RECT*);
ClipCursor_t g_originalClipCursor = nullptr;

// The game's own last request, so the confinement can be restored exactly rather than guessed at.
RECT g_wantedRect = {};
bool g_haveWanted = false;

// Whether we are currently suppressing it, so the transitions are applied once each rather than
// every frame -- ClipCursor is a system call and this runs at frame rate.
bool g_suppressing = false;

unsigned long long g_intercepted = 0;

// Should the cursor be free right now?
//
// Both halves are required. UI mode alone is not enough on a borderless display, where releasing
// the clip does nothing useful because the window already covers the monitor.
bool WantFree() {
	return Input_UiMouse() && Input_IsWindowedMode();
}

BOOL WINAPI New_ClipCursor(const RECT* rect) {
	++g_intercepted;

	// nullptr means "release", which the game does on focus loss among other things. Remember that
	// too, or we would restore a stale rect the moment UI mode ended.
	if (rect) {
		g_wantedRect = *rect;
		g_haveWanted = true;
	} else {
		g_haveWanted = false;
	}

	if (WantFree()) {
		// Swallowed, not forwarded. The game keeps asking every frame and keeps being told nothing
		// happened, which is exactly the behaviour that makes this stable instead of a fight.
		g_suppressing = true;
		return g_originalClipCursor ? g_originalClipCursor(nullptr) : TRUE;
	}

	g_suppressing = false;
	return g_originalClipCursor ? g_originalClipCursor(rect) : TRUE;
}

} // namespace

bool CursorClip_Install() {
	if (g_installed) return true;

	// user32 is always loaded, and ClipCursor is an ordinary export -- no signature, nothing
	// version-coupled, nothing to re-derive after a game update.
	HMODULE user32 = GetModuleHandleW(L"user32.dll");
	if (!user32) return false;
	g_originalClipCursor = (ClipCursor_t)GetProcAddress(user32, "ClipCursor");
	if (!g_originalClipCursor) {
		advancedfx::Warning("AfxHookDeadlock: could not resolve ClipCursor.\n");
		return false;
	}

	DetourTransactionBegin();
	DetourUpdateThread(GetCurrentThread());
	DetourAttach(&(PVOID&)g_originalClipCursor, New_ClipCursor);
	if (NO_ERROR != DetourTransactionCommit()) {
		g_originalClipCursor = nullptr;
		advancedfx::Warning("AfxHookDeadlock: could not detour ClipCursor.\n");
		return false;
	}

	g_installed = true;
	advancedfx::Message("AfxHookDeadlock: cursor-clip hook installed.\n");
	return true;
}

bool CursorClip_Installed() { return g_installed; }

void CursorClip_RunFrame() {
	if (!g_installed || !g_originalClipCursor) return;

	const bool free = WantFree();
	if (free == g_suppressing) return;   // already in the right state

	if (free) {
		// Release NOW rather than waiting for the game's next ClipCursor call. Deadlock does not
		// necessarily re-apply the clip every frame, so without this the cursor stays trapped until
		// something else happens to trigger a re-clip.
		g_suppressing = true;
		g_originalClipCursor(nullptr);
	} else {
		g_suppressing = false;
		// Put back exactly what the game last asked for. Restoring a rect of our own devising is
		// how the cursor ends up confined to slightly the wrong box after leaving UI mode.
		if (g_haveWanted) g_originalClipCursor(&g_wantedRect);
	}
}

void CursorClip_Report() {
	advancedfx::Message(
		"  cursor:    clip hook %s  windowed=%s  uiMouse=%s  -> %s  (calls seen %llu)\n",
		g_installed ? "installed" : "NOT installed",
		Input_IsWindowedMode() ? "yes" : "no",
		Input_UiMouse() ? "yes" : "no",
		g_suppressing ? "cursor FREE to leave the window" : "confinement left to the game",
		g_intercepted);
	if (g_installed && !Input_IsWindowedMode())
		advancedfx::Message(
			"             borderless or fullscreen, so the clip is left alone by design -- the\n"
			"             window covers the monitor and there is nowhere for the cursor to go.\n");
}

} // namespace Deadlock
