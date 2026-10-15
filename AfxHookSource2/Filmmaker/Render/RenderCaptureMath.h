#pragma once

// Shared capture-rate clamp for CS2 and Deadlock render jobs.
//
// Locked recording semantics:
//   * `fps` is both OUTPUT / mux FPS and the demo sampling rate (`host_framerate`).
//   * Choosing FPS and starting a take is enough — the timeline speed dial is NOT part of
//     render math. During the job, `demo_timescale` is forced to 1 so preview dial speed
//     cannot stretch or compress the capture clock.
//   * Watching the file at 1x plays real-time motion at the chosen frame density (high FPS
//     = more samples per demo-second for smooth scrub / high-fps playback).
//
// Engine-free; unit-tested in Automation/tests/render-capture-math-tests.cpp.

namespace Filmmaker {

inline int RenderCaptureFps(int fps) {
	if (fps < 1) return 1;
	if (fps > 1000000) return 1000000;
	return fps;
}

} // namespace Filmmaker
