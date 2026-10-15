// Tests for DeadlockPlaybackSpeedMath.h -- the preset ladder, the clamp, and re-entering the
// ladder from a continuous value.
//
// The case worth having a test for is the last group: a slider sets 0.37x, and the stepper buttons
// then have to carry on from there. Snapping first (which is what the code did before the sliders
// existed) makes a press move to the wrong rung, and in one direction move nowhere at all.

#include "../../AfxHookSource2/Deadlock/DeadlockPlaybackSpeedMath.h"

#include <cmath>
#include <cstdio>

namespace {

int g_failures = 0;

void Check(bool ok, const char* what) {
	if (ok) return;
	std::printf("FAIL: %s\n", what);
	++g_failures;
}

void CheckNear(double got, double want, const char* what) {
	if (std::fabs(got - want) < 1e-9) return;
	std::printf("FAIL: %s -- got %.6f, wanted %.6f\n", what, got, want);
	++g_failures;
}

} // namespace

int main() {
	using namespace Deadlock;

	// --- the ladder itself ---------------------------------------------------------------------
	int count = 0;
	const double* steps = PlaybackSpeed_Steps(count);
	Check(count >= 8, "the ladder has at least the game's eight speeds");
	Check(steps[0] > 0.0, "the slowest rung is positive");
	for (int i = 1; i < count; ++i)
		Check(steps[i] > steps[i - 1], "the ladder ascends");

	bool hasOne = false;
	for (int i = 0; i < count; ++i)
		if (std::fabs(steps[i] - 1.0) < 1e-9) hasOne = true;
	Check(hasOne, "1x is a rung -- it is the one speed a user expects to land on exactly");

	// --- the clamp -----------------------------------------------------------------------------
	CheckNear(PlaybackSpeed_Clamp(1.0), 1.0, "an ordinary speed passes through");
	CheckNear(PlaybackSpeed_Clamp(0.0), 1.0,
	          "zero becomes 1x -- a frozen replay looks exactly like a hang");
	CheckNear(PlaybackSpeed_Clamp(-3.0), 1.0, "a negative speed becomes 1x");
	CheckNear(PlaybackSpeed_Clamp(1e12), 1.0, "an absurd value becomes 1x rather than the cap");
	CheckNear(PlaybackSpeed_Clamp(0.001), PlaybackSpeed_Min(), "below the floor clamps to it");
	CheckNear(PlaybackSpeed_Clamp(99.0), PlaybackSpeed_Max(), "above the ceiling clamps to it");

	// NaN. Written as 0.0/0.0 through a volatile so the compiler cannot fold the comparison away.
	{
		volatile double zero = 0.0;
		const double nan = zero / zero;
		CheckNear(PlaybackSpeed_Clamp(nan), 1.0, "NaN becomes 1x");
	}

	// --- stepping from a rung ------------------------------------------------------------------
	CheckNear(PlaybackSpeed_NextStep(1.0, 1), 1.5, "up from 1x is the next rung");
	CheckNear(PlaybackSpeed_NextStep(1.0, -1), 0.5, "down from 1x is the previous rung");
	CheckNear(PlaybackSpeed_NextStep(steps[count - 1], 1), steps[count - 1],
	          "up at the top is a no-op, never a wrap-around");
	CheckNear(PlaybackSpeed_NextStep(steps[0], -1), steps[0], "down at the bottom is a no-op");
	CheckNear(PlaybackSpeed_NextStep(1.0, 0), 1.0, "a zero direction changes nothing");

	// --- stepping from BETWEEN rungs, which is where a slider leaves it -------------------------
	CheckNear(PlaybackSpeed_NextStep(0.37, 1), 0.5, "up from 0.37x is the first rung above it");
	CheckNear(PlaybackSpeed_NextStep(0.37, -1), 0.25, "down from 0.37x is the first rung below it");
	// Just under a rung. Nearest-rung snapping would answer 1.0 here and then "step up" to 1.0
	// again -- a button that does nothing.
	CheckNear(PlaybackSpeed_NextStep(0.99, 1), 1.0, "up from just under 1x reaches 1x");
	CheckNear(PlaybackSpeed_NextStep(1.01, -1), 1.0, "down from just over 1x reaches 1x");
	CheckNear(PlaybackSpeed_NextStep(0.999999999, 1), 1.5,
	          "a value within the tolerance of 1x counts as ON 1x, so up goes past it");

	// Repeated stepping walks the whole ladder and stops.
	{
		double s = steps[0];
		for (int i = 0; i < count * 2; ++i) s = PlaybackSpeed_NextStep(s, 1);
		CheckNear(s, steps[count - 1], "stepping up repeatedly settles at the top rung");
		for (int i = 0; i < count * 2; ++i) s = PlaybackSpeed_NextStep(s, -1);
		CheckNear(s, steps[0], "stepping down repeatedly settles at the bottom rung");
	}

	// --- naming the nearest rung ----------------------------------------------------------------
	Check(PlaybackSpeed_NearestIndex(1.0) >= 0, "1x names a rung");
	CheckNear(steps[PlaybackSpeed_NearestIndex(1.0)], 1.0, "1x names the 1x rung");
	CheckNear(steps[PlaybackSpeed_NearestIndex(0.26)], 0.25, "0.26x names the 0.25x rung");
	CheckNear(steps[PlaybackSpeed_NearestIndex(1000.0)], steps[count - 1],
	          "an over-cap value names the top rung, because the clamp runs first");

	if (g_failures == 0) std::printf("deadlock playback speed math: all checks passed\n");
	return g_failures == 0 ? 0 : 1;
}
