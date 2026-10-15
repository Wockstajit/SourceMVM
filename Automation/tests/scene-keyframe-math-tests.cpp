#include "../../AfxHookSource2/Filmmaker/Movie/SceneKeyframeMath.h"

#include <cmath>
#include <iostream>
#include <vector>

using namespace Filmmaker::SceneKeys;

namespace {

int g_failed = 0;

void ExpectNear(double actual, double expected, double eps, const char* label) {
	if (std::fabs(actual - expected) <= eps) return;
	std::cerr << "FAILED " << label << ": got " << actual << ", expected " << expected << "\n";
	++g_failed;
}

void ExpectTrue(bool v, const char* label) {
	if (v) return;
	std::cerr << "FAILED " << label << "\n";
	++g_failed;
}

} // namespace

int main() {
	std::vector<ScalarKey> empty;
	bool active = true;
	ExpectNear(SampleFloatKeys(empty, 10, 42.0, &active), 42.0, 1e-9, "empty → baseline");
	ExpectTrue(!active, "empty not active");

	std::vector<ScalarKey> one{ {1, 100.0, 5.0} };
	ExpectNear(SampleFloatKeys(one, 100, 1.0, &active), 1.0, 1e-9, "one key → baseline");
	ExpectTrue(!active, "one key not active");

	std::vector<ScalarKey> two{
		{1, 100.0, 0.0},
		{2, 200.0, 10.0},
	};
	ExpectNear(SampleFloatKeys(two, 50, 7.0, &active), 7.0, 1e-9, "before span → baseline");
	ExpectTrue(!active, "before not active");
	ExpectNear(SampleFloatKeys(two, 250, 7.0, &active), 7.0, 1e-9, "after span → baseline");
	ExpectTrue(!active, "after not active");
	ExpectNear(SampleFloatKeys(two, 150, 7.0, &active), 5.0, 1e-6, "mid linear");
	ExpectTrue(active, "mid active");

	std::vector<ScalarKey> ang{
		{1, 0.0, 10.0},
		{2, 100.0, 350.0},
	};
	ang[0].angleDir = AngleDir::Shortest;
	ExpectNear(SampleAngleKeys(ang, 50, 0.0, &active), 0.0, 1e-6, "shortest 10→350 midpoint ≈ 0");

	ang[0].angleDir = AngleDir::Clockwise;
	const double cw = SampleAngleKeys(ang, 50, 0.0, &active);
	ExpectTrue(cw > 10.0 && cw < 350.0, "clockwise mid stays positive path");

	std::vector<ScalarKey> step{
		{1, 10.0, 0.0},
		{2, 20.0, 1.0},
	};
	ExpectNear(SampleStepKeys(step, 15, 9.0, &active), 0.0, 1e-9, "step hold-left");
	ExpectNear(SampleStepKeys(step, 20, 9.0, &active), 1.0, 1e-9, "step at key");
	ExpectNear(SampleStepKeys(step, 5, 9.0, &active), 9.0, 1e-9, "step outside");

	std::vector<ColourKey> cols{
		{1, 0.0, 255, 0, 0, 255},
		{2, 100.0, 0, 0, 255, 255},
	};
	const ColourSample mid = SampleColourKeys(cols, 50, 0, 0, 0, 0);
	ExpectTrue(mid.active, "colour mid active");
	// OKLab red→blue midpoint is not muddy grey (R≈G≈B).
	ExpectTrue(!(std::fabs(mid.r - mid.g) < 8.0 && std::fabs(mid.g - mid.b) < 8.0),
		"OKLab midpoint is not grey mud");

	std::vector<double> ticks{ 200.0, 100.0, 100.0, 150.0 };
	std::vector<double> unionTicks;
	CollectEventTicks(ticks, unionTicks);
	ExpectTrue(unionTicks.size() == 3, "union tick count");
	ExpectNear(unionTicks[0], 100.0, 1e-9, "union sorted 0");
	ExpectNear(unionTicks[2], 200.0, 1e-9, "union sorted 2");

	if (g_failed) {
		std::cerr << g_failed << " check(s) failed\n";
		return 1;
	}
	std::cout << "SceneKeyframeMath tests passed.\n";
	return 0;
}
