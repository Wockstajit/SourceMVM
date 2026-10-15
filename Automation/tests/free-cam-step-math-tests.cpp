// Behavior tests for Movie/FreeCamStepMath.h: one wheel notch of free-cam speed.
#include "../../AfxHookSource2/Filmmaker/Movie/FreeCamStepMath.h"

#include <cmath>
#include <cstdio>

using namespace Filmmaker;

namespace {
int g_failures = 0;
void Check(bool ok, const char* what) {
	if (!ok) { std::printf("FAIL: %s\n", what); ++g_failures; }
}
bool Near(double a, double b) { return std::fabs(a - b) < 1e-9; }
}

int main() {
	Check(Near(NextFreeCamSpeed(1.0, +1), 1.5), "one notch up adds 0.5x");
	Check(Near(NextFreeCamSpeed(1.5, -1), 1.0), "one notch down removes 0.5x");
	Check(Near(NextFreeCamSpeed(1.0, 0), 1.0), "no notch, no change");
	// A speed off the grid (the old multiplicative steps, or a console value) snaps onto it.
	Check(Near(NextFreeCamSpeed(1.22, +1), 1.5), "off-grid speed rounds up onto the 0.5 grid");
	Check(Near(NextFreeCamSpeed(1.22, -1), 1.0), "off-grid speed rounds down onto the 0.5 grid");
	// Below 0.5x the steps halve so slow pushes stay reachable, and climb back the same way.
	Check(Near(NextFreeCamSpeed(0.5, -1), 0.25), "below 0.5x a notch halves");
	Check(Near(NextFreeCamSpeed(0.25, -1), 0.125), "halving continues to 0.125x");
	Check(Near(NextFreeCamSpeed(0.125, -1), 0.125), "0.125x is the floor");
	Check(Near(NextFreeCamSpeed(0.125, +1), 0.25), "climbing from the floor doubles");
	Check(Near(NextFreeCamSpeed(0.25, +1), 0.5), "and rejoins the 0.5 grid");
	Check(Near(NextFreeCamSpeed(256.0, +1), 256.0), "256x is the ceiling");
	// Ten notches up from 1x is a round, predictable 6x.
	double s = 1.0;
	for (int i = 0; i < 10; ++i) s = NextFreeCamSpeed(s, +1);
	Check(Near(s, 6.0), "ten notches from 1x is 6x");
	if (g_failures) { std::printf("%d failure(s)\n", g_failures); return 1; }
	std::printf("free-cam-step-math-tests: all passed\n");
	return 0;
}
