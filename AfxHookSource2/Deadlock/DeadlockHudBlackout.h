#pragma once

// HUD BLACKOUT (F8): one flag that clears the screen of every overlay, ours and the game's.
//
// Ported from CS2's Filmmaker::HudBlackout (AfxHookSource2/Filmmaker/Movie/HudBlackout.h) with one
// deliberate difference: Deadlock exposes no `crosshair` convar (a signature sweep for it during
// this port returned zero hits), so there is nothing to save and restore there. The crosshair is a
// gameplay HUD child like any other, so it disappears with the rest of the pass
// DeadlockNativeHudJs.h already runs to clear the native HUD -- this flag now also drives that pass.
//
// A RENDER-TIME FLAG, NOT A SHUTDOWN. Nothing about the editor's own state -- the open workspace,
// the selected marker, an in-progress reposition -- is touched, and this flag does not close
// anything. Only what gets DRAWN changes: our own workspace chrome, the game's own gameplay HUD and
// replay bar, and the scaled world preview (which is released back to full screen so blackout is
// not a small picture surrounded by black). A second press puts the screen back exactly as it was.

namespace advancedfx { class ICommandArgs; }

namespace Deadlock {

bool HudBlackout_Active();
void HudBlackout_Set(bool on);
void HudBlackout_Toggle();

// Main thread, once per frame: drops the blackout when the replay it was turned on in ends (or was
// never loaded), so a blackout can never persist into the dashboard with no visible way to lift it.
void HudBlackout_RunFrame();

// `mirv_filmmaker blackout [on|off|toggle|status]`.
void HudBlackout_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd);

} // namespace Deadlock
