#pragma once

namespace advancedfx { class ICommandArgs; }

namespace Filmmaker {

// Experimental client-clock driver for the Customize player preview. It does NOT currently animate
// the paused preview and must not be presented as the replacement for `previewnudge`.
//
// Measured 2026-07-29 (see Docs/panorama/panorama-player-preview-summary.md): the preview pawn
// (`csgo_previewplayer`) is frozen while the demo tick is fixed, but writing client globals
// `curtime` / `frametime` at both available frame stages still produced a bit-identical pose.
// The engine restores its demo-derived time before the preview's AG2 evaluation.
//
// The real solution must advance the preview pawn's CNmGraphInstance through the engine's normal
// main-thread evaluation path with a wall-clock delta. The current live chain is documented as:
// preview pawn + 0x34F0 -> CCS2UIPawnGraphController; body + 0x960 -> CNmGraphInstance. The ABI and
// stable resolver for that call still need to be proven before this module can replace the nudge.
//
// Scope warning: the client globals are GLOBAL. Advancing them lets other client-side animation
// (world pawn animgraphs) keep idling in place while paused too. Networked state stays frozen, so
// nothing moves through the world and the demo tick does not drift -- but this is deliberately
// opt-in and off by default rather than always-on, so scrubbing/gizmo work is never affected
// unless the preview is actually being used.
//
// Standalone module (CLAUDE.md: never couple a new thing into an existing thing) -- own command
// (`mvm_previewclock`), own pump, own state.

// Main/UI thread, once per frame from Filmmaker::RunMainThreadFrame. No-op unless enabled.
//
// NOTE (measured 2026-07-29): a write at THIS stage does not survive. The injected value lands (the
// read-back shows it) but every following frame reports the same pre-write curtime, i.e. the client
// re-applies its own value from the demo clock afterwards -- 1152 frames and 28.7s injected here
// produced zero pose change. This is the identical failure mode documented on
// Filmmaker::RunMainThreadLateFrame for the mesh-group mask. Hence the late stage below, which is
// the default; this entry point is kept only so the two can still be A/B'd from the console.
void PreviewAnimClock_RunFrame();

// Main thread, from FrameStageNotify AFTER the engine's own handler -- as late as we can get before
// the draw, so our curtime/frametime are the last values written before bone setup reads them.
void PreviewAnimClock_RunLateFrame();

void PreviewAnimClock_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd);

// Enabled state for explicit diagnostics only. The preview UI must not treat this as a working
// substitute for `cosmetics previewnudge`.
bool PreviewAnimClock_Enabled();
void PreviewAnimClock_SetEnabled(bool enabled);

} // namespace Filmmaker
