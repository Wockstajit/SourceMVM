// Regression tests for the GTA V camera-shake evaluator (Filmmaker/Movie/RageShake.*).
//
// These exist because this module shipped twice in a state where it was mathematically running
// and visually doing nothing. Both failures were invisible from the code:
//
//   1. Family amplitudes were ~5x too small, so every style resolved to a fraction of a degree.
//   2. One-shots used DAMPED_SINEWAVE, whose decay sweep starts mid-cycle -- explosions opened
//      at ~17% of their nominal amplitude, i.e. muted on the exact frames they should hit.
//
// "It compiles" and "the shake is applied" were both true throughout. So the assertions below are
// about MAGNITUDE and BEHAVIOUR, not about plumbing: every menu style has to move the camera a
// visible amount, intensity has to actually span a range, speed has to change timing WITHOUT
// changing size, and one-shots have to die while loops keep going.

#include "../../AfxHookSource2/Filmmaker/Movie/RageShake.h"
#include "../../AfxHookSource2/Filmmaker/Movie/CamPathShakeRampMath.h"

#include <cmath>
#include <cstdio>
#include <string>

using namespace Filmmaker;

namespace {

int g_failures = 0;

void Check(bool ok, const std::string& what) {
	if (!ok) {
		std::printf("FAIL: %s\n", what.c_str());
		++g_failures;
	}
}

// Must match CamPathShake.cpp's AmplitudeFor and the speed/50 time scale ShakeRamp::ClockSeconds
// integrates. Duplicated on purpose: if the mapping there changes, these tests should be
// re-examined rather than silently follow along.
double AmplitudeFor(double intensity) { return 0.024 * intensity * intensity + 0.076 * intensity; }
double TimeScaleFor(double speed) { return speed / 50.0; }

struct Peak {
	double pitch = 0, yaw = 0, roll = 0, pos = 0, fov = 0;
	double AngMax() const { return std::fmax(pitch, std::fmax(yaw, roll)); }
};

// Peak excursion over `seconds`, sampled well above any style's frequency.
Peak Sweep(int style, double intensity, double speed, double seconds = 12.0) {
	const RageShakeDef& def = RageShake_Style(style);
	const double amp = AmplitudeFor(intensity);
	const double ts = TimeScaleFor(speed);
	Peak p;
	const int steps = (int)(seconds * 200.0);
	for (int i = 0; i < steps; ++i) {
		const RageShakePose q = RageShake_Eval(def, i / 200.0, amp, ts);
		p.pitch = std::fmax(p.pitch, std::fabs(q.pitch));
		p.yaw = std::fmax(p.yaw, std::fabs(q.yaw));
		p.roll = std::fmax(p.roll, std::fabs(q.roll));
		p.fov = std::fmax(p.fov, std::fabs(q.fov));
		p.pos = std::fmax(p.pos, std::sqrt(
			q.right * q.right + q.fwd * q.fwd + q.up * q.up +
			q.worldX * q.worldX + q.worldY * q.worldY + q.worldZ * q.worldZ));
	}
	return p;
}

} // namespace

int main() {
	Check(RageShake_StyleCount() == 86, "the native style table still has all 86 SHAKE_CAM names");
	Check(RageShake_MenuCount() > 0, "the menu is not empty");
	Check(RageShake_RockstarEditorCount() == 5,
		"the Rockstar Editor section is its five marker shakes");

	// The five Rockstar Editor shakes must be exactly the REPLAY_-prefixed ones, in the editor's
	// menu order.
	const char* kRse[] = {
		"REPLAY_HAND_SHAKE", "REPLAY_DRUNK_SHAKE", "REPLAY_EXPLOSION_SHAKE",
		"REPLAY_SKY_DIVING_SHAKE", "REPLAY_HIGH_SPEED_VEHICLE_SHAKE",
	};
	for (int i = 0; i < 5; ++i) {
		Check(RageShake_MenuStyle(i) == RageShake_FindStyle(kRse[i]),
			std::string("menu slot ") + std::to_string(i) + " is " + kRse[i]);
	}

	// Every menu entry resolves, has a label, and round-trips.
	for (int m = 0; m < RageShake_MenuCount(); ++m) {
		const int si = RageShake_MenuStyle(m);
		Check(si >= 0 && si < RageShake_StyleCount(), "menu entry resolves to a real style");
		Check(RageShake_StyleLabel(si)[0] != '\0', "menu entry has a label");
		Check(RageShake_MenuIndexOfStyle(si) == m, "menu index round-trips");
	}

	// --- MAGNITUDE: the bug that shipped twice ---
	for (int m = 0; m < RageShake_MenuCount(); ++m) {
		const int si = RageShake_MenuStyle(m);
		const std::string name = RageShake_StyleLabel(si);

		const Peak mid = Sweep(si, 5.0, 50.0);
		// At the default intensity a shake must be plainly visible. Half a degree of angular
		// swing, or a unit of travel, is about the floor for "you can tell it is on" at 90 FOV.
		Check(mid.AngMax() >= 0.30 || mid.pos >= 0.80,
			name + " moves the camera a visible amount at intensity 5 (got "
			+ std::to_string(mid.AngMax()) + " deg / " + std::to_string(mid.pos) + " units)");
		// ...and must not be nauseating.
		Check(mid.AngMax() <= 12.0, name + " is not absurd at intensity 5");

		// --- INTENSITY spans a real range, and does so faster than linearly (the quadratic
		// intensity curve).
		const Peak lo = Sweep(si, 1.0, 50.0);
		const Peak hi = Sweep(si, 10.0, 50.0);
		const double loM = std::fmax(lo.AngMax(), lo.pos);
		const double midM = std::fmax(mid.AngMax(), mid.pos);
		const double hiM = std::fmax(hi.AngMax(), hi.pos);
		Check(loM > 0.0 && midM > loM * 4.0, name + " intensity 5 is far above intensity 1");
		Check(hiM > midM * 2.0, name + " intensity 10 is far above intensity 5");

		// --- SPEED is a TIME SCALE: it changes when things happen, never how big they are.
		// (Looping styles only: a one-shot's peak legitimately shifts, because a stretched
		// envelope means the oscillator is at a different point when the peak is available.)
		const bool oneShot = RageShake_Finished(RageShake_Style(si), 8.0, 1.0);
		if (!oneShot) {
			const Peak slow = Sweep(si, 5.0, 10.0, 40.0); // longer window: it oscillates slower
			const Peak fast = Sweep(si, 5.0, 100.0);
			Check(std::fabs(slow.AngMax() - mid.AngMax()) <= mid.AngMax() * 0.15 + 0.02,
				name + " speed 10 keeps the same amplitude as speed 50");
			Check(std::fabs(fast.AngMax() - mid.AngMax()) <= mid.AngMax() * 0.15 + 0.02,
				name + " speed 100 keeps the same amplitude as speed 50");
		}
	}

	// --- LIFETIME: one-shots stop, loops do not.
	{
		const int expl = RageShake_FindStyle("REPLAY_EXPLOSION_SHAKE");
		const int hand = RageShake_FindStyle("REPLAY_HAND_SHAKE");
		Check(expl >= 0 && hand >= 0, "the two reference styles exist");

		// The explosion has to PUNCH immediately -- this is the damped-sine regression.
		double early = 0.0;
		for (int i = 0; i < 20; ++i) { // first 100 ms
			const RageShakePose q = RageShake_Eval(RageShake_Style(expl), i / 200.0, AmplitudeFor(5.0), 1.0);
			early = std::fmax(early, std::fabs(q.pitch));
		}
		const Peak explPeak = Sweep(expl, 5.0, 50.0, 3.0);
		Check(early >= explPeak.pitch * 0.5,
			"the explosion reaches at least half its peak within the first 100 ms");

		Check(RageShake_Finished(RageShake_Style(expl), 8.0, 1.0), "the explosion one-shot ends");
		Check(!RageShake_Finished(RageShake_Style(hand), 600.0, 1.0), "the hand shake loops forever");

		// A slowed one-shot must last LONGER -- proof the time scale reaches the ENVELOPE and not
		// just the oscillator, which is the whole point of speed being a time scale. The explosion
		// releases over 1.2 s, so at t = 2 s it is done at speed 50 (scale 1.0) and only 0.4 s into
		// its release at speed 10 (scale 0.2).
		Check(RageShake_Finished(RageShake_Style(expl), 2.0, TimeScaleFor(50.0)),
			"the one-shot has ended by t=2s at normal speed");
		Check(!RageShake_Finished(RageShake_Style(expl), 2.0, TimeScaleFor(10.0)),
			"the same one-shot is still running at t=2s when slowed 5x");
	}

	// --- DETERMINISM: the same t must always give the same pose (no wall clock, no RNG).
	for (int m = 0; m < RageShake_MenuCount(); ++m) {
		const int si = RageShake_MenuStyle(m);
		const RageShakePose a = RageShake_Eval(RageShake_Style(si), 3.25, 1.0, 1.0);
		const RageShakePose b = RageShake_Eval(RageShake_Style(si), 3.25, 1.0, 1.0);
		Check(a.pitch == b.pitch && a.yaw == b.yaw && a.roll == b.roll
			&& a.right == b.right && a.up == b.up && a.fwd == b.fwd && a.fov == b.fov,
			std::string(RageShake_StyleLabel(si)) + " is deterministic at a given time");
	}

	// --- ZERO INPUTS produce zero output (the non-destructive contract).
	{
		const RageShakePose z = RageShake_Eval(RageShake_Style(RageShake_MenuStyle(0)), 2.0, 0.0, 1.0);
		Check(!z.Any(), "amplitude 0 contributes nothing at all");
		const RageShakePose neg = RageShake_Eval(RageShake_Style(RageShake_MenuStyle(0)), -1.0, 1.0, 1.0);
		Check(!neg.Any(), "a negative time (before the keyframe) contributes nothing");
	}

	{
		// Ramps between shake keys (CamPathShakeRampMath.h): eased, and on an integrated clock.
		using namespace Filmmaker::ShakeRamp;
		Check(Value(2, 8, 0) == 2 && Value(2, 8, 1) == 8 && std::fabs(Value(2, 8, 0.5) - 5) < 1e-12, "a ramp runs from key to key");
		const double h = 1e-4;
		Check(std::fabs(Value(2, 8, h) - 2) < 1e-6 && std::fabs(Value(2, 8, 1 - h) - 8) < 1e-6,
			"and leaves and lands on a key with no sudden change of rate");
		Check(std::fabs(Integral(2, 8, 1) - 5) < 1e-12, "a whole segment's integral is its mean");
		const double spt = 1.0 / 64.0;
		// Hold at speed 50 for 640 ticks (10 s), then ramp to 100 over 640 ticks.
		const ClockKey run[] = { { 0, 50, true }, { 640, 50, true }, { 1280, 100, false } };
		Check(std::fabs(ClockSeconds(run, 3, 0, 320, spt) - 5.0) < 1e-9, "speed 50 is real time");
		Check(std::fabs(ClockSeconds(run, 3, 0, 1280, spt) - 25.0) < 1e-9, "a ramp from 50 to 100 runs 1.5x on average");
		Check(std::fabs(ClockSeconds(run, 3, 0, 1344, spt) - 27.0) < 1e-9, "after the last key the speed holds");
		// The clock's rate is the current speed and never jumps: no rush while the speed ramps,
		// however long the run has been going (the old time x speed product sped up 4x here).
		double worst = 0;
		for (double t = 641; t < 1279; t += 1) {
			const double rate = (ClockSeconds(run, 3, 0, t + 0.5, spt) - ClockSeconds(run, 3, 0, t - 0.5, spt)) / spt;
			const double want = Value(50, 100, (t - 640) / 640.0) / 50.0;
			worst = std::fmax(worst, std::fabs(rate - want));
		}
		Check(worst < 1e-3, "the clock's rate follows the ramped speed (worst " + std::to_string(worst) + ")");
		const ClockKey solo[] = { { 100, 30, false }, { 200, 90, false } };
		Check(std::fabs(ClockSeconds(solo, 2, 0, 228, spt) - 128 * spt * 0.6) < 1e-9, "an ending run keeps its speed through a handover");
		Check(ClockSeconds(solo, 2, 1, 150, spt) == 0, "before its first key a run's clock has not started");
	}

	if (g_failures == 0) {
		std::printf("rage-shake-math-tests: all checks passed\n");
		return 0;
	}
	std::printf("rage-shake-math-tests: %d failure(s)\n", g_failures);
	return 1;
}
