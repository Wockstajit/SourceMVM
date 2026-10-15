#include "../../AfxHookSource2/Filmmaker/Movie/CameraShakePhaseClockMath.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <vector>

using Filmmaker::PhaseClockState;
using Filmmaker::PhaseClockReset;
using Filmmaker::PhaseClockStep;
using Filmmaker::kPhaseClockMaxStep;
using Filmmaker::kPhaseClockMaxLead;

namespace {

int g_failures = 0;

void Check(bool ok, const char *message) {
	if (!ok) {
		std::cerr << "FAIL: " << message << '\n';
		++g_failures;
	}
}

bool Near(double a, double b, double tolerance = 1e-9) { return std::fabs(a - b) <= tolerance; }

// Plays `pattern` frames-per-tick repeatedly at the given demo tick and wall frame time, and
// returns the steps issued over the second half, once the loop has settled.
std::vector<double> Settled(double tick, double wall, const std::vector<int> &pattern, int repeats) {
	PhaseClockState s;
	std::vector<double> steps;
	for (int r = 0; r < repeats; ++r)
		for (int frames : pattern) {
			steps.push_back(PhaseClockStep(s, tick, wall));
			for (int i = 1; i < frames; ++i) steps.push_back(PhaseClockStep(s, 0.0, wall));
		}
	return std::vector<double>(steps.begin() + steps.size() / 2, steps.end());
}

} // namespace

int main() {
	const double tick = 1.0 / 64;

	// The bug itself: a render frame that shares a demo tick used to advance nothing.
	{
		PhaseClockState s;
		(void)PhaseClockStep(s, tick, 0.008);
		const double held = PhaseClockStep(s, 0.0, 0.008);
		Check(held > 0.004, "a same-tick frame advances the phase instead of freezing it");
	}

	// The regression of the first fix: with two held frames in one tick the TICK frame used to
	// issue zero instead. Now no frame in the 64-tick / ~110 fps pattern stalls, and the steps
	// stay close to one another -- the staircase is gone, not moved.
	{
		const double wall = tick * 3 / 5; // five frames per three ticks
		const auto steps = Settled(tick, wall, {2, 2, 1}, 40);
		double lo = 1, hi = 0, sum = 0;
		for (double v : steps) { lo = std::min(lo, v); hi = std::max(hi, v); sum += v; }
		Check(lo > 0.006, "no frame stalls in the 2-2-1 hold pattern");
		Check(hi - lo < 0.0025, "steps in the 2-2-1 hold pattern stay within 2.5 ms of one another");
		Check(Near(sum / steps.size(), wall, 0.0006),
		      "the mean step equals demo time per frame (phase locked to the demo)");
	}

	// The measured regime (2026-09-19 log): the demo clock steps ~7.8 ms most frames and 0 on
	// some, against an ~8.9 ms wall frame. The phase must come out smooth and stay locked.
	{
		PhaseClockState s;
		double issued = 0, demo = 0, lo = 1, hi = 0;
		for (int i = 0; i < 400; ++i) {
			const double dt = (i % 12 == 0) ? 0.0 : 0.0073 + 0.0012 * ((i * 7) % 3) / 2.0;
			const double step = PhaseClockStep(s, dt, 0.0089);
			issued += step; demo += dt;
			if (i > 200) { lo = std::min(lo, step); hi = std::max(hi, step); }
		}
		Check(hi - lo < 0.002, "an irregular sub-tick demo clock is smoothed to within 2 ms");
		Check(std::fabs(issued - demo) < kPhaseClockMaxLead + 1e-9,
		      "the issued phase never drifts from demo time by more than the lead bound");
	}

	// A PAUSED demo holds the pose. The pause looks identical to a same-tick hold for the first
	// frames, so the phase creeps briefly and must then stop completely.
	{
		PhaseClockState s;
		for (int i = 0; i < 200; ++i) (void)PhaseClockStep(s, tick, tick); // normal playback
		double crept = 0;
		for (int i = 0; i < 400; ++i) crept += PhaseClockStep(s, 0.0, 0.009); // then paused
		Check(crept < 0.08, "a paused demo stops the phase instead of bobbing on");
		Check(Near(PhaseClockStep(s, 0.0, 0.009), 0.0), "a settled pause issues exactly nothing");
	}

	// Slow motion is NOT a stall: ticks are far apart in wall time but they do keep arriving,
	// which is exactly why the stall window is learned from the observed interval.
	{
		PhaseClockState s;
		const double wall = 0.009;
		const int framesPerTick = 17; // 0.1x playback: one demo tick every ~156 ms of wall time
		double issued = 0, demo = 0;
		for (int i = 0; i < 600; ++i) {
			const double dt = (i % framesPerTick == 0) ? tick : 0.0;
			issued += PhaseClockStep(s, dt, wall);
			demo += dt;
		}
		Check(issued > demo * 0.8,
		      "slow-motion playback keeps advancing rather than being called a stall");
	}

	// Recorder lock: the step is the demo dt exactly, regardless of wall time.
	{
		PhaseClockState s;
		for (int i = 0; i < 10; ++i) {
			const double step = PhaseClockStep(s, tick, 0.5 * (i + 1), true);
			Check(Near(step, tick), "a locked (capture) frame advances by exactly its demo dt");
		}
		Check(Near(s.lead, 0.0), "the lock clears the loop state");
	}

	// Playback speed: at half speed the learned rate halves and the steps follow.
	{
		PhaseClockState s;
		double last = 0;
		// half a tick of demo time per full tick of wall time
		for (int i = 0; i < 400; ++i) last = PhaseClockStep(s, tick * 0.5, tick);
		Check(s.rate > 0.45 && s.rate < 0.55, "the demo rate converges on the observed playback speed");
		Check(Near(last, tick * 0.5, 0.0005), "steps follow the playback speed");
	}

	// Reset clears the loop but keeps the learned rate.
	{
		PhaseClockState s;
		for (int i = 0; i < 400; ++i) (void)PhaseClockStep(s, tick * 0.5, tick);
		(void)PhaseClockStep(s, 0.0, 0.01);
		PhaseClockReset(s);
		Check(Near(s.lead, 0.0) && Near(s.wallSinceTick, 0.0), "reset clears the loop state");
		Check(s.rate > 0.45 && s.rate < 0.55, "reset keeps the learned playback rate");
	}

	// Degenerate inputs.
	{
		PhaseClockState s;
		Check(Near(PhaseClockStep(s, 0.0, 0.0), 0.0), "a zero wall delta advances nothing");
		Check(Near(PhaseClockStep(s, 0.0, -1.0), 0.0), "a negative wall delta advances nothing");
		Check(PhaseClockStep(s, tick, 10.0) <= kPhaseClockMaxStep + 1e-9, "a huge wall delta is capped");
	}

	if (g_failures) {
		std::cerr << g_failures << " failure(s)\n";
		return EXIT_FAILURE;
	}
	std::cout << "camera-shake-phase-clock-math-tests: ok\n";
	return EXIT_SUCCESS;
}
