#pragma once

namespace advancedfx { class ICommandArgs; }

namespace Filmmaker {

// Drives the Customize player preview's animation while the demo is PAUSED, by running the client's
// own animgraph tick for the preview pawns only.
//
// Why this module exists (see Docs/panorama/panorama-player-preview-summary.md for the full history):
//
//  * The preview character is a client-only `csgo_previewplayer` pawn living in the `ui/buy_menu`
//    spawn group that `MapPlayerPreviewPanel` loads. Its pose comes from an AG2 graph
//    (`CNmGraphInstance`) that is evaluated once per CLIENT TICK, not per rendered frame. Measured:
//    300 samples over ~1200 render frames at a pinned demo tick produced one bit-identical pose.
//  * Writing the client globals (`curtime` / `frametime`) from our frame pumps does NOT work: the
//    client re-applies its demo-derived time after both available main-thread stages, so the value
//    never survives to a consumer. That is `PreviewAnimClock`, kept only as a diagnostic.
//
// The mechanism here is different in the one way that matters: instead of writing a clock and hoping
// something reads it, we write the clock and then CALL THE CONSUMER OURSELVES, synchronously, in the
// same instant. Nothing can re-apply the demo time in between.
//
// The consumer is `CAnimGraphGameSystem::AnimTickUpdate(this, mode)` in client.dll. It builds the
// per-tick animgraph update list, dispatches the "UpdateAnimGraph2Animations" jobs, and joins them.
//
// SCOPE IS THE WHOLE POINT. Running an unfiltered client anim tick while paused would keep every
// world pawn's idle/aim layers playing in place, which breaks the "paused means frozen" contract
// that scrubbing and frame-accurate rendering depend on. So the update list is filtered, inside the
// engine's own list builder, down to the preview pawns before anything is evaluated. If the filter
// cannot be installed, the forced tick refuses to run at all (fail closed) -- an animated preview is
// never worth a moving world.
//
// Standalone module (CLAUDE.md: never couple a new thing into an existing thing) -- own resolver,
// own detours, own command (`mvm_previewtick`), own pump, own state.

// Main/UI thread, once per frame from Filmmaker::RunMainThreadFrame. `customizeModalOpen` is the
// arming signal in the default `auto` mode -- the preview only animates while the modal that shows
// it is actually on screen, so nothing in the process pays for this the rest of the time. No-op
// unless armed AND the demo is paused AND at least one preview pawn is present.
void PreviewAnimTick_RunFrame(bool customizeModalOpen);

void PreviewAnimTick_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd);

// Currently arming (mode + modal state resolved). Diagnostics only.
bool PreviewAnimTick_Enabled();

// True once the client.dll resolution + detours succeeded. Used by `mirv_filmmaker health`-style
// reporting and by the UI to decide whether the legacy nudge fallback is still needed.
bool PreviewAnimTick_Available();

} // namespace Filmmaker
