#pragma once

// Keeps CS2 rendering at full rate while its window is unfocused.
//
// The engine sleeps `engine_no_focus_sleep` milliseconds per frame whenever the window loses
// focus (stock default throttles hard enough to read as jitter/lag the moment you alt-tab).
// SOURCE:MVM is an offline movie tool whose normal workflow has OTHER windows focused -- editors,
// encoders, netcon automation -- while the game keeps playing and rendering, so the throttle is
// forced to 0 once at startup. The tools-mode launcher already does this from the outside; this
// makes every launch path behave the same from inside the hook.
//
// One-shot with retry: ticks from the main thread until the cvar resolves, writes 0, then goes
// inert. A user (or the recorder's own save/restore) changing the cvar later is left alone.

namespace Filmmaker {

void UnfocusedRate_MainThreadTick();

} // namespace Filmmaker
