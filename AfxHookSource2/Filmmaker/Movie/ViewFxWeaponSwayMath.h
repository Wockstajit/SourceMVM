#pragma once

// Weapon sway -- pure math, no engine dependencies so it can be unit tested offline.
// `ViewFxWeaponSway.{h,cpp}` is the thin plumbing that owns the knob and calls into here.
//
// The effect is a slow movement-gated drift on the viewmodel: a Lissajous figure whose PHASE runs
// on a free-running wall clock (so it behaves identically whether the demo is playing or paused)
// and whose AMPLITUDE is gated by the stack's last tracked planar speed. A stationary player's
// weapon sits perfectly still.
//
// ---------------------------------------------------------------------------------------------
// WHY THIS IS A SEPARATE UNIT: the jitter.
//
// The shipped version multiplied the raw sine straight by the movement fraction:
//
//     out.x = sin(t * 0.9) * idleAmp * speedFrac;
//
// `speedFrac` is derived from `ViewFx`'s shared planar-speed estimator, and that estimator does
// not produce a continuous signal. It STEPS, for four unrelated reasons, and because the term
// multiplies a POSITION every step is a visible jump of the gun:
//
//   1. `ViewFx::ResetTracker` zeroes the speed outright on a seek, a spectator switch, a big
//      clock gap, or a sustained teleport streak. Amplitude goes to zero in one frame and then
//      climbs back over the estimator's own half-time.
//   2. The tracker is skipped entirely on any frame another camera system owns the pose
//      (`NoteGatedByOverride`), so the estimate freezes and resumes without warning.
//   3. The staleness gate faded the amplitude out over 0.20 s once updates stopped, then restored
//      it AT FULL SIZE on the first frame an update returned. The fade-out was smooth; the
//      recovery was a step.
//   4. That same gate used FIXED WALL-CLOCK thresholds (stale after 0.30 s) against a signal
//      whose cadence is one DEMO TICK. Under slow-motion playback -- an ordinary thing to do in a
//      filmmaking tool -- a tick is hundreds of wall-milliseconds apart, so the gate declared the
//      stream stale between every pair of ticks: fade out, snap back, fade out, once per tick.
//      That is the periodic "jitter that sometimes happens" and nothing in the sway settings
//      could reach it.
//
// So the amplitude is now a SMOOTHED state with its own frame-rate-independent half-time, driven
// by a target rather than applied raw. Every one of the four discontinuities above becomes a
// short fade in the only place that can actually see them. And the staleness threshold is derived
// from the observed update cadence instead of a wall constant, so it means "this stream stopped"
// at any playback speed rather than "this stream is slower than 64 Hz".
//
// The phase was already integrated (`m_phase += dt * rate`) and then THROWN AWAY -- the sines read
// raw wall-clock `t`. The integral is now the thing that drives them, and it FREEZES while the
// amplitude is gated out, so resuming continues the figure instead of teleporting to wherever a
// free-running clock had wandered. Visible rates are unchanged (0.9 and 0.6 rad/s), so this is a
// jitter fix and not a re-tune: the movement-scaled cadence the old dead code implied was never
// shipped and is not introduced here -- this effect is the slow drift its UI advertises, and
// `WeaponBob` is the separate module that owns footstep cadence.

#include <cmath>

namespace Filmmaker {

// The drift figure. x and y run at 3:2, which is what makes it a figure rather than a line.
constexpr double kViewFxSwayBaseAmp = 0.40;   // viewmodel units at 100% intensity, full speed
constexpr double kViewFxSwayRateX = 0.9;      // radians/sec
constexpr double kViewFxSwayRateY = 0.6;      // radians/sec
constexpr double kViewFxSwayPhaseY = 1.3;     // radians, y's phase offset
constexpr double kViewFxSwayAmpY = 0.6;       // y amplitude relative to x

// Micro eye-origin jitter (breathing / lean animations) must not read as walking.
constexpr double kViewFxSwaySpeedDeadband = 15.0;  // units/sec
constexpr double kViewFxSwaySpeedRef = 250.0;      // units/sec; CS2 run speed

// A wall-clock gap bigger than this is a fresh start, not a frame.
constexpr double kViewFxSwayResetGap = 0.25;   // seconds
constexpr double kViewFxSwayMaxStep = 0.10;    // seconds; a hitch cannot leap the phase

// Amplitude smoothing half-time. Long enough that a tracker reset or a gated frame reads as a
// fade instead of a jump; short enough that breaking into a run reaches full sway in about half
// a second. This is the single knob that turns all four discontinuities above into non-events.
constexpr double kViewFxSwayAmpHalfTime = 0.18; // seconds

// Staleness, expressed in MULTIPLES of the stream's own observed update interval rather than in
// wall seconds -- see the header comment. The bounds stop a single anomalous gap from either
// disarming the gate (max) or arming it during normal playback (min).
constexpr double kViewFxSwayStaleIntervals = 4.0;
constexpr double kViewFxSwayStaleMin = 0.12;   // seconds
constexpr double kViewFxSwayStaleMax = 2.00;   // seconds
constexpr double kViewFxSwayIntervalHalfTime = 0.50; // seconds; smoothing of the cadence estimate

struct ViewFxWeaponSwayState {
	bool haveSample = false;
	double lastTime = 0.0;        // wall seconds of the previous call
	double phase = 0.0;           // integrated drift phase, radians
	double amplitude = 0.0;       // SMOOTHED movement fraction actually applied, 0..1
	unsigned long long seenSerial = 0;
	double serialTime = 0.0;      // wall time the serial last changed
	double serialInterval = 0.0;  // smoothed wall seconds between serial changes; 0 = unknown
	bool haveSerial = false;
};

struct ViewFxWeaponSwayOutput {
	double x = 0.0, y = 0.0, z = 0.0;
	double amplitude = 0.0;   // the smoothed fraction that was applied (diagnostics)
	double target = 0.0;      // the raw fraction it is chasing (diagnostics)
	bool stale = false;       // the speed stream stopped updating (diagnostics)
};

// Frame-rate-independent approach fraction for an exponential smoother with the given half-time.
inline double ViewFxSwayHalfTimeAlpha(double halfTime, double dt) {
	if (!(dt > 0.0) || !(halfTime > 0.0)) return 0.0;
	return 1.0 - std::exp(-0.6931471805599453 * dt / halfTime);
}

// One frame of sway.
//
//   nowSeconds  free-running WALL clock (not demo time -- see the class header)
//   speed       the stack's smoothed planar speed, units/sec
//   speedSerial bumped by the stack every time `speed` is recomputed; the staleness signal
//   intensity   the user's 0-150 percent knob
//
// Idempotent for a repeated call at the same `nowSeconds`: dt is zero, so nothing integrates.
inline ViewFxWeaponSwayOutput ViewFxWeaponSwayStep(
	ViewFxWeaponSwayState& state,
	double nowSeconds, double speed, unsigned long long speedSerial, double intensity) {
	ViewFxWeaponSwayOutput out;

	if (!(intensity > 0.0)) {
		state = ViewFxWeaponSwayState();
		return out;
	}
	if (!std::isfinite(nowSeconds)) return out;

	const bool gapTooLarge =
		state.haveSample && (nowSeconds - state.lastTime) > kViewFxSwayResetGap;
	double dt = 0.0;
	if (!state.haveSample || gapTooLarge || nowSeconds <= state.lastTime) {
		state.haveSample = true;
	} else {
		dt = nowSeconds - state.lastTime;
		if (dt > kViewFxSwayMaxStep) dt = kViewFxSwayMaxStep;
	}
	state.lastTime = nowSeconds;

	// ---- movement fraction (the amplitude TARGET) ----
	double target = 0.0;
	if (std::isfinite(speed)) {
		target = (speed - kViewFxSwaySpeedDeadband)
			/ (kViewFxSwaySpeedRef - kViewFxSwaySpeedDeadband);
		if (target < 0.0) target = 0.0; else if (target > 1.0) target = 1.0;
	}

	// ---- staleness, measured against the stream's OWN cadence ----
	if (!state.haveSerial) {
		state.haveSerial = true;
		state.seenSerial = speedSerial;
		state.serialTime = nowSeconds;
	} else if (speedSerial != state.seenSerial) {
		const double gap = nowSeconds - state.serialTime;
		if (gap > 0.0 && gap <= kViewFxSwayStaleMax) {
			// Learn how often this stream actually updates. A demo playing at 0.1x speed updates
			// ten times slower than one at 1x and is not stale for it.
			if (!(state.serialInterval > 0.0)) state.serialInterval = gap;
			else {
				const double a = ViewFxSwayHalfTimeAlpha(kViewFxSwayIntervalHalfTime, gap);
				state.serialInterval += (gap - state.serialInterval) * a;
			}
		}
		state.seenSerial = speedSerial;
		state.serialTime = nowSeconds;
	} else {
		double staleAfter = kViewFxSwayStaleIntervals
			* ((state.serialInterval > 0.0) ? state.serialInterval : kViewFxSwayStaleMin);
		if (staleAfter < kViewFxSwayStaleMin) staleAfter = kViewFxSwayStaleMin;
		else if (staleAfter > kViewFxSwayStaleMax) staleAfter = kViewFxSwayStaleMax;
		if ((nowSeconds - state.serialTime) > staleAfter) {
			// Target zero and let the amplitude smoother fade there. The old code faded on its
			// own schedule and then restored at full size in one frame; a target is symmetric,
			// so the recovery is the same shape as the decay.
			target = 0.0;
			out.stale = true;
		}
	}

	// ---- amplitude: the one place that can absorb a discontinuous speed estimate ----
	if (dt > 0.0) {
		const double a = ViewFxSwayHalfTimeAlpha(kViewFxSwayAmpHalfTime, dt);
		state.amplitude += (target - state.amplitude) * a;
	}
	if (state.amplitude < 0.0) state.amplitude = 0.0;
	else if (state.amplitude > 1.0) state.amplitude = 1.0;

	// ---- phase ----
	// Frozen while the stream is stale, so resuming continues the figure from where it stopped
	// rather than from wherever a free-running clock reached in the meantime.
	if (dt > 0.0 && !out.stale) {
		state.phase += dt;
		// Keep the accumulator small. Both sines read this one phase at their own rate, so the
		// only safe wrap is the period that returns BOTH to their starting point: 2*pi divided by
		// the greatest common rate (0.9 and 0.6 are 3x and 2x of 0.3), not by either rate alone.
		constexpr double kCommonRate = 0.3;
		constexpr double kWrapPeriod = 2.0 * 3.14159265358979323846 / kCommonRate;
		if (state.phase > kWrapPeriod) state.phase = std::fmod(state.phase, kWrapPeriod);
	}

	const double amp = kViewFxSwayBaseAmp * (intensity / 100.0) * state.amplitude;
	out.x = std::sin(state.phase * kViewFxSwayRateX) * amp;
	out.y = std::sin(state.phase * kViewFxSwayRateY + kViewFxSwayPhaseY) * amp * kViewFxSwayAmpY;
	out.z = 0.0;
	out.amplitude = state.amplitude;
	out.target = target;
	return out;
}

} // namespace Filmmaker
