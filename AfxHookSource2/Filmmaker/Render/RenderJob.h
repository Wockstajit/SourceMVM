#pragma once

// One render job, snapshotted at start time and immutable for the job's whole life.
//
// WHY THIS EXISTS (plan section 2 / contract rule 1). The live workspace settings
// (RenderCommand's range/camera statics, RenderOutputSettingsRef, RenderPassesRef) keep changing
// while the user clicks around. A running job must NOT follow them: the session driver copies
// everything it needs into this struct when the job starts, resolves the range to concrete
// ticks once, and only ever reads the copy afterwards. That is what makes "the pipeline never
// re-derives bounds mid-job" a structural property instead of a discipline.

#include "RenderTypes.h"
#include "RenderRange.h"
#include "RenderOutput.h"

#include <string>

namespace Filmmaker {

struct RenderJob {
	// RESOLVED range: mode already applied, ticks concrete, Valid() true. The pipeline reads
	// only startTick / endTickExclusive from here (contract rule 1).
	RenderRange range;

	RenderCameraSource camera = RenderCameraSource::CurrentViewer;
	CameraPathExtension extension = CameraPathExtension::Fail;

	// Copies of the live settings at start time.
	RenderOutputSettings out;
	RenderPasses passes;

	// Pre-roll: seek lands at max(demoStart, startTick - warmupTicks) and plays hidden up to
	// startTick so particles/materials/animation settle before the first captured frame.
	int warmupTicks = 64;
	// Frames to wait after the pre-roll seek before resuming playback (a demo_gototick is an
	// asynchronous multi-100ms stall; capturing during it would record the stall).
	int settleFrames = 30;

	double tickRate = 64.0;

	// Resolved output directory for this job (BuildTakeDirectory result), and the recorder
	// settings preset registered for its beauty stream.
	std::wstring takeDir;
	std::string presetName;

	// Game speed recorded for the take. Always 1.0: the timeline dial is preview-only and is
	// forced off for the job so it cannot stretch the capture clock.
	double timescale = 1.0;
	// Sampling rate = output fps (clamped). host_framerate, frame count, and path evaluation
	// all ride this — choose FPS, hit render; no dial required.
	int captureFps = 60;
	// range.FrameCount(captureFps, tickRate), cached for progress math.
	int framesTotal = 0;

};

} // namespace Filmmaker
