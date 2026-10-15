// Three-key smoothing regression tests for the shared cubic interpolators.
//
// "Smooth" on a camera path used to need four markers: CCubicDoubleInterpolation,
// CSCubicQuaternionInterpolation and qspline_init all refused fewer, so a three-marker
// path silently fell back to Linear geometry while the UI still said Smooth. The
// underlying solvers do handle three points (one interior unknown, empty elimination
// and back-substitution loops), so the guards are now >= 3. These tests pin that:
// three keys must evaluate, must pass through the keys, and must actually bend.

#include "../../Shared/AfxMath.h"

#include <cmath>
#include <cstdio>

using namespace Afx::Math;

namespace {

int g_failures = 0;

void Check(bool ok, const char* what) {
	if (!ok) {
		++g_failures;
		std::printf("FAIL: %s\n", what);
	}
}

void CheckNear(double got, double want, double tol, const char* what) {
	if (!(std::fabs(got - want) <= tol)) {
		++g_failures;
		std::printf("FAIL: %s (got %.9f, want %.9f +/- %g)\n", what, got, want, tol);
	}
}

double SelectDouble(double const& v) { return v; }
Quaternion SelectQuaternion(Quaternion const& v) { return v; }

// Yaw-only rotation, which is what a camera path's turn mostly is.
Quaternion YawQuat(double degrees) {
	const double half = degrees * (3.14159265358979323846 / 180.0) * 0.5;
	return Quaternion(std::cos(half), 0.0, 0.0, std::sin(half)).Normalized();
}

void TestDoubleNeedsThreeNotFour() {
	CInterpolationMap<double> map;
	CInterpolationMapView<double, double> view(&map, SelectDouble);
	CCubicDoubleInterpolation<double> interp(&view);

	map[0.0] = 0.0;
	interp.InterpolationMapChanged();
	Check(!interp.CanEval(), "one key cannot evaluate a cubic");

	map[1.0] = 1.0;
	interp.InterpolationMapChanged();
	Check(!interp.CanEval(), "two keys cannot evaluate a cubic");

	map[2.0] = 0.0;
	interp.InterpolationMapChanged();
	Check(interp.CanEval(), "three keys can evaluate a cubic");
}

void TestDoubleThreeKeysBend() {
	CInterpolationMap<double> map;
	map[0.0] = 0.0;
	map[1.0] = 1.0;
	map[2.0] = 0.0;

	CInterpolationMapView<double, double> view(&map, SelectDouble);
	CCubicDoubleInterpolation<double> interp(&view);

	CheckNear(interp.Eval(0.0), 0.0, 1e-9, "cubic passes through first key");
	CheckNear(interp.Eval(1.0), 1.0, 1e-9, "cubic passes through middle key");
	CheckNear(interp.Eval(2.0), 0.0, 1e-9, "cubic passes through last key");

	// The point of Smooth: the curve must leave the straight chord between keys.
	// Linear would put t=0.25 dead on 0.25.
	const double quarter = interp.Eval(0.25);
	Check(std::fabs(quarter - 0.25) > 0.05, "three-key cubic leaves the chord between keys");

	// Symmetric keys give a symmetric curve, so the solver is not lopsided on the one
	// interior unknown that three keys produce.
	CheckNear(interp.Eval(0.25), interp.Eval(1.75), 1e-9, "three-key cubic is symmetric");
	CheckNear(interp.Eval(0.75), interp.Eval(1.25), 1e-9, "three-key cubic is symmetric past the middle key");
}

// The interpolators are built with clamped (zero-derivative) end conditions, so a path
// eases out of its first key and into its last one. Three keys must get the SAME end
// conditions four keys already had -- that is what makes the shorter path feel like the
// longer one instead of like a Linear leg with curved neighbours.
double StartSlope(CCubicDoubleInterpolation<double>& interp) {
	const double dt = 1e-4;
	return (interp.Eval(dt) - interp.Eval(0.0)) / dt;
}

void TestDoubleThreeKeysEaseLikeFour() {
	CInterpolationMap<double> three;
	three[0.0] = 0.0;
	three[1.0] = 2.0;
	three[2.0] = 4.0;
	CInterpolationMapView<double, double> threeView(&three, SelectDouble);
	CCubicDoubleInterpolation<double> threeInterp(&threeView);

	CInterpolationMap<double> four(three);
	four[3.0] = 6.0;
	CInterpolationMapView<double, double> fourView(&four, SelectDouble);
	CCubicDoubleInterpolation<double> fourInterp(&fourView);

	// Chord slope is 2.0; both must start far below it rather than running straight.
	const double threeSlope = StartSlope(threeInterp);
	const double fourSlope = StartSlope(fourInterp);
	Check(threeSlope < 0.5, "three-key cubic eases out of its first key");
	Check(fourSlope < 0.5, "four-key cubic eases out of its first key");
	CheckNear(threeSlope, fourSlope, 0.35, "three keys ease like four rather than running linear");

	// Collinear keys still advance monotonically; the ease must not fold the path back.
	double prev = threeInterp.Eval(0.0);
	for (int i = 1; i <= 40; ++i) {
		const double v = threeInterp.Eval(2.0 * i / 40.0);
		Check(v >= prev - 1e-9, "three-key cubic advances monotonically over collinear keys");
		prev = v;
	}
}

void TestQuaternionNeedsThreeNotFour() {
	CInterpolationMap<Quaternion> map;
	CInterpolationMapView<Quaternion, Quaternion> view(&map, SelectQuaternion);
	CSCubicQuaternionInterpolation<Quaternion> interp(&view);

	map[0.0] = YawQuat(0.0);
	map[1.0] = YawQuat(30.0);
	interp.InterpolationMapChanged();
	Check(!interp.CanEval(), "two keys cannot evaluate a quaternion cubic");

	map[2.0] = YawQuat(120.0);
	interp.InterpolationMapChanged();
	Check(interp.CanEval(), "three keys can evaluate a quaternion cubic");
}

void TestQuaternionThreeKeysInterpolate() {
	CInterpolationMap<Quaternion> map;
	map[0.0] = YawQuat(0.0);
	map[1.0] = YawQuat(30.0);
	map[2.0] = YawQuat(120.0);

	CInterpolationMapView<Quaternion, Quaternion> view(&map, SelectQuaternion);
	CSCubicQuaternionInterpolation<Quaternion> interp(&view);

	// qspline_init must solve its single interior rate without throwing, and the curve
	// must still hit the keys it was built from.
	for (int i = 0; i < 3; ++i) {
		const double t = (double)i;
		const Quaternion got = interp.Eval(t);
		const Quaternion want = map[t];
		CheckNear(std::fabs(DotProduct(got, want)), 1.0, 1e-6, "quaternion cubic passes through its keys");
		CheckNear(got.Norm(), 1.0, 1e-6, "quaternion cubic stays normalized");
	}

	// The eased approach into the 30 -> 120 degree acceleration must not match the
	// constant-rate slerp of the first span; that equality is what Linear looks like.
	const Quaternion mid = interp.Eval(0.5);
	const Quaternion slerpMid = YawQuat(0.0).Slerp(YawQuat(30.0), 0.5);
	Check(std::fabs(DotProduct(mid, slerpMid)) < 1.0 - 1e-6, "three-key quaternion cubic differs from slerp");
}

} // namespace

int main() {
	TestDoubleNeedsThreeNotFour();
	TestDoubleThreeKeysBend();
	TestDoubleThreeKeysEaseLikeFour();
	TestQuaternionNeedsThreeNotFour();
	TestQuaternionThreeKeysInterpolate();

	if (0 != g_failures) {
		std::printf("cubic-min-keys-math-tests: %d failure(s)\n", g_failures);
		return 1;
	}

	std::printf("cubic-min-keys-math-tests: all tests passed\n");
	return 0;
}
