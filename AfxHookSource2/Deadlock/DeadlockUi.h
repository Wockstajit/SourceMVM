#pragma once

// Who owns the mouse this frame.
//
// The CS2 hook answers this by ORing seven named predicates together inside MirvInputEx, one per
// panel that can want the cursor. That works, but it means every new panel has to remember to add
// itself to a predicate list in a different file, and the failure when it forgets is subtle: the
// panel's sliders work and its buttons are dead, because MirvInput consumes the click before
// Panorama sees it and only the slider path reads our own cursor probe.
//
// Here panels REGISTER instead. A panel claims the cursor while it is open and releases it when
// it closes, and the camera runtime asks one question. Forgetting to register is then a panel that
// never gets the mouse at all, which is obvious, rather than a panel whose buttons half-work.

#include <cstddef>

namespace Deadlock {

// A stable id per panel. Adding one here is the only step needed to make a panel cursor-aware.
enum class UiPanel {
	CameraEditor,
	CameraTimeline,
	GraphEditor,
	MarkerMenu,
	Config,
	Render,
	Count,
};

// Claim or release the OS cursor for a panel. Idempotent.
void Ui_SetPanelWantsCursor(UiPanel panel, bool wants);

bool Ui_PanelWantsCursor(UiPanel panel);

// True when any registered panel is claiming the cursor.
bool Ui_AnyPanelWantsCursor();

// True while Deadlock's own console overlay is up. The camera must stand down for it, or typing
// into the console flies the camera around.
bool Ui_ConsoleVisible();

// Panorama HUD panels need the game to stop swallowing input as well as MirvInput standing down.
// Without this a panel's sliders respond and its buttons do not -- the recorded tell for this
// exact mistake. Emits the AddDenyInputFlagsToGame / RemoveDenyInputFlagsToGame pair through
// RunScript, once per transition rather than per frame.
void Ui_SetGameInputDenied(bool denied);

bool Ui_GameInputDenied();

// The answer to "does UiToolkitAPI exist here", reported back by the probe through the command
// bridge. Deadlock's HUD context has no engine bindings at all, so this is expected to be false --
// but it is measured rather than assumed, and `health` says which it was.
void Ui_SetToolkitAvailable(bool available);

// A one-line summary for `mirv_filmmaker health`.
void Ui_Report();

} // namespace Deadlock
