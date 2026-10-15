#include "../../AfxHookSource2/Filmmaker/Movie/SmoothViewAirMotionMath.h"
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

bool Near(double a, double b, double epsilon = 1e-6) {
	return std::fabs(a - b) <= epsilon;
}

}
int main() {
	SmoothViewAirMotionState air;
	auto airResult = SmoothViewUpdateAirMotion(
		air, 500.0, false, true, 0.5, true, 0.5, 1.0 / 60.0, true);
	Check(airResult.targetPitch < -4.99, "jump target preserves the configured five-degree kick");
	Check(airResult.pitch < 0.0 && airResult.pitch > airResult.targetPitch,
		"jump pitch eases in instead of appearing in one frame");

	const double firstJumpPitch = airResult.pitch;
	airResult = SmoothViewUpdateAirMotion(
		air, 500.0, false, true, 0.5, true, 0.5, 0.0, false);
	Check(Near(airResult.pitch, firstJumpPitch), "duplicate airborne application is idempotent");

	for (int i = 0; i < 60; ++i) {
		airResult = SmoothViewUpdateAirMotion(
			air, 500.0, false, true, 0.5, true, 0.5, 1.0 / 60.0, true);
	}
	Check(std::fabs(airResult.pitch - airResult.targetPitch) < 0.01,
		"held jump velocity converges smoothly to its target");

	const double beforeLanding = airResult.pitch;
	airResult = SmoothViewUpdateAirMotion(
		air, 0.0, true, true, 0.5, true, 0.5, 1.0 / 60.0, true);
	Check(airResult.pitch > beforeLanding && airResult.pitch < 0.0,
		"landing returns toward neutral without a one-frame snap");

	// JUMP KICK: a take-off jolts the view up, a hard landing dips it down, and both settle.
	{
		SmoothViewAirMotionState k;
		SmoothViewUpdateAirMotion(k, 0.0, true, true, 0.5, true, 1.0, 1.0 / 60.0, true); // standing
		double minPitch = 0.0;
		auto r = SmoothViewUpdateAirMotion(k, 300.0, false, true, 0.5, true, 1.0, 1.0 / 60.0, true); // take-off
		for (int i = 0; i < 12; ++i) {
			r = SmoothViewUpdateAirMotion(k, 300.0, false, true, 0.5, true, 1.0, 1.0 / 60.0, true);
			minPitch = std::fmin(minPitch, r.kick);
		}
		Check(minPitch < -2.0, "a take-off kicks the view up by a visible amount");
		for (int i = 0; i < 30; ++i) r = SmoothViewUpdateAirMotion(k, -550.0, false, true, 0.5, true, 1.0, 1.0 / 60.0, true);
		double maxKick = 0.0;
		r = SmoothViewUpdateAirMotion(k, 0.0, true, true, 0.5, true, 1.0, 1.0 / 60.0, true); // hard landing
		for (int i = 0; i < 12; ++i) {
			r = SmoothViewUpdateAirMotion(k, 0.0, true, true, 0.5, true, 1.0, 1.0 / 60.0, true);
			maxKick = std::fmax(maxKick, r.kick);
		}
		Check(maxKick > 4.0, "a hard landing dips the view down by a visible amount");
		for (int i = 0; i < 120; ++i) r = SmoothViewUpdateAirMotion(k, 0.0, true, true, 0.5, true, 1.0, 1.0 / 60.0, true);
		Check(std::fabs(r.pitch) < 0.01, "the kick settles back to level on the ground");
		// Switched off: no kick at all.
		SmoothViewAirMotionState off;
		SmoothViewUpdateAirMotion(off, 0.0, true, true, 0.5, false, 1.0, 1.0 / 60.0, true);
		auto o = SmoothViewUpdateAirMotion(off, 300.0, false, true, 0.5, false, 1.0, 1.0 / 60.0, true);
		for (int i = 0; i < 10; ++i) o = SmoothViewUpdateAirMotion(off, 300.0, false, true, 0.5, false, 1.0, 1.0 / 60.0, true);
		Check(o.kick == 0.0 && o.pitch == 0.0, "Jump kick off: no kick, no rise pitch");
		// Starting mid-air (a seek into a jump) is not a take-off.
		SmoothViewAirMotionState seek;
		auto s = SmoothViewUpdateAirMotion(seek, 300.0, false, true, 0.5, true, 1.0, 1.0 / 60.0, true);
		Check(s.kick == 0.0, "a seek into the air does not kick");
	}

	std::cout << "SmoothViewAirMotionMathTests: all checks passed\n";
	return 0;
}
