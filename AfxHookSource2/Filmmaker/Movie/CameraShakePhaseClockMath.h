#pragma once

#include <cmath>

namespace Filmmaker {

// A continuous phase clock for oscillators that are driven by the demo clock during INTERACTIVE
// playback.
//
// The problem it solves. The view-setup demo clock is not smooth frame to frame: two render
// frames can share a tick (dt = 0), consecutive frames report 7.3 ms then 8.5 ms then 0 for the
// same wall interval, and it routinely steps backwards by a few ms (sub-tick interpolation). An
// oscillator that integrates that clock directly renders a staircase -- the Weapon Bob jitter.
//
// The first attempt subdivided each tick speculatively and issued "the remainder" when the tick
// landed. Measured against a real log (2026-09-19, 112 fps, dt spread 7.3-8.5 ms with 8% zeros)
// that only moved the stall: the speculative step often exceeded the tick's dt, so the remainder
// clamped to zero and the tick frame froze instead of the held one.
//
// This is a phase-locked loop instead. Every frame advances by wall time scaled by the observed
// playback rate, which is smooth because the wall clock is smooth. The running LEAD (phase issued
// minus demo time elapsed) is paid back a small fraction per frame, so over any window longer
// than a few frames the phase tracks demo time exactly, while no single frame ever sees the
// demo clock's own steps.
//
// Two things must still stop it dead.
//
// A PAUSED demo holds the shake pose -- that is the stack's contract, and a loop that only ever
// chases wall time would keep bobbing through a pause. A pause looks exactly like a same-tick
// hold from one frame's view (both report dt = 0), so the discriminator is elapsed wall time
// since the last real demo step, measured against how long a step normally takes. Playback at
// 0.1x is not a stall, which is why the window is learned rather than fixed.
//
// A CAPTURE must not depend on wall time at all, so the caller passes `locked` while the
// recorder is active: the step is then the demo dt exactly and the loop state is cleared. The
// recorder steps the demo clock once per output frame, so dt is never 0 there anyway.
struct PhaseClockState {
	double lead = 0;             // phase seconds issued minus demo seconds elapsed; + = ahead
	double rate = 1;             // demo seconds per wall second, smoothed; 1 at normal speed
	double wallSinceTick = 0;    // wall seconds since the last real demo step
	double tickWallInterval = 0; // smoothed wall seconds between real demo steps
};

// Bounds. A single bad sample must not run the phase away.
inline constexpr double kPhaseClockMaxStep = 0.05;  // seconds, ~3 ticks at 64 Hz
inline constexpr double kPhaseClockMaxLead = 0.05;  // seconds
inline constexpr double kPhaseClockMaxRate = 8.0;
// Rate learning is slow on purpose: the wall time a tick spans alternates between one and two
// frames, so a per-tick estimate is noisy and the lead correction covers the transient anyway.
inline constexpr double kPhaseClockRateBlend = 0.1;
// Fraction of the lead removed each frame. 0.1 keeps a +/-8 ms lead swing (one held frame at
// 64 Hz) to under a millisecond of step variation while still converging within ~20 frames.
inline constexpr double kPhaseClockCorrection = 0.1;
// The demo is considered stalled once this many normal step intervals pass with no demo time.
// Four rather than two: at high frame rates the interval estimate is noisy, and briefly
// over-running a stall is invisible while falsely declaring one freezes a moving gun.
inline constexpr double kPhaseClockStallIntervals = 4.0;
// The stall window never shrinks below this, so a fast demo cannot make the test hair-trigger.
inline constexpr double kPhaseClockMinStallWindow = 0.05; // seconds

inline void PhaseClockReset(PhaseClockState &s) {
	s.lead = 0;
	s.wallSinceTick = 0;
	// Rate and the tick interval are properties of the playback speed, not of the tracked
	// target, so a seek or a spectator switch has no reason to relearn them.
}

// True once enough wall time has passed with no demo step that the demo must be paused or
// stalled rather than merely between ticks.
inline bool PhaseClockStalled(const PhaseClockState &s) {
	double window = s.tickWallInterval * kPhaseClockStallIntervals;
	if (!(window > kPhaseClockMinStallWindow)) window = kPhaseClockMinStallWindow;
	return s.wallSinceTick > window;
}

// The phase-time step this frame should advance. `demoDt` is ShakeInput::dt (0 on a same-tick
// hold), `wallDt` is ShakeInput::wallDt, `locked` means the recorder owns the clock.
inline double PhaseClockStep(PhaseClockState &s, double demoDt, double wallDt, bool locked = false) {
	if (!(demoDt > 0)) demoDt = 0;
	if (!(wallDt > 0)) wallDt = 0;
	if (locked) {
		s.lead = 0;
		s.wallSinceTick = 0;
		return demoDt;
	}
	s.wallSinceTick += wallDt;
	if (demoDt > 0) {
		if (s.wallSinceTick > 1e-6) {
			double observed = demoDt / s.wallSinceTick;
			if (observed > kPhaseClockMaxRate) observed = kPhaseClockMaxRate;
			s.rate += (observed - s.rate) * kPhaseClockRateBlend;
			if (s.tickWallInterval > 0)
				s.tickWallInterval += (s.wallSinceTick - s.tickWallInterval) * kPhaseClockRateBlend;
			else
				s.tickWallInterval = s.wallSinceTick;
		}
		s.wallSinceTick = 0;
		s.lead -= demoDt; // the demo moved on; the phase is now this much further behind
	} else if (PhaseClockStalled(s)) {
		// Paused, loading or alt-tabbed. Hold the pose and leave the lead where it is, so
		// playback resumes from the phase it stopped at.
		return 0;
	}
	if (s.lead < -kPhaseClockMaxLead) s.lead = -kPhaseClockMaxLead;
	double step = wallDt * s.rate - s.lead * kPhaseClockCorrection;
	if (step < 0) step = 0;
	if (step > kPhaseClockMaxStep) step = kPhaseClockMaxStep;
	s.lead += step;
	if (s.lead > kPhaseClockMaxLead) s.lead = kPhaseClockMaxLead;
	return step;
}

} // namespace Filmmaker
