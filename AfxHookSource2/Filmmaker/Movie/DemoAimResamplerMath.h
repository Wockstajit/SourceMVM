#pragma once

// Demo aim resampling -- pure math, no engine dependencies so it can be unit tested offline.
//
// WHY THIS EXISTS.
// Smooth View's "anchor" is the spectated player's recorded aim. Live, the equivalent signal is
// the player's own view angles, recomputed from raw mouse deltas every render frame: continuous
// in value AND sampled at render rate. In a demo it is the engine's per-frame reconstruction of
// a 64 Hz networked stream, and MEASURED (mvm_debug camera.smoothview.trace, 20 runs across two
// sessions) that reconstruction carries a large amount of per-frame noise:
//
//   lag-1 autocorrelation of the anchor's per-frame angular VELOCITY
//     high frame rate (>150 fps, 3-4.7 frames per tick) : +0.89   (a smooth process)
//     the user's frame rate (57-93 fps, ~1 frame/tick)  : +0.40   (about half the frame-to-frame
//                                                                  velocity variance is noise)
//     the CAMERA derived from it, after the chase filter: +0.87   (our low-pass cleans it up)
//
// The free-aim residual is `anchor - camera`, i.e. the difference between the noisy signal and its
// own smoothed version -- which is very nearly the noise by itself. That residual drives the
// viewmodel offset and the 3D crosshair, which is why the gun and crosshair jitter visibly worse
// than the camera does, and why no amplitude slider removes it.
//
// SmoothView.cpp already carried this diagnosis in a comment and shipped `faVmSmoothing` as a
// mitigation. That is a low-pass on ONE consumer (the gun's positional offset); it cannot reach
// the 3D crosshair or the faVmAim rotation, and being a low-pass it also rounds off genuine
// motion -- which is why adding gun smoothing did not help the flicks.
//
// WHAT THE DEFECT IS NOT.
// The obvious suspect is a tick staircase -- the aim holding for a frame or two and then jumping.
// It was tested and REJECTED twice. Simulating a staircase (nearest-tick sampling of a smooth aim
// at 70 fps against 64 tick) yields a velocity autocorrelation of +0.90, not the +0.40 that was
// actually measured; and a pure hold-then-jump series has NEGATIVE autocorrelation (-0.33), which
// is nothing like the observation either. So interpolating between recorded tick values is aimed
// at the wrong defect -- and at the user's ~1.1 frames per tick nearly every frame carries a new
// tick anyway, so such a scheme would just replay the same noisy values a tick late.
//
// What the numbers describe is a smooth aim plus substantial ADDITIVE noise.
//
// WHAT THIS DOES INSTEAD.
// Estimate the underlying aim by LOCAL POLYNOMIAL REGRESSION (Savitzky-Golay) over a short window
// of recent per-frame samples: fit a quadratic in time by least squares and read the fitted value
// and its slope.
//
// Regression, specifically, rather than a low-pass -- and that distinction is the whole point:
//   * An exponential low-pass (what every "smoothing" slider does, including the faVmSmoothing
//     band-aid already in SmoothView) chases its input, so it always lags and always rounds the
//     peak off a fast flick. That is why adding gun smoothing did not help.
//   * A local quadratic fit tracks the local TREND. On a constant-velocity turn it is exactly
//     unbiased; on constant acceleration it is exactly unbiased. It removes zero-mean noise
//     without flattening genuine motion, which is what "1:1 with live" requires.
// The fitted slope is the "eye angles -> mouse movement" signal: a clean per-frame angular
// velocity, the same shape the GMod addon gets from InputMouseApply.
//
// The window is specified in TIME, not frames, so behaviour does not change with frame rate.

#include <cmath>

namespace Filmmaker {

inline double DemoAimWrapDegrees(double angle) {
	angle = std::fmod(angle, 360.0);
	if (angle > 180.0) angle -= 360.0;
	else if (angle < -180.0) angle += 360.0;
	return angle;
}

inline double DemoAimClamp(double v, double lo, double hi) {
	return v < lo ? lo : v > hi ? hi : v;
}

// One recorded tick. `t` is demo seconds (tick index * interval per tick); `yaw` is UNWRAPPED --
// see DemoAimResampler::Update, which accumulates yaw continuously so a 359->1 crossing is a
// small step rather than a 358 degree one. Splining wrapped yaw would fling the camera around.
struct DemoAimSample {
	double t = 0.0;
	double pitch = 0.0;
	double yaw = 0.0;
	int tick = 0;
};

struct DemoAimFitResult {
	bool valid = false;
	double value = 0.0;
	double rate = 0.0;   // d(value)/dt in degrees per second -- the synthetic "mouse" velocity
	int used = 0;        // samples that fell inside the window
	int degree = 0;      // 2 normally; drops to 1, then 0, as the window empties
};

// Local polynomial regression of v(t), evaluated at tEval.
//
// Samples are oldest-first with `n` entries. Only those within `window` seconds before the newest
// sample take part, so the estimator's behaviour is set by TIME and does not change with frame
// rate. Degree falls back automatically (2 -> 1 -> 0) when there are too few points or the normal
// equations are ill-conditioned, so a fresh buffer or a frame-rate collapse degrades to something
// sane instead of producing garbage.
//
// Time is normalised by the window before fitting. Without that, the x^4 moments of a ~0.06 s
// window underflow toward 1e-8 and the 3x3 solve loses most of its precision.
inline DemoAimFitResult DemoAimLocalFit(
	const double* t, const double* v, int n, double tEval, double window) {
	DemoAimFitResult out;
	if (n <= 0 || !t || !v) return out;
	if (!(window > 0.0) || !std::isfinite(window)) window = 1.0 / 16.0;

	const double newest = t[n - 1];
	const double oldestAllowed = newest - window;

	int first = n - 1;
	while (first > 0 && t[first - 1] >= oldestAllowed) --first;
	const int count = n - first;
	out.used = count;

	// Degenerate: a single point carries no slope information.
	if (count <= 1) {
		out.valid = true;
		out.value = v[n - 1];
		out.rate = 0.0;
		out.degree = 0;
		return out;
	}

	const double scale = (window > 0.0) ? window : 1.0;
	double S0 = 0, S1 = 0, S2 = 0, S3 = 0, S4 = 0, T0 = 0, T1 = 0, T2 = 0;
	for (int i = first; i < n; ++i) {
		const double u = (t[i] - tEval) / scale;
		const double u2 = u * u;
		S0 += 1.0;      S1 += u;        S2 += u2;
		S3 += u2 * u;   S4 += u2 * u2;
		T0 += v[i];     T1 += u * v[i]; T2 += u2 * v[i];
	}

	// Quadratic: solve the symmetric 3x3 normal equations by Cramer's rule.
	if (count >= 5) {
		const double d =
			  S0 * (S2 * S4 - S3 * S3)
			- S1 * (S1 * S4 - S3 * S2)
			+ S2 * (S1 * S3 - S2 * S2);
		if (std::fabs(d) > 1e-12) {
			const double a =
				( T0 * (S2 * S4 - S3 * S3)
				- S1 * (T1 * S4 - S3 * T2)
				+ S2 * (T1 * S3 - S2 * T2)) / d;
			const double b =
				( S0 * (T1 * S4 - S3 * T2)
				- T0 * (S1 * S4 - S3 * S2)
				+ S2 * (S1 * T2 - T1 * S2)) / d;
			if (std::isfinite(a) && std::isfinite(b)) {
				out.valid = true;
				out.value = a;
				out.rate = b / scale;
				out.degree = 2;
				return out;
			}
		}
	}

	// Linear fallback.
	{
		const double d = S0 * S2 - S1 * S1;
		if (std::fabs(d) > 1e-12) {
			const double a = (T0 * S2 - S1 * T1) / d;
			const double b = (S0 * T1 - S1 * T0) / d;
			if (std::isfinite(a) && std::isfinite(b)) {
				out.valid = true;
				out.value = a;
				out.rate = b / scale;
				out.degree = 1;
				return out;
			}
		}
	}

	// Mean fallback.
	out.valid = true;
	out.value = (S0 > 0.0) ? (T0 / S0) : v[n - 1];
	out.rate = 0.0;
	out.degree = 0;
	return out;
}

// Sample clock.
//
// The regression needs a time axis that is SMOOTH and MONOTONE. It does not need to agree with
// absolute demo time: the sample timestamps and the evaluation point are both taken from this
// clock, so any consistent axis gives the same fit. Free-running accumulated render time is
// exactly that, and it is jitter-free.
//
// MEASURED, and the reason this is as simple as it is: an earlier version tried to be clever and
// track demo time -- estimating a playback rate and soft-locking the clock toward the newest tick
// time each frame. Because that lock target is a tick STAIRCASE, the correction injected a
// per-frame phase wobble into the evaluation point, which the fit then converted into amplitude
// noise proportional to turn speed. In-game that version measured the velocity-noise metric
// WORSE than the raw engine signal (3.4% -> 15.8% at 202 fps) even though it improved the
// autocorrelation. The clock was the noise source. Do not reintroduce a lock here.
//
// Seeks are not this object's problem: the caller detects them from the demo tick and resets.
struct DemoAimClock {
	bool initialized = false;
	double playTime = 0.0;

	void Reset() {
		initialized = true;
		playTime = 0.0;
	}

	// `renderDt` is unscaled real frame time. Returns the query clock.
	double Advance(double renderDt, bool paused) {
		if (!initialized) {
			initialized = true;
			playTime = 0.0;
			return playTime;
		}
		// A paused demo must not animate: hold the clock exactly where it is.
		if (paused) return playTime;
		if (std::isfinite(renderDt) && renderDt > 0.0 && renderDt <= 0.5) playTime += renderDt;
		return playTime;
	}
};

} // namespace Filmmaker
