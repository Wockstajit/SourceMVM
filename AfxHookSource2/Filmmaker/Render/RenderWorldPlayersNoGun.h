#pragma once

// World + Players / No Gun capture mode.
//
// This is intentionally smaller than World Only: it does not classify scene draws and it does
// not touch players, world weapons, materials, or lighting. It only keeps CS2's first-person
// viewmodel disabled for the lifetime of the selected render/live preview.

namespace Filmmaker {

// Main thread. Reconciles the desired state with CS2's r_drawviewmodel cvar and the existing
// first-person particle-effect suppression gate. Safe to call every frame; writes only when the
// state changes, with a small reassertion cadence for POV/player changes that reset the cvar.
void RenderWorldPlayersNoGun_RunMainThreadFrame();

// Main thread. `reason` is recorded in mvm_debug so a mode left armed after a failed job is
// traceable to its owner.
void RenderWorldPlayersNoGun_SetArmed(bool armed, const char* reason);
bool RenderWorldPlayersNoGun_IsArmed();

} // namespace Filmmaker
