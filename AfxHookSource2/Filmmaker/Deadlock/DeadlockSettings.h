#pragma once

// Global Deadlock filmmaker preferences. Demo-authored data (markers, path timing and loop
// endpoints) remains in its per-demo sidecars; this store owns the controls that should survive
// restarting the game.

namespace Filmmaker {

// Main-thread pump. Loads once, then writes a changed snapshot after a short debounce.
void DeadlockSettings_RunFrame();

} // namespace Filmmaker
