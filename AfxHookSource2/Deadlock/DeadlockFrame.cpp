#include "DeadlockFrame.h"
#include "DeadlockCameraCommands.h"
#include "DeadlockCameraRuntime.h"
#include "DeadlockInput.h"
#include "DeadlockHotkeys.h"
#include "DeadlockHudBlackout.h"

#include "DeadlockAddresses.h"
#include "DeadlockBoot.h"
#include "DeadlockEngine.h"
#include "DeadlockDemo.h"
#include "DeadlockNetcon.h"
#include "DeadlockSchema.h"
#include "DeadlockUi.h"
#include "DeadlockUiCommandBridge.h"
#include "../Filmmaker/Panorama/UiLiveReload.h"
#include "../Filmmaker/Deadlock/DeadlockRender.h"
#include "../Filmmaker/Deadlock/DeadlockSettings.h"
#include "../Filmmaker/Deadlock/DeadlockShot.h"
#include "../Filmmaker/Deadlock/DeadlockWorkspaces.h"
#include "DeadlockPresentHook.h"

#include "../../Shared/AfxConsole.h"

#include <atomic>

namespace Deadlock {

namespace {

std::atomic<unsigned long long> g_renderCount{ 0 };
std::atomic<unsigned long long> g_mainCount{ 0 };

// IsPlayingDemo and IsDemoPaused both blip false partway through a seek on the CS2 side, so a
// panel gated directly on them flickers away and back on every scrub. Thirty frames of hysteresis
// is what stopped that; assume Deadlock behaves the same until measured.
const int kDemoDebounceFrames = 30;

bool g_demoLoadedDebounced = false;

// -mvmNoUi. Declared with the rest of the pump state so the pump can read it directly.
bool g_uiDisabled = false;
int g_demoFalseFrames = 0;

// The schema table is read from the MAIN thread, lazily, rather than from the module dispatcher.
// The dispatcher runs inside LoadLibraryExW and therefore under the loader lock, and at that
// moment the game has not populated the table yet -- walking it there would read an empty or
// half-built structure and cache the result forever. Retried each frame until it takes, then
// capped, so a genuinely broken pattern costs one warning rather than a scan per frame for the
// life of the process.
const int kSchemaAttemptCap = 600;
int g_schemaAttempts = 0;


void TrySchema() {
	if (Schema_Ready() || g_schemaAttempts >= kSchemaAttemptCap) return;
	if (!g_H_SchemaSystem) return;
	++g_schemaAttempts;
	if (Schema_Init(g_H_SchemaSystem)) return;
	if (g_schemaAttempts == kSchemaAttemptCap)
		advancedfx::Warning(
			"AfxHookDeadlock: gave up resolving the schema table. Entity-backed features stay off;\n"
			"'mirv_filmmaker health' reports why.\n");
}

void UpdateDemoDebounce() {
	// Blitz temporarily reports IsPlayingDemo=false while its revolver/card selection overlay owns
	// the screen. The loaded demo player remains valid throughout that phase, so use its presence
	// to keep the editor and timeline mounted. A real unload clears the player and still takes the
	// normal debounce path below.
	//
	// BUT THE PLAYER POINTER ALONE IS NOT PROOF OF A DEMO. Deadlock does not null that global when
	// a replay ends, and its vtable still validates, so `Demo_Player()` on its own latches this
	// true for the rest of the process. Measured after a replay disconnected: `demo=no tick=-1
	// level='dl_hideout'` while the workspace still believed a demo was loaded -- so the blit kept
	// clearing the frame and scaling the main menu into the preview rect, with our panels drawn
	// around it. That is the reported wrecked UI after leaving a replay. The demo tick is the
	// discriminator: a live player reports a tick, an abandoned one reports -1.
	if (Engine_IsPlayingDemo() || (Demo_Player() && Engine_DemoTick() >= 0)) {
		g_demoFalseFrames = 0;
		g_demoLoadedDebounced = true;
		return;
	}
	if (!g_demoLoadedDebounced) return;
	if (++g_demoFalseFrames >= kDemoDebounceFrames) {
		g_demoLoadedDebounced = false;
		g_demoFalseFrames = 0;
	}
}

} // namespace

void Frame_Render() {
	const bool firstRender = (g_renderCount.fetch_add(1) == 0);
	if (firstRender) DL_BOOT("render pump: first frame");

	// No Panorama work here, ever: V8 lives on the main thread, so a RunScript call from this
	// thread is a crash rather than a race. Frame capture belongs here and only here -- the
	// finished back buffer exists nowhere else.
	Filmmaker::Render_RunRenderFrame(PresentHook_SwapChain());
	if (firstRender) DL_BOOT("render pump: first frame complete");
}

void Frame_MainThread() {
	const bool firstMain = (g_mainCount.fetch_add(1) == 0);
	if (firstMain) DL_BOOT("main pump: first frame");

	// Synchronize project state before any queued edit can act on the previous replay.
	// The debounce preserves transient seek gaps; a different filename switches immediately.
	UpdateDemoDebounce();
	const bool replayChanged = Filmmaker::Shot_UpdateReplay(g_demoLoadedDebounced);
	CameraRuntime_RunFrame(replayChanged ? false : g_demoLoadedDebounced);

	// Console commands arriving over netcon can only be executed here.
	Netcon_PumpMainThread();
	// Panorama button presses, drained BEFORE the workspace renders so a click and the state it
	// produces land in the same frame rather than a frame apart.
	UiCommandBridge_PumpMainThread();
	if (firstMain) DL_BOOT("main pump: netcon pumped");

	TrySchema();
	Input_RunFrame();
	CameraCommands_RunFrame(g_demoLoadedDebounced);
	MirvCommands_RunFrame();
	Filmmaker::DeadlockSettings_RunFrame();
	if (firstMain) DL_BOOT("main pump: demo debounce ok");

	if (!g_uiDisabled) {
		// Drops a stale blackout (the replay it was turned on in already ended) BEFORE the hotkey
		// that can set a fresh one and the workspace pump that reads it, so both see the same
		// answer this frame rather than one seeing last frame's.
		HudBlackout_RunFrame();
		// AFTER Input_RunFrame, whose focus sampling this reads, and BEFORE the workspace renders,
		// so a shortcut and the state feed it changes land in the same frame rather than a frame
		// apart -- the same ordering a Panorama button gets through the command bridge.
		Hotkeys_RunMainThreadFrame(g_demoLoadedDebounced, Filmmaker::Workspaces_Open());
		Filmmaker::Workspaces_RunMainThreadFrame(g_demoLoadedDebounced);
		// Live JS runs AFTER the workspace, so a hot-reloaded script sees the panels it is editing
		// already built and already rendered this frame. Cheap no-op when nothing is watched.
		Filmmaker::UiLive_PumpMainThread();
	}
	if (firstMain) DL_BOOT("main pump: first frame complete");
}

void Frame_SetUiDisabled(bool disabled) { g_uiDisabled = disabled; }
bool Frame_UiDisabled() { return g_uiDisabled; }

unsigned long long Frame_RenderCount() { return g_renderCount.load(); }
unsigned long long Frame_MainCount() { return g_mainCount.load(); }

} // namespace Deadlock
