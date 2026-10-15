#include "DeadlockHotkeys.h"

#include "DeadlockCommands.h"
#include "DeadlockEngine.h"
#include "DeadlockHudBlackout.h"
#include "DeadlockInput.h"
#include "DeadlockUi.h"

#include "../Filmmaker/Debug/MvmDebugLog.h"
#include "../../Shared/AfxConsole.h"

#include <windows.h>

namespace Deadlock {

namespace {

// One route for every shortcut, and it is the one the UI buttons use: ours first, the engine's
// second. See DeadlockUiCommandBridge::PumpMainThread, which does exactly this.
void Run(const char* line) {
	if (!Commands_DispatchLine(line))
		Engine_ExecuteClientCmd(line);
}

// PLAY/PAUSE IS STATE-AWARE, because Deadlock has no toggle.
//
// `demo_pause` only pauses here. Measured: paused=yes, send `demo_pause`, still paused=yes; send
// `demo_resume` and the tick advances again. Binding SPACE straight to `demo_pause` therefore gave
// exactly the reported behaviour -- "you can press space to pause, but you can't press space to
// unpause". CS2's `demo_pause` is a toggle, which is where the assumption came from.
const char* PlayPauseCommand() {
	return Engine_IsDemoPaused() ? "demo_resume" : "demo_pause";
}

struct Binding {
	int vk;
	const char* command;              // fixed line, or null when the action decides
	const char* (*resolve)();         // asked at press time when the line depends on state
	bool standsDownForFreeCam;        // see the header: left/right are MirvInput's yaw
	// CS2's own OnKey (MovieMode.cpp) proves marker/transport hotkeys need no editor panel open:
	// its only blanket precondition is IsDemoActive(), and K is commented "any camera mode". But
	// Deadlock is not CS2 -- its NATIVE spectator HUD binds SPACE to "Change Team" and the arrow
	// keys to its own scrubber (both visible in CitadelHudReplay's own on-screen shortcut list),
	// and this port polls rather than intercepts (DeadlockHotkeys.h's own "what this cannot do"),
	// so our command always runs ALONGSIDE the game's, never instead of it. Firing demo_pause or a
	// ±10s seek as a surprise side effect of switching teams while just spectating -- with none of
	// our UI on screen to explain why -- is a real collision, not a hypothetical one. So only the
	// keys with NO such native collision (K, F8) drop the workspace-open requirement; SPACE and the
	// arrows keep it.
	bool requiresWorkspace;
	const char* name;
	bool down;
	unsigned long long fired;
	unsigned long long lastMs;
};

// A SHORTCUT CANNOT FIRE TWICE INSIDE THIS. Measured while testing: one press of SPACE sometimes
// produced two fires a few frames apart -- `demo_resume` immediately followed by `demo_pause`, so
// the replay ended up exactly where it started and the key looked dead. A keyboard that bounces, a
// press that arrives while the window is taking focus, and an auto-repeat that starts early all
// produce the same extra edge, and none of them is a second intent.
//
// Short enough that deliberately tapping play and pause still works: at 250ms that is four presses
// a second.
const unsigned long long kBounceMs = 250;

Binding g_bindings[] = {
	// Place a camera at the free cam. The same subcommand the PATH card's + ADD button runs. No
	// workspace-open requirement (see Binding::requiresWorkspace): Deadlock's native HUD has no
	// competing use for K, so this is safe standalone the same way CS2's own K is.
	{ 'K',      "mirv_filmmaker marker add",      nullptr,           false, false, "K place",       false, 0, 0 },
	// F selects (and goes to) the marker under the crosshair, L deletes it -- CS2's OnKey aimed
	// hotkeys. Both no-op unless a marker is actually aimed at (DeadlockShot.cpp hover), so they
	// cannot collide with anything Deadlock's replay HUD does on those keys.
	{ 'F',      "mirv_filmmaker marker hovergoto",   nullptr,        false, false, "F select aimed", false, 0, 0 },
	{ 'L',      "mirv_filmmaker marker hoverdelete", nullptr,        false, false, "L delete aimed", false, 0, 0 },
	{ VK_SPACE, nullptr,                         &PlayPauseCommand, false, true,  "Space play",    false, 0, 0 },
	{ VK_LEFT,  "mirv_skip time -10",            nullptr,           true,  true,  "Left skip -10", false, 0, 0 },
	{ VK_RIGHT, "mirv_skip time 10",             nullptr,           true,  true,  "Right skip 10", false, 0, 0 },
	// F8: HUD BLACKOUT. No workspace-open requirement -- its whole point is a clean screen to WATCH
	// the replay with, often precisely because the tool is closed, and it has no native collision
	// either (Deadlock binds no gameplay action to F8).
	{ VK_F8,    "mirv_filmmaker blackout toggle", nullptr,           false, false, "F8 blackout",   false, 0, 0 },
};
const int kBindingCount = (int)(sizeof(g_bindings) / sizeof(g_bindings[0]));

bool g_active = false;
unsigned long long g_frames = 0;

} // namespace

void Hotkeys_RunMainThreadFrame(bool demoLoaded, bool workspaceOpen) {
	++g_frames;

	// EDGE DETECTION RUNS EVEN WHILE GATED OFF, and releasing the gate must not fire anything.
	//
	// If the down-state were only sampled while active, a key held across the gate opening would
	// read as a fresh press on the first active frame. Opening the workspace with a finger on SPACE
	// would toggle the pause; releasing it and pressing again would be needed to get back. So the
	// state is always tracked and the GATE decides whether a transition does anything.
	//
	// FOCUS IS PART OF THE GATE. GetAsyncKeyState is global: it reports the physical key whoever
	// pressed it and whatever window has the foreground. Without this, typing in another
	// application drives the replay -- and it is not a subtle failure, because a key held anywhere
	// reads as down every frame.
	const bool baseGate = Input_Focused() && demoLoaded && !Ui_ConsoleVisible();
	g_active = baseGate;

	const unsigned long long now = GetTickCount64();

	// The free camera's own keys. Asked once: it reads the cursor mode and the console, and asking
	// per binding would let the answer change inside one frame's worth of presses.
	const bool freeCamOwnsInput = Input_OwnsCamera();

	for (int i = 0; i < kBindingCount; ++i) {
		Binding& b = g_bindings[i];
		const bool down = (GetAsyncKeyState(b.vk) & 0x8000) != 0;
		const bool pressed = down && !b.down;
		b.down = down;

		const bool gate = baseGate && (!b.requiresWorkspace || workspaceOpen);
		if (!pressed || !gate) continue;
		if (b.standsDownForFreeCam && freeCamOwnsInput) continue;
		if (b.lastMs && now - b.lastMs < kBounceMs) continue;

		const char* line = b.resolve ? b.resolve() : b.command;
		if (!line) continue;

		b.lastMs = now;
		++b.fired;
		if (Filmmaker::MvmDebugLog_Active())
			Filmmaker::MvmDebugLog_LinefAlways("deadlock.hotkey", "%s -> '%s'", b.name, line);
		Run(line);
	}
}

void Hotkeys_Report() {
	advancedfx::Message(
		"  hotkeys:   %s  (focused + a replay loaded + console hidden; SPACE/Left/Right also need\n"
		"             the workspace open -- K and F8 do not, see DeadlockHotkeys.cpp)\n",
		g_active ? "armed" : "standing down");
	for (int i = 0; i < kBindingCount; ++i) {
		const Binding& b = g_bindings[i];
		const char* line = b.resolve ? b.resolve() : b.command;
		advancedfx::Message("             %-14s -> %-26s fired=%llu%s%s\n",
		                    b.name, line ? line : "(nothing right now)", b.fired,
		                    b.requiresWorkspace ? "  (workspace must be open)" : "  (works anywhere)",
		                    b.standsDownForFreeCam ? "  (stands down while the free cam has input)" : "");
	}
	if (!g_active)
		advancedfx::Message(
			"             nothing fires while this says standing down -- that is the gate, not a\n"
			"             missed key. Focus the game with a replay loaded and the console hidden.\n");
}

} // namespace Deadlock
