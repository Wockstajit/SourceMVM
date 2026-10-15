// Unit tests for the demo aim resampler's pure math (DemoAimResamplerMath.h).
//
// This feature has a history of regressions that only showed up in-game, so the properties that
// actually matter are pinned here:
//   * the fit is UNBIASED on real motion (constant velocity and constant acceleration are
//     recovered exactly) -- i.e. it does not flatten flicks, which is the failure mode of every
//     low-pass "smoothing" slider that has been tried on this problem;
//   * it rejects the additive noise that was MEASURED on the engine's per-frame recorded aim;
//   * it beats an exponential low-pass at preserving a flick for the same noise rejection;
//   * the sample clock is a perfectly smooth, monotone time axis at every frame rate -- an
//     earlier version that soft-locked it to the tick staircase WAS the noise source.

#include "../../AfxHookSource2/Filmmaker/Movie/DemoAimResamplerMath.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace Filmmaker;

static int g_failures = 0;

static void Check(bool ok, const std::string& what) {
	if (!ok) {
		std::printf("FAIL: %s\n", what.c_str());
		++g_failures;
	}
}

static void CheckNear(double a, double b, double tol, const std::string& what) {
	if (!(std::fabs(a - b) <= tol)) {
		std::printf("FAIL: %s (got %.9f, want %.9f, tol %g)\n", what.c_str(), a, b, tol);
		++g_failures;
	}
}

// Deterministic pseudo-noise so the test never flakes.
struct Rng {
	unsigned long long s = 0x9E3779B97F4A7C15ull;
	double Next() {                       // uniform in [-1,1]
		s ^= s << 13; s ^= s >> 7; s ^= s << 17;
		return ((double)((s >> 11) & 0x1FFFFFFFFFFFFFull) / (double)0x1FFFFFFFFFFFFFull) * 2.0 - 1.0;
	}
};

static double Lag1Autocorrelation(const std::vector<double>& v) {
	if (v.size() < 8) return 0.0;
	double mean = 0.0;
	for (double x : v) mean += x;
	mean /= (double)v.size();
	double num = 0.0, den = 0.0;
	for (size_t i = 0; i + 1 < v.size(); ++i) num += (v[i] - mean) * (v[i + 1] - mean);
	for (double x : v) den += (x - mean) * (x - mean);
	return den > 0.0 ? num / den : 0.0;
}

// ---------------------------------------------------------------------------------------------
// The fit must be UNBIASED on real motion. This is the property that separates it from a filter.
// ---------------------------------------------------------------------------------------------

static void TestExactOnConstantVelocity() {
	const double dt = 1.0 / 70.0, window = 0.090, slope = 690.0;   // a full-speed flick
	std::vector<double> t, v;
	for (int i = 0; i < 12; ++i) { t.push_back(i * dt); v.push_back(3.0 + slope * i * dt); }
	const DemoAimFitResult r = DemoAimLocalFit(t.data(), v.data(), (int)t.size(), t.back(), window);
	Check(r.valid, "constant-velocity fit is valid");
	CheckNear(r.value, v.back(), 1e-9, "constant velocity: value is exact (no lag, no rounding)");
	CheckNear(r.rate, slope, 1e-6, "constant velocity: a 690 deg/s flick keeps its full speed");
}

static void TestExactOnConstantAcceleration() {
	const double dt = 1.0 / 70.0, window = 0.090;
	auto f = [](double x) { return 2.0 + 40.0 * x + 900.0 * x * x; };
	std::vector<double> t, v;
	for (int i = 0; i < 12; ++i) { t.push_back(i * dt); v.push_back(f(i * dt)); }
	const double te = t.back();
	const DemoAimFitResult r = DemoAimLocalFit(t.data(), v.data(), (int)t.size(), te, window);
	Check(r.degree == 2, "quadratic degree selected when points allow");
	CheckNear(r.value, f(te), 1e-7, "constant acceleration: value is exact");
	CheckNear(r.rate, 40.0 + 1800.0 * te, 1e-4, "constant acceleration: slope is exact");
}

static void TestEvaluatesAtRequestedTime() {
	const double dt = 1.0 / 70.0, window = 0.090, lag = 0.015;
	auto f = [](double x) { return 5.0 + 120.0 * x; };
	std::vector<double> t, v;
	for (int i = 0; i < 12; ++i) { t.push_back(i * dt); v.push_back(f(i * dt)); }
	const double te = t.back() - lag;
	const DemoAimFitResult r = DemoAimLocalFit(t.data(), v.data(), (int)t.size(), te, window);
	CheckNear(r.value, f(te), 1e-7, "lag evaluates the fit at the requested earlier time");
}

// ---------------------------------------------------------------------------------------------
// Noise rejection, against the measured condition.
// ---------------------------------------------------------------------------------------------

static void TestRejectsMeasuredNoise() {
	// A smooth aim (what the player's hand did) plus additive per-frame noise -- the model the
	// measurements actually support. The engine's recorded aim was measured at a velocity lag-1
	// autocorrelation of about +0.40 at this frame rate; a smooth process sits near +0.9.
	//
	// Both amplitudes are calibrated to the logs rather than picked for effect: the demo's median
	// yaw speed while moving was 12-21 deg/s (so the signal's velocity is ~10 deg/s RMS here), and
	// the median frame-to-frame velocity CHANGE was 41-57% of that speed, which back-solves to
	// about 0.05-0.08 degrees of per-frame noise.
	const double dt = 1.0 / 70.0, window = 0.090, lag = 0.015;
	auto trueYaw = [](double x) { return 5.0 * std::sin(x * 2.2) + 1.5 * std::sin(x * 5.1); };

	Rng rng;
	std::vector<double> t, raw;
	for (int i = 0; i < 700; ++i) {
		t.push_back(i * dt);
		raw.push_back(trueYaw(i * dt) + 0.139 * rng.Next());   // uniform, sd ~= 0.08 deg
	}

	std::vector<double> rawVel, fitVel, trueVel;
	double pRaw = 0, pFit = 0, pTrue = 0;
	for (int i = 8; i < (int)t.size(); ++i) {
		const double te = t[i] - lag;
		const DemoAimFitResult f = DemoAimLocalFit(t.data(), raw.data(), i + 1, te, window);
		if (i > 12) {
			rawVel.push_back((raw[i] - pRaw) / dt);
			fitVel.push_back((f.value - pFit) / dt);
			trueVel.push_back((trueYaw(te) - pTrue) / dt);
		}
		pRaw = raw[i];
		pFit = f.value;
		pTrue = trueYaw(te);
	}

	const double aRaw = Lag1Autocorrelation(rawVel);
	const double aFit = Lag1Autocorrelation(fitVel);
	std::printf("  velocity lag-1 acf: engine-like %.3f -> reconstructed %.3f\n", aRaw, aFit);
	Check(aRaw < 0.70, "the synthetic input really is noisy (sanity check on the model)");
	Check(aFit > aRaw + 0.25, "reconstruction markedly smooths the velocity series");
	Check(aFit > 0.85, "reconstructed velocity looks like a smooth process");

	// Amplitude must survive. Compare peak-to-peak against the TRUE motion, not the noisy input.
	double fitMin = 1e9, fitMax = -1e9, trueMin = 1e9, trueMax = -1e9;
	for (double x : fitVel) { fitMin = x < fitMin ? x : fitMin; fitMax = x > fitMax ? x : fitMax; }
	for (double x : trueVel) { trueMin = x < trueMin ? x : trueMin; trueMax = x > trueMax ? x : trueMax; }
	const double ratio = (fitMax - fitMin) / (trueMax - trueMin);
	std::printf("  velocity peak-to-peak vs truth: %.3f\n", ratio);
	// The lower bound is the one that matters: the estimate must not have flattened the motion.
	// A little above 1.0 is residual noise the fit did not remove, not invented movement.
	Check(ratio > 0.8 && ratio < 1.8, "reconstruction preserves the real motion's amplitude");
}

static void TestBeatsExponentialLowPassOnAFlick() {
	// The band-aid already in SmoothView is an exponential low-pass. On a flick it lags and
	// rounds the peak; the regression should not. Both are given the same noisy input.
	const double dt = 1.0 / 70.0, window = 0.090;
	auto flick = [](double x) {           // still, then a hard 120 degree turn, then still
		if (x < 0.30) return 0.0;
		if (x > 0.45) return 120.0;
		const double u = (x - 0.30) / 0.15;
		return 120.0 * (u * u * (3.0 - 2.0 * u));
	};

	Rng rng;
	std::vector<double> t, raw;
	for (int i = 0; i < 90; ++i) { t.push_back(i * dt); raw.push_back(flick(i * dt) + 0.035 * rng.Next()); }

	// Exponential low-pass with a comparable time constant to the regression window.
	const double tau = 0.030;
	const double alpha = 1.0 - std::exp(-dt / tau);
	double lp = raw[0], prevLp = raw[0], prevFit = raw[0];
	double peakLp = 0.0, peakFit = 0.0, peakTrue = 0.0, prevTrue = 0.0;

	for (int i = 1; i < (int)t.size(); ++i) {
		lp += (raw[i] - lp) * alpha;
		const DemoAimFitResult f = DemoAimLocalFit(t.data(), raw.data(), i + 1, t[i], window);
		const double vLp = (lp - prevLp) / dt;
		const double vFit = (f.value - prevFit) / dt;
		const double vTrue = (flick(t[i]) - prevTrue) / dt;
		if (vLp > peakLp) peakLp = vLp;
		if (vFit > peakFit) peakFit = vFit;
		if (vTrue > peakTrue) peakTrue = vTrue;
		prevLp = lp; prevFit = f.value; prevTrue = flick(t[i]);
	}

	std::printf("  flick peak velocity: true %.0f | regression %.0f | exp low-pass %.0f deg/s\n",
		peakTrue, peakFit, peakLp);
	Check(peakFit > peakLp, "regression preserves more of the flick than an exponential low-pass");
	Check(peakFit > 0.80 * peakTrue, "regression keeps most of the flick's true peak speed");
}

// ---------------------------------------------------------------------------------------------
// Degenerate inputs.
// ---------------------------------------------------------------------------------------------

static void TestDegenerateInputs() {
	const double t1[1] = { 0.0 };
	const double v1[1] = { 7.0 };
	DemoAimFitResult r = DemoAimLocalFit(t1, v1, 1, 0.0, 0.090);
	Check(r.valid && r.degree == 0, "a single sample yields a value with no slope");
	CheckNear(r.value, 7.0, 1e-12, "single sample passes its value through");
	CheckNear(r.rate, 0.0, 1e-12, "single sample reports no rate");

	r = DemoAimLocalFit(nullptr, nullptr, 0, 0.0, 0.090);
	Check(!r.valid, "no samples is not a valid fit");

	r = DemoAimLocalFit(t1, v1, 0, 0.0, 0.090);
	Check(!r.valid, "zero count is not a valid fit");

	// All samples at the same instant: the normal equations are singular in u.
	const double tSame[6] = { 2.0, 2.0, 2.0, 2.0, 2.0, 2.0 };
	const double vSame[6] = { 1.0, 2.0, 3.0, 4.0, 5.0, 6.0 };
	r = DemoAimLocalFit(tSame, vSame, 6, 2.0, 0.090);
	Check(r.valid && std::isfinite(r.value) && std::isfinite(r.rate),
		"coincident timestamps degrade safely instead of dividing by zero");

	// Only two samples in the window: must fall back to linear, not attempt a quadratic.
	const double t2[2] = { 0.0, 1.0 / 70.0 };
	const double v2[2] = { 0.0, 2.0 };
	r = DemoAimLocalFit(t2, v2, 2, t2[1], 0.090);
	Check(r.valid && r.degree == 1, "two samples fall back to a linear fit");
	CheckNear(r.rate, 2.0 * 70.0, 1e-6, "linear fallback still reports the right slope");
}

static void TestWindowExcludesOldSamples() {
	// A stale sample far outside the window must not drag the fit.
	const double dt = 1.0 / 70.0, window = 0.090;
	std::vector<double> t, v;
	t.push_back(-5.0); v.push_back(999.0);          // ancient outlier
	for (int i = 0; i < 10; ++i) { t.push_back(i * dt); v.push_back(4.0); }
	const DemoAimFitResult r = DemoAimLocalFit(t.data(), v.data(), (int)t.size(), t.back(), window);
	CheckNear(r.value, 4.0, 1e-6, "samples older than the window are excluded");
}

// ---------------------------------------------------------------------------------------------
// Playback clock.
// ---------------------------------------------------------------------------------------------

static void TestClockIsSmoothAndMonotone() {
	// The clock's only job is a smooth, monotone time axis for the fit -- it deliberately does NOT
	// track demo time. An earlier version soft-locked it to the tick staircase and that lock was
	// itself the noise source (measured in-game: velocity noise 3.4% -> 15.8%).
	const double rates[] = { 1.0 / 40.0, 1.0 / 64.0, 1.0 / 70.0, 1.0 / 202.0, 1.0 / 300.0 };
	for (double renderDt : rates) {
		DemoAimClock c;
		double prev = -1e9;
		std::vector<double> steps;
		const int frames = (int)(6.0 / renderDt);
		for (int f = 0; f < frames; ++f) {
			const double t = c.Advance(renderDt, false);
			if (f > 2) {
				Check(t >= prev - 1e-12, "clock is monotone at every frame rate");
				steps.push_back(t - prev);
			}
			prev = t;
		}
		double mn = 1e9, mx = -1e9;
		for (double s2 : steps) { mn = s2 < mn ? s2 : mn; mx = s2 > mx ? s2 : mx; }
		Check(mx - mn < 1e-12, "clock steps are exactly the render delta -- no injected wobble");
	}
}

static void TestClockPauseAndBadDeltas() {
	DemoAimClock c;
	for (int i = 0; i < 10; ++i) c.Advance(1.0 / 70.0, false);
	const double held = c.Advance(1.0 / 70.0, true);
	for (int i = 0; i < 30; ++i)
		CheckNear(c.Advance(1.0 / 70.0, true), held, 1e-12, "a paused demo does not animate");
	CheckNear(c.Advance(0.0, false), held, 1e-12, "a zero frame time does not advance the clock");
	CheckNear(c.Advance(-1.0, false), held, 1e-12, "a negative frame time is ignored");
	CheckNear(c.Advance(5.0, false), held, 1e-12, "an absurd frame time is ignored");
	const double after = c.Advance(1.0 / 70.0, false);
	Check(after > held, "the clock resumes normally after bad deltas");
}

static void TestClockSurvivesVariableFrameTime() {
	// Real frame times jitter. The clock must still be strictly monotone.
	Rng rng;
	DemoAimClock c;
	double prev = -1e9;
	for (int f = 0; f < 2000; ++f) {
		const double dt = (1.0 / 70.0) * (1.0 + 0.4 * rng.Next());
		const double t = c.Advance(dt, false);
		if (f > 2) Check(t >= prev - 1e-12, "clock is monotone under jittery frame times");
		prev = t;
	}
}

// ---------------------------------------------------------------------------------------------

static void TestWrapAndClamp() {
	CheckNear(DemoAimWrapDegrees(370.0), 10.0, 1e-9, "wrap above 180");
	CheckNear(DemoAimWrapDegrees(-190.0), 170.0, 1e-9, "wrap below -180");
	CheckNear(DemoAimClamp(5.0, 0.0, 3.0), 3.0, 1e-9, "clamp high");
	CheckNear(DemoAimClamp(-5.0, 0.0, 3.0), 0.0, 1e-9, "clamp low");

	// Unwrapping across the seam must produce small steps, not a 358 degree sweep.
	double cont = 179.5, lastRaw = 179.5;
	const double raws[] = { 179.9, -179.8, -179.4, 179.9 };
	const double expected[] = { 179.9, 180.2, 180.6, 179.9 };
	for (int i = 0; i < 4; ++i) {
		cont += DemoAimWrapDegrees(raws[i] - lastRaw);
		lastRaw = raws[i];
		CheckNear(cont, expected[i], 1e-9, "yaw unwraps continuously across +-180");
	}
}

int main() {
	TestWrapAndClamp();
	TestExactOnConstantVelocity();
	TestExactOnConstantAcceleration();
	TestEvaluatesAtRequestedTime();
	TestRejectsMeasuredNoise();
	TestBeatsExponentialLowPassOnAFlick();
	TestDegenerateInputs();
	TestWindowExcludesOldSamples();
	TestClockIsSmoothAndMonotone();
	TestClockPauseAndBadDeltas();
	TestClockSurvivesVariableFrameTime();

	if (g_failures == 0) {
		std::printf("DemoAimResamplerMathTests: all tests passed\n");
		return 0;
	}
	std::printf("DemoAimResamplerMathTests: %d failure(s)\n", g_failures);
	return 1;
}
