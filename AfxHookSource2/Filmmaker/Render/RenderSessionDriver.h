#pragma once

// Executes one render job: the per-frame state-machine driver behind
// `mirv_filmmaker render start`.
//
// WHY THIS SHAPE. The job rides the game's own main-thread frame loop (plan section 18): the
// recorder's host_framerate override makes the engine step a fixed 1/fps of demo time per
// rendered frame, so "advance deterministic source time" is the engine loop itself and this
// driver only has to (a) put the world in the right place before capture starts, (b) start and
// stop the recorder on exact tick boundaries, and (c) put everything back afterwards. There is
// no worker thread: every step here runs inside Filmmaker::RunMainThreadFrame, which is also
// what makes "one pump call == one output frame while capturing" true and lets frames.csv be
// written with exact frame->tick mapping.
//
// The authoritative phase lives in RenderSession (the legal-transition machine); this driver is
// the only writer that walks it through a job. Cancellation is cooperative: the cancel command
// only raises the session flag, and this driver notices at its per-frame checkpoints.
//
// State touched during a job and restored on EVERY exit (success, cancel, failure):
//   * RenderStateRestoreGuard categories (camera ownership/pose, effect modules, tracked cvars),
//   * recorder knobs (record name, screen-record enable, settings preset, fps override,
//     cam export, TGA/BMP format, the job's capture streams),
//   * the editor's game-HUD visibility mode,
//   * the render-resolution override,
//   * demo position and paused state.

#include <string>

namespace Filmmaker {

struct RenderProgress {
	int curTick = 0;
	int framesDone = 0;        // LOGICAL output frames: the same count in every stream
	int framesTotal = 0;
	double percent = 0.0;      // 0..100 over the capture range
	int passes = 0;            // output streams (one per pass)
	int rendersPerFrame = 0;   // physical renders per logical frame
	long long physicalDone = 0;
	long long physicalTotal = 0;
	std::string phase;         // short human phase word for the UI
	std::string lastError;     // last failure reason ("" when none)
	std::string takeDirUtf8;   // output directory of the current/last job
};

// Starts a job from the CURRENT live workspace settings. Returns false with outError set when
// the job cannot start (validation failure, busy session, no demo, unresolvable range...).
// On success the session machine leaves Idle and the per-frame driver takes over.
bool RenderSessionDriver_Start(std::string& outError);

// Main thread, once per frame (Filmmaker::RunMainThreadFrame). Cheap no-op while idle.
void RenderSessionDriver_RunFrame();

// True while a job is between start and its terminal state.
bool RenderSessionDriver_IsActive();

const RenderProgress& RenderSessionDriver_Progress();

// True while captured frames could include Panorama UI (before-present capture): the JS reads
// this through the state bridge and hides every Source MVM panel so editor chrome is never
// baked into a render.
bool RenderSessionDriver_WantsChromeHidden();

} // namespace Filmmaker
