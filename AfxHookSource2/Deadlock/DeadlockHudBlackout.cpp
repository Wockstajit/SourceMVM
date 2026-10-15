#include "DeadlockHudBlackout.h"

#include "DeadlockEngine.h"

#include "../../Shared/AfxConsole.h"

#include <atomic>
#include <cstring>

namespace Deadlock {

namespace {

std::atomic<bool> g_active{ false };

// A PAUSED replay is still a loaded one. Pausing to compose a shot is exactly when a clean screen
// is wanted, so this must not read as "the replay ended" the moment playback stops -- see CS2's
// HudBlackout.cpp DemoIsLoaded, which this mirrors.
bool DemoIsLoaded() {
	return Engine_IsPlayingDemo() || Engine_IsDemoPaused();
}

} // namespace

bool HudBlackout_Active() { return g_active.load(std::memory_order_relaxed); }
void HudBlackout_Set(bool on) { g_active.store(on, std::memory_order_relaxed); }
void HudBlackout_Toggle() { HudBlackout_Set(!HudBlackout_Active()); }

void HudBlackout_RunFrame() {
	if (g_active.load(std::memory_order_relaxed) && !DemoIsLoaded())
		HudBlackout_Set(false);
}

void HudBlackout_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd) {
	const char* action = (argc >= 3) ? args->ArgV(2) : "toggle";

	if (0 == _stricmp(action, "status")) {
		advancedfx::Message("%s blackout: screen is %s.\n", cmd,
			HudBlackout_Active() ? "BLACKED OUT (F8 restores it)" : "showing all UI");
		return;
	}
	if (0 == _stricmp(action, "on") || 0 == _stricmp(action, "1")) HudBlackout_Set(true);
	else if (0 == _stricmp(action, "off") || 0 == _stricmp(action, "0")) HudBlackout_Set(false);
	else if (0 == _stricmp(action, "toggle")) HudBlackout_Toggle();
	else {
		advancedfx::Warning("usage: %s blackout [on|off|toggle|status]\n", cmd);
		return;
	}
	advancedfx::Message("%s blackout: %s.\n", cmd,
		HudBlackout_Active()
			? "screen cleared -- every overlay hidden (F8 restores)"
			: "UI restored");
}

} // namespace Deadlock
