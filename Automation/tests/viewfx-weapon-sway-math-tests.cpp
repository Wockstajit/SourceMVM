#include "../../AfxHookSource2/Filmmaker/Movie/ViewFxWeaponSwayMath.h"

#include <cmath>
#include <cstdlib>
#include <iostream>

using namespace Filmmaker;

namespace {
void Check(bool condition, const char* message) {
	if (!condition) {
		std::cerr << "FAIL: " << message << "\n";
		std::exit(1);
	}
}

bool Near(double a, double b, double epsilon = 1e-9) {
	return std::fabs(a - b) <= epsilon;
}

// Run the sway for `seconds` at `fps`, bumping the speed serial every `serialPeriod` wall seconds
// (that is the demo-tick cadence as the sway sees it), and report the largest single-frame jump in
// the applied offset. That jump IS the reported jitter: the offset is a position, so any step in
// it is a visible flick of the weapon.
struct Run {
	double worstStep = 0.0;
	double lastX = 0.0, lastY = 0.0;
	double finalAmplitude = 0.0;
	double time = 0.0;
	unsigned long long serial = 0;
	double nextSerialAt = 0.0;
	bool haveLast = false;
	ViewFxWeaponSwayState state;
};

void Advance(Run& run, double seconds, double fps, double speed, double serialPeriod,
	double intensity = 100.0) {
	const double dt = 1.0 / fps;
	const int frames = (int)(seconds * fps);
	for (int i = 0; i < frames; ++i) {
		run.time += dt;
		if (serialPeriod > 0.0 && run.time >= run.nextSerialAt) {
			++run.serial;
			run.nextSerialAt = run.time + serialPeriod;
		}
		const ViewFxWeaponSwayOutput out =
			ViewFxWeaponSwayStep(run.state, run.time, speed, run.serial, intensity);
		if (run.haveLast) {
			const double step = std::hypot(out.x - run.lastX, out.y - run.lastY);
			if (step > run.worstStep) run.worstStep = step;
		}
		run.lastX = out.x;
		run.lastY = out.y;
		run.finalAmplitude = out.amplitude;
		run.haveLast = true;
	}
}
} // namespace

int main() {
	// ---- off means off ----
	{
		ViewFxWeaponSwayState state;
		const auto out = ViewFxWeaponSwayStep(state, 1.0, 250.0, 1, 0.0);
		Check(Near(out.x, 0.0) && Near(out.y, 0.0) && Near(out.z, 0.0),
			"zero intensity produces no offset");
	}

	// ---- a stationary player's weapon sits perfectly still ----
	{
		Run run;
		Advance(run, 4.0, 120.0, 0.0, 1.0 / 64.0);
		Check(Near(run.finalAmplitude, 0.0, 1e-3), "a stationary player gets no sway");
		Check(run.worstStep < 1e-4, "a stationary player's weapon does not move at all");
	}

	// ---- movement produces sway, and it is smooth ----
	{
		Run run;
		Advance(run, 6.0, 120.0, 250.0, 1.0 / 64.0);
		Check(run.finalAmplitude > 0.98, "a sprinting player reaches full sway amplitude");
		// 0.4 units peak at 100%; a frame at 120 fps covers a small fraction of a 0.9 rad/s sine.
		Check(run.worstStep < 0.02, "sustained running produces a continuous, step-free offset");
	}

	// ---- THE BUG: an amplitude that steps because the speed estimate steps ----
	// ViewFx's shared estimator zeroes on a seek / spectator switch and freezes on any frame
	// another camera system owns the pose. Applied raw, each of those was a one-frame jump of the
	// weapon's POSITION. Smoothed, the largest achievable jump is bounded by the smoother.
	{
		Run run;
		Advance(run, 4.0, 120.0, 250.0, 1.0 / 64.0);   // running
		const double before = run.lastX;
		Advance(run, 1.0 / 120.0, 120.0, 0.0, 1.0 / 64.0); // estimator zeroed by a reset
		Check(std::fabs(run.lastX - before) < 0.02,
			"a speed estimate collapsing to zero fades the sway instead of snapping it");
		Advance(run, 1.0, 120.0, 0.0, 1.0 / 64.0);
		Check(run.finalAmplitude < 0.05, "and it does reach zero shortly afterwards");
	}
	{
		// The reverse edge: speed reappearing at full size must not restore the offset in one
		// frame either.
		Run run;
		Advance(run, 2.0, 120.0, 0.0, 1.0 / 64.0);
		const double before = run.lastX;
		Advance(run, 1.0 / 120.0, 120.0, 250.0, 1.0 / 64.0);
		Check(std::fabs(run.lastX - before) < 0.02,
			"a speed estimate reappearing fades the sway back in instead of snapping it");
	}

	// ---- staleness: a stopped stream stops the sway ----
	{
		Run run;
		Advance(run, 4.0, 120.0, 250.0, 1.0 / 64.0);
		Check(run.finalAmplitude > 0.9, "running at 64 Hz updates is not stale");
		// serialPeriod 0 == the serial never changes again: a paused demo with a frozen estimate.
		Advance(run, 3.0, 120.0, 250.0, 0.0);
		Check(run.finalAmplitude < 0.05,
			"a frozen speed estimate stops the sway rather than marching on a paused demo");
		Check(run.worstStep < 0.02, "and it stops by fading, not by cutting");
	}

	// ---- THE JITTER: slow motion must not read as a stopped stream ----
	// The old gate declared the stream stale after a FIXED 0.30 s of wall clock. Under slow-motion
	// playback a demo tick is hundreds of wall-milliseconds apart, so the gate fired between every
	// pair of ticks: fade out, snap back to full, fade out, once per tick, forever.
	{
		Run run;
		Advance(run, 4.0, 120.0, 250.0, 0.5); // one speed update every 500 ms of wall clock
		Check(run.finalAmplitude > 0.9,
			"a slow-motion demo still sways: staleness is measured against the stream's own cadence");
		const double worstDuringSlowMotion = run.worstStep;
		Advance(run, 6.0, 120.0, 250.0, 0.5);
		Check(run.worstStep < 0.02 && worstDuringSlowMotion < 0.02,
			"slow-motion playback produces no periodic pop");
		Check(run.finalAmplitude > 0.9, "and it stays engaged rather than cycling on and off");
	}

	// ---- the phase is integrated, and it holds across a gate ----
	{
		ViewFxWeaponSwayState state;
		ViewFxWeaponSwayStep(state, 0.0, 250.0, 1, 100.0);
		ViewFxWeaponSwayStep(state, 0.01, 250.0, 2, 100.0);
		const double phase = state.phase;
		// A repeated call at the same wall time integrates nothing.
		ViewFxWeaponSwayStep(state, 0.01, 250.0, 2, 100.0);
		Check(Near(state.phase, phase), "a repeated call at the same time is idempotent");
		// A hitch cannot leap the phase further than one clamped step.
		ViewFxWeaponSwayStep(state, 0.06, 250.0, 3, 100.0);
		Check(state.phase - phase <= kViewFxSwayMaxStep + 1e-9,
			"a hitch advances the phase by at most one clamped step");
	}
	{
		// The phase freezes while the sway is gated out, so resuming continues the figure from
		// where it stopped instead of teleporting to wherever a free-running clock had reached.
		Run run;
		Advance(run, 4.0, 120.0, 250.0, 1.0 / 64.0);
		Advance(run, 3.0, 120.0, 250.0, 0.0); // stale: amplitude falls to zero
		const double frozen = run.state.phase;
		Advance(run, 2.0, 120.0, 250.0, 0.0);
		Check(Near(run.state.phase, frozen, 1e-9),
			"the phase does not march on while the sway is gated out");
	}

	// ---- intensity scales the result linearly ----
	{
		ViewFxWeaponSwayState a, b;
		double t = 0.0;
		unsigned long long serial = 0;
		ViewFxWeaponSwayOutput oa, ob;
		for (int i = 0; i < 600; ++i) {
			t += 1.0 / 120.0;
			++serial;
			oa = ViewFxWeaponSwayStep(a, t, 250.0, serial, 100.0);
			ob = ViewFxWeaponSwayStep(b, t, 250.0, serial, 50.0);
		}
		Check(std::fabs(oa.x) > 1e-3, "the reference run actually produced an offset");
		Check(Near(ob.x, 0.5 * oa.x, 1e-9), "intensity scales the offset linearly");
	}

	// ---- the deadband keeps micro eye-origin jitter out ----
	{
		Run run;
		Advance(run, 3.0, 120.0, kViewFxSwaySpeedDeadband - 1.0, 1.0 / 64.0);
		Check(Near(run.finalAmplitude, 0.0, 1e-3),
			"breathing-scale eye motion below the deadband does not read as walking");
	}

	std::cout << "ViewFxWeaponSwayMathTests: all checks passed\n";
	return 0;
}
