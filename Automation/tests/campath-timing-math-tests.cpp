// CamPathTimingMath.h: smooth pacing along the camera path (time -> shape axis).
#include "../../AfxHookSource2/Filmmaker/Movie/CamPathTimingMath.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

using namespace Filmmaker;

namespace {
int g_failed = 0;
void Check(bool ok, const char* what) {
	if (!ok) { std::printf("FAIL: %s\n", what); ++g_failed; }
}
// s at absolute time T.
double SAt(const std::vector<double>& t, const std::vector<double>& s, const std::vector<double>& m, double T) {
	int k = 0;
	while (k < (int)t.size() - 2 && T >= t[k + 1]) ++k;
	return CamPathPchipAt(t, s, m, k, (T - t[k]) / (t[k + 1] - t[k]));
}
}

int main() {
	// The user's path (2026-10-10): three markers, the second segment 2.6x the distance of the
	// first in a slightly shorter time. Timing in seconds, shape in units.
	const std::vector<double> t = {0.0, 1.985, 3.766};
	const std::vector<double> s = {0.0, 282.2, 1028.4};
	const auto m = CamPathPchipSlopes(t, s);
	// Speed just before and just after the middle marker: equal (no jolt) -- piecewise-linear
	// pacing jumped from 142 to 419 units/s there.
	const double e = 1e-4;
	const double vBefore = (SAt(t, s, m, t[1]) - SAt(t, s, m, t[1] - e)) / e;
	const double vAfter = (SAt(t, s, m, t[1] + e) - SAt(t, s, m, t[1])) / e;
	Check(std::fabs(vBefore - vAfter) < 1.0, "speed is continuous through the middle marker");
	Check(vBefore > 142.0 && vBefore < 419.0, "the middle speed sits between the two segment speeds");
	// Every marker is still passed at its own time, and the camera never backs up.
	Check(std::fabs(SAt(t, s, m, t[1]) - s[1]) < 1e-6 && std::fabs(SAt(t, s, m, t[2]) - s[2]) < 1e-6, "markers hit on time");
	double prev = -1.0;
	bool monotone = true;
	for (int i = 0; i <= 400; ++i) {
		const double v = SAt(t, s, m, t.back() * i / 400.0);
		if (v < prev - 1e-9) monotone = false;
		prev = v;
	}
	Check(monotone, "the camera never moves backwards along the path");
	// Equal speeds stay exactly linear.
	const std::vector<double> t2 = {0, 1, 2, 3}, s2 = {0, 100, 200, 300};
	const auto m2 = CamPathPchipSlopes(t2, s2);
	Check(std::fabs(SAt(t2, s2, m2, 1.5) - 150.0) < 1e-6 && std::fabs(SAt(t2, s2, m2, 0.25) - 25.0) < 1e-6, "constant speed stays linear");
	// A stop (two coincident markers) holds still and does not overshoot either side.
	const std::vector<double> t3 = {0, 1, 2, 3}, s3 = {0, 100, 100.001, 300};
	const auto m3 = CamPathPchipSlopes(t3, s3);
	Check(SAt(t3, s3, m3, 1.5) >= 100.0 - 1e-6 && SAt(t3, s3, m3, 1.5) <= 100.001 + 1e-6, "a hold stays a hold");
	if (g_failed) return 1;
	std::printf("CamPathTimingMathTests: all checks passed\n");
	return 0;
}
