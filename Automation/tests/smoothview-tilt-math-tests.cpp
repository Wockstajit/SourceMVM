#include "../../AfxHookSource2/Filmmaker/Movie/SmoothViewTiltMath.h"

#include <cmath>
#include <cstdlib>
#include <iostream>

using Filmmaker::SmoothViewTiltTarget;
using Filmmaker::kSmoothViewTiltFullSpeed;

namespace {

int g_failures = 0;

void Check(bool ok, const char *message) {
	if (!ok) {
		std::cerr << "FAIL: " << message << '\n';
		++g_failures;
	}
}

// MEASURED in game (free camera at roll +10 vs 0, 2026-09-30): positive roll leans the view RIGHT.
bool LeansLeft(double target) { return target < 0; }
bool LeansRight(double target) { return target > 0; }

} // namespace

int main() {
	const double factor = 40.0; // the shipped default

	// THE BUG, reported 2026-09-19 and again 2026-09-30: moving left leaned the view right.
	// lateralSpeed is velocity . Right, so moving LEFT is negative.
	Check(LeansLeft(SmoothViewTiltTarget(-250.0, factor, false)),
	      "strafing left leans the view left");
	Check(LeansRight(SmoothViewTiltTarget(250.0, factor, false)),
	      "strafing right leans the view right");

	// Standing still, and pure forward/back (which the caller projects to exactly zero).
	Check(SmoothViewTiltTarget(0.0, factor, false) == 0.0,
	      "no lateral motion means no lean");

	// Symmetry: equal and opposite strafes lean equally and oppositely.
	{
		const double left = SmoothViewTiltTarget(-180.0, factor, false);
		const double right = SmoothViewTiltTarget(180.0, factor, false);
		Check(std::fabs(left + right) < 1e-12, "left and right lean symmetrically");
	}

	// Diagonal movement leans by its lateral component alone, so forward+left leans LESS than
	// left alone. That is the Quake behaviour the effect is imitating.
	{
		const double full = std::fabs(SmoothViewTiltTarget(-250.0, factor, false));
		const double diagonal = std::fabs(SmoothViewTiltTarget(-250.0 * 0.7071, factor, false));
		Check(diagonal < full, "a diagonal strafe leans less than a pure one");
		Check(diagonal > full * 0.5, "a diagonal strafe still leans substantially");
	}

	// Magnitude: the mod's radians-as-degrees convention, ~1.40 degrees at factor 40.
	{
		const double lean = std::fabs(SmoothViewTiltTarget(-kSmoothViewTiltFullSpeed, 40.0, false));
		Check(lean > 1.3 && lean < 1.5, "factor 40 yields about 1.4 degrees, matching the mod");
	}

	// Saturation: beyond the full-speed point the lean stops growing.
	{
		const double at = std::fabs(SmoothViewTiltTarget(-kSmoothViewTiltFullSpeed, factor, false));
		const double past = std::fabs(SmoothViewTiltTarget(-4000.0, factor, false));
		Check(std::fabs(at - past) < 1e-12, "lean saturates at the full-speed threshold");
	}

	// Below it the lean scales with speed, so walking leans less than running.
	{
		const double walk = std::fabs(SmoothViewTiltTarget(-120.0, factor, false));
		const double run = std::fabs(SmoothViewTiltTarget(-250.0, factor, false));
		Check(walk < run, "a slower strafe leans less");
	}

	// Invert is the escape hatch, and must be exactly the opposite.
	Check(LeansRight(SmoothViewTiltTarget(-250.0, factor, true)),
	      "invert leans the other way");
	Check(std::fabs(SmoothViewTiltTarget(-250.0, factor, true)
	                + SmoothViewTiltTarget(-250.0, factor, false)) < 1e-12,
	      "invert is exactly the negation");

	// A zero factor disables the lean without disabling the effect.
	Check(SmoothViewTiltTarget(-250.0, 0.0, false) == 0.0, "factor 0 means no lean");

	if (g_failures) {
		std::cerr << g_failures << " failure(s)\n";
		return EXIT_FAILURE;
	}
	std::cout << "smoothview-tilt-math-tests: ok\n";
	return EXIT_SUCCESS;
}
