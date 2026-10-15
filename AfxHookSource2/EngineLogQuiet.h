#pragma once

// Silences specific engine logging channels that spam the console with per-frame warnings the
// user cannot act on (e.g. RenderSystem "Attempting to render without calling BindRenderTargets!"
// and SceneSystem "Failed to allocate scratch rendertarget for game.waterefects..." after the
// window is resized to a non-native size). The console command `log_verbosity` is LOCKED in the
// retail client ("Log verbosity levels are locked."), so this goes through tier0's exported
// LoggingSystem API directly, which the lock does not cover.
//
// Main thread, called from the RunMainThreadFrame pump. Retries until tier0 and the target
// channels exist, then latches. Errors on the silenced channels still print (verbosity OFF only
// gates below-error spew per Source 2's response policy for do-not-echo; the channels chosen are
// diagnostic-noise channels).

namespace Filmmaker {

void EngineLogQuiet_RunMainThreadFrame();

} // namespace Filmmaker
