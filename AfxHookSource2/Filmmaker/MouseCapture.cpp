#include "MouseCapture.h"

#include "Filmmaker.h" // LiveFxMenu_WantsGameMouseReleased: a UI click is not a game click
#include "Debug/MvmDebugLog.h"
#include "../../Shared/AfxConsole.h"

#include <atomic>

namespace Filmmaker {

namespace {
std::atomic<bool> g_freeMouse{ false };
std::atomic<bool> g_clickToCapture{ false };

void MouseCapture_ApplyFreeState(bool freeMouse, const char* reason) {
	const bool previous = g_freeMouse.exchange(freeMouse, std::memory_order_acq_rel);
	if (previous != freeMouse && MvmDebugLog_Active())
		MvmDebugLog_Linef("input.capture", "%s previous=%d free=%d",
			reason, previous ? 1 : 0, freeMouse ? 1 : 0);
}

const char* MouseCapture_StateText() {
	if (g_clickToCapture.load(std::memory_order_acquire))
		return MouseCapture_IsFree() ? "click-to-capture (free until game click)" : "click-to-capture (captured: the game has the mouse)";
	return MouseCapture_IsFree() ? "free (CS2 mouse-look disabled)" : "game";
}
}

void MouseCapture_Initialize(bool clickToCapture) {
	g_clickToCapture.store(clickToCapture, std::memory_order_release);
	g_freeMouse.store(clickToCapture, std::memory_order_release);
}

bool MouseCapture_IsFree() {
	return g_freeMouse.load(std::memory_order_acquire);
}

void MouseCapture_SetFree(bool freeMouse) {
	g_clickToCapture.store(false, std::memory_order_release);
	MouseCapture_ApplyFreeState(freeMouse, "command");
}

void MouseCapture_SetClickToCapture(bool clickToCapture) {
	g_clickToCapture.store(clickToCapture, std::memory_order_release);
	MouseCapture_ApplyFreeState(clickToCapture, "mode");
}

void MouseCapture_TakeForCamera(const char* reason) {
	if (!g_clickToCapture.load(std::memory_order_acquire)) return; // explicit free/game: not ours to change
	if (LiveFxMenu_WantsGameMouseReleased()) return;
	MouseCapture_ApplyFreeState(false, reason ? reason : "camera");
}

void MouseCapture_OnActivate(bool active) {
	if (MvmDebugLog_Active())
		MvmDebugLog_Linef("input.capture", "wm_activate active=%d clickMode=%d free=%d",
			active ? 1 : 0, g_clickToCapture.load(std::memory_order_acquire) ? 1 : 0,
			MouseCapture_IsFree() ? 1 : 0);

	if (active || !g_clickToCapture.load(std::memory_order_acquire))
		return;

	// Re-arm the free cursor whenever the game loses focus. Alt+Tab back in therefore
	// leaves the desktop cursor alone until the user explicitly clicks the game.
	MouseCapture_ApplyFreeState(true, "deactivate rearm");
}

void MouseCapture_OnGameClick() {
	const bool clickToCapture = g_clickToCapture.load(std::memory_order_acquire);
	if (MvmDebugLog_Active()) {
		static unsigned s_clickCount = 0;
		if ((s_clickCount++ % 8) == 0)
			MvmDebugLog_Linef("input.capture", "game_click clickMode=%d freeBefore=%d",
				clickToCapture ? 1 : 0, MouseCapture_IsFree() ? 1 : 0);
	}

	// A click on the live Insert panel is a UI click, not the user opting back into CS2's
	// relative mouse mode. Without this, the first press on any button or tab flipped the
	// global cursor mode underneath the panel the user is still working in.
	if (LiveFxMenu_WantsGameMouseReleased())
		return;

	// Same for a workspace in UI mode: the click is the panel's (a keyframe's right-click menu
	// died when this took the mouse for CS2). A click into the preview switches to GAME mode
	// first (MovieMode::OnUiClick), which captures the mouse there.
	if (CameraTimeline_WantsCursor() || MarkerMenu_WantsCursor() || CameraEditor_CustomizeModalOpen())
		return;

	if (!clickToCapture)
		return;

	// Do this after filmmaker/MirvInput have had a chance to consume the click for UI.
	// An actual game click is the explicit opt-in to CS2's relative mouse mode.
	MouseCapture_ApplyFreeState(false, "game click");
}

void MouseCapture_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd) {
	const char* action = (argc >= 3) ? args->ArgV(2) : "state";

	if (0 == _stricmp(action, "state")) {
		advancedfx::Message("%s mouse: %s.\n", cmd, MouseCapture_StateText());
		return;
	}

	if (0 == _stricmp(action, "free") || 0 == _stricmp(action, "on") || 0 == _stricmp(action, "1")) {
		MouseCapture_SetFree(true);
	} else if (0 == _stricmp(action, "click") || 0 == _stricmp(action, "arm")) {
		MouseCapture_SetClickToCapture(true);
	} else if (0 == _stricmp(action, "game") || 0 == _stricmp(action, "off") || 0 == _stricmp(action, "0")) {
		MouseCapture_SetFree(false);
	} else if (0 == _stricmp(action, "toggle")) {
		MouseCapture_SetFree(!MouseCapture_IsFree());
	} else {
		advancedfx::Warning("usage: %s mouse free|click|game|toggle|state\n", cmd);
		return;
	}

	advancedfx::Message("%s mouse: %s.\n", cmd, MouseCapture_StateText());
}

} // namespace Filmmaker
