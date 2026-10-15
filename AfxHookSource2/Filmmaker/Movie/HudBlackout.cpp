#include "HudBlackout.h"

#include "../../WrpConsole.h"
#include "../../../Shared/AfxConsole.h"

#include "../../../Deps/release/prop/cs2/sdk_src/public/cdll_int.h"
#include "../../../Deps/release/prop/cs2/sdk_src/public/icvar.h" // read/restore the crosshair convar

#include <atomic>
#include <cstring>

extern SOURCESDK::CS2::ISource2EngineToClient* g_pEngineToClient;

namespace Filmmaker {

namespace {

std::atomic<bool> g_active{ false };

// A PAUSED demo is still an active demo. IsPlayingDemo() alone flips false on pause, and pausing
// to compose a shot is exactly when a clean screen is wanted -- reading it that way would drop the
// blackout at the worst possible moment.
bool DemoIsLoaded() {
	if (!g_pEngineToClient) return false;
	if (auto pDemo = g_pEngineToClient->GetDemoFile())
		return pDemo->IsPlayingDemo() || pDemo->IsDemoPaused();
	return false;
}

// THE CROSSHAIR IS NOT A PANORAMA PANEL. Hiding the HUD panels leaves it drawn, and a crosshair
// sitting in the middle of an otherwise clean frame is exactly the thing F8 is meant to remove.
// It is a convar, so it is saved and put back rather than assumed to have been on: someone who
// directs with `crosshair 0` must not have it switched ON by turning a blackout off.
bool g_crosshairSaved = false;
bool g_crosshairWasOn = true;

void ExecClientCmd(const char* cmd) {
	if (g_pEngineToClient)
		g_pEngineToClient->ExecuteClientCmd(0, cmd, true);
}

bool ReadCrosshairOn() {
	using namespace SOURCESDK::CS2;
	if (!g_pCVar) return true;
	Cvar_s* handle = g_pCVar->GetCvar(g_pCVar->FindConVar("crosshair", false).Get());
	if (!handle) return true;
	return handle->m_Value.m_bValue;
}

// Called on the ON and OFF edges only -- never per frame. Writing a convar every frame would
// fight anyone changing it and spam the console.
void ApplyCrosshair(bool blackedOut) {
	if (blackedOut) {
		if (!g_crosshairSaved) {
			g_crosshairWasOn = ReadCrosshairOn();
			g_crosshairSaved = true;
		}
		ExecClientCmd("crosshair 0");
	} else if (g_crosshairSaved) {
		g_crosshairSaved = false;
		ExecClientCmd(g_crosshairWasOn ? "crosshair 1" : "crosshair 0");
	}
}

} // namespace

bool HudBlackout_Active() { return g_active.load(std::memory_order_relaxed); }

void HudBlackout_Set(bool on) {
	const bool was = g_active.exchange(on, std::memory_order_relaxed);
	if (was != on) ApplyCrosshair(on);
}

void HudBlackout_Toggle() { HudBlackout_Set(!HudBlackout_Active()); }

void HudBlackout_RunFrame() {
	// Never let a blackout outlive the demo it was turned on in. The toggle that clears it is
	// itself one of the things it hides, so a blackout stuck on in the main menu would look like
	// the UI had simply broken.
	if (g_active.load(std::memory_order_relaxed) && !DemoIsLoaded())
		HudBlackout_Set(false); // through Set so the crosshair is restored too
}

void HudBlackout_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd) {
	const char* action = (argc >= 3) ? args->ArgV(2) : "toggle";

	if (0 == _stricmp(action, "status")) {
		advancedfx::Message("%s hud: screen is %s.\n", cmd,
			HudBlackout_Active() ? "BLACKED OUT (F8 restores it)" : "showing all UI");
		return;
	}
	if (0 == _stricmp(action, "on") || 0 == _stricmp(action, "1")) HudBlackout_Set(true);
	else if (0 == _stricmp(action, "off") || 0 == _stricmp(action, "0")) HudBlackout_Set(false);
	else if (0 == _stricmp(action, "toggle")) HudBlackout_Toggle();
	else {
		advancedfx::Warning("usage: %s hud [on|off|toggle|status]\n", cmd);
		return;
	}
	advancedfx::Message("%s hud: %s.\n", cmd,
		HudBlackout_Active()
			? "screen cleared -- every overlay hidden (F8 restores)"
			: "UI restored");
}

} // namespace Filmmaker
