// Deadlock's Panorama access layer.
//
// This file exists so the shared Panorama plumbing can be REUSED rather than duplicated.
// AfxHookSource2/Filmmaker/Panorama/PanoramaBridge.cpp and PanoramaFindPanel.cpp are entirely
// game-neutral except for one include of ../../DeathMsg.h, and that header is declaration-only:
// four accessor functions, one struct, and a handful of `extern ptrdiff_t` vtable displacements.
// Defining those here, with Deadlock-resolved values, lets both files compile into the Deadlock
// target byte-for-byte unchanged. DeathMsg.cpp itself -- CS2 death notices, econ, the CS2 HUD --
// is not part of this target.
//
// Workspace state is passed directly in RunScript, so this target never calls unknown CUIPanel
// attribute methods.

#include "DeadlockAddresses.h"
#include "DeadlockPanorama.h"

#include "../DeathMsg.h"
#include "../Globals.h"
#include "../../Shared/AfxConsole.h"
#include "../Filmmaker/Debug/MvmDebugLog.h"


// ---------------------------------------------------------------------------
// The DeathMsg.h contract, satisfied with Deadlock values.
// ---------------------------------------------------------------------------

currentGameCamera g_CurrentGameCamera = {};

namespace CS2 {
namespace PanoramaUIPanel {
ptrdiff_t getAttributeString = 0;
ptrdiff_t setAttributeString = 0;
}
namespace PanoramaPanelStyle {
// Only used by the CS2 death-notice styling path, which this target does not build. Left at zero
// so any accidental use is a guarded no-op rather than a call through a wild displacement.
ptrdiff_t setPanelStyleProperty = 0;
}
namespace PanoramaUIEngine {
ptrdiff_t makeSymbol = 0;
}
} // namespace CS2

unsigned char** AfxHookSource2_GetPanoramaUiEngine() {
	return Deadlock::g_pUIEngine;
}

void* AfxHookSource2_GetPanoramaRunScript() {
	return Deadlock::g_Panorama_RunScript;
}

unsigned char* AfxHookSource2_GetPanoramaHudPanel() {
	unsigned char** slot = Deadlock::g_pHudPanel;
	if (!slot) return nullptr;
	// The SLOT is non-null long before the panel is, and null again after teardown -- leaving a
	// demo, a map change, shutdown. Both dereferences are checked separately because this is asked
	// for every frame by the timeline and editor HUDs, so an unguarded chain here is an access
	// violation on every one of those transitions.
	unsigned char* clientPanel = slot[0];        // *slot -> CPanel2D*
	if (!clientPanel) return nullptr;
	return ((unsigned char**)clientPanel)[1];    // CPanel2D->uiPanel at +0x8 -> CUIPanel*
}

unsigned char* AfxHookSource2_GetPanoramaMainMenuPanel() {
	// Deadlock's main menu is the dashboard, a separate Panorama window, and the filmmaker UI does
	// not attach to it: there is no Demos navbar page in this port, only the in-game workspaces.
	// Returning null makes PanoramaBridge fall back to the HUD panel, which is what we want.
	return nullptr;
}

// Declared in DeathMsg.h; this target never installs the CS2 death-notice or Panorama-layout
// hooks, so both are deliberate no-ops rather than missing symbols.
void HookDeathMsg(HMODULE) {}
void HookPanorama(HMODULE) {}

// ---------------------------------------------------------------------------
// Attribute-offset calibration.
// ---------------------------------------------------------------------------

namespace Deadlock {

void Panorama_ReportCandidates() {
	advancedfx::Message("CUIPanel attributes are unused; workspace state is passed through RunScript.\n");
}

} // namespace Deadlock
