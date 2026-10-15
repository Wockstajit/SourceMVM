#pragma once

// Engine-free Scene Keys sampling: sparse per-property overrides over a CONFIG baseline.
//
// Semantics (locked):
//   * Before the first and after the last key for a property → baseline (CONFIG), not hold.
//   * One key alone is incomplete (no override span).
//   * Floats: linear or cubic Bezier easing.
//   * Angles: wrapped; per-segment Shortest / Clockwise / CounterClockwise.
//   * Colours: OKLab with separate alpha; one scalar ease curve.
//   * Bool / enum / sky id: hold-left step (no Bezier).
//
// Unit-tested in Automation/tests/scene-keyframe-math-tests.cpp.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace Filmmaker {
namespace SceneKeys {

enum class ChannelType : int {
	Float = 0,
	Angle,
	Colour,   // 3 or 4 components; alpha optional
	Bool,
	Enum,     // integer discrete
	SkyId,    // string discrete (sampled as stepped left key)
};

enum class AngleDir : int {
	Shortest = 0,
	Clockwise = 1,
	CounterClockwise = 2,
};

struct ControlPoint {
	bool active = false;
	double tx = 0.33; // 0..1 fraction toward neighbour
	double dv = 0.0;  // value offset (float/angle/ease only)
};

struct ScalarKey {
	int id = 0;
	double tick = 0.0;
	double value = 0.0;
	ControlPoint left;
	ControlPoint right;
	AngleDir angleDir = AngleDir::Shortest; // used on the segment leaving this key
};

struct ColourKey {
	int id = 0;
	double tick = 0.0;
	double r = 0, g = 0, b = 0, a = 255;
	ControlPoint left;  // ease of blend curve
	ControlPoint right;
};

inline double Clamp01(double t) {
	if (t < 0.0) return 0.0;
	if (t > 1.0) return 1.0;
	return t;
}

inline double Lerp(double a, double b, double t) { return a + (b - a) * t; }

// Cubic Bezier in value space (GraphExpModel-compatible): X(s)=tick fraction solved by bisection.
inline double BezierValue(double a, double b, double t,
	const ControlPoint& ra, const ControlPoint& lb) {
	if (!ra.active && !lb.active) return Lerp(a, b, t);
	const double p0 = a;
	const double p1 = a + (ra.active ? ra.dv : 0.0);
	const double p2 = b + (lb.active ? lb.dv : 0.0);
	const double p3 = b;
	const double x0 = 0.0;
	const double x1 = ra.active ? Clamp01(ra.tx) : (1.0 / 3.0);
	const double x2 = lb.active ? (1.0 - Clamp01(lb.tx)) : (2.0 / 3.0);
	const double x3 = 1.0;
	double lo = 0.0, hi = 1.0, s = t;
	for (int i = 0; i < 24; ++i) {
		const double u = 1.0 - s;
		const double x = u * u * u * x0 + 3 * u * u * s * x1 + 3 * u * s * s * x2 + s * s * s * x3;
		if (x < t) lo = s;
		else hi = s;
		s = 0.5 * (lo + hi);
	}
	const double u = 1.0 - s;
	return u * u * u * p0 + 3 * u * u * s * p1 + 3 * u * s * s * p2 + s * s * s * p3;
}

inline double NormalizeAngleDeg(double deg) {
	while (deg > 180.0) deg -= 360.0;
	while (deg < -180.0) deg += 360.0;
	return deg;
}

inline double AngleDelta(double from, double to, AngleDir dir) {
	const double a = NormalizeAngleDeg(from);
	const double b = NormalizeAngleDeg(to);
	double d = NormalizeAngleDeg(b - a);
	if (dir == AngleDir::Clockwise) {
		if (d <= 0.0) d += 360.0;
		if (d == 0.0 && a != b) d = 360.0;
	} else if (dir == AngleDir::CounterClockwise) {
		if (d >= 0.0) d -= 360.0;
		if (d == 0.0 && a != b) d = -360.0;
	}
	return d;
}

inline double SampleFloatKeys(const std::vector<ScalarKey>& keys, double tick, double baseline,
	bool* outActive) {
	if (outActive) *outActive = false;
	if (keys.size() < 2) return baseline;
	if (tick < keys.front().tick || tick > keys.back().tick) return baseline;
	if (outActive) *outActive = true;
	if (tick <= keys.front().tick) return keys.front().value;
	if (tick >= keys.back().tick) return keys.back().value;
	for (size_t i = 0; i + 1 < keys.size(); ++i) {
		const ScalarKey& a = keys[i];
		const ScalarKey& b = keys[i + 1];
		if (tick < a.tick || tick > b.tick) continue;
		if (b.tick <= a.tick) return a.value;
		const double u = (tick - a.tick) / (b.tick - a.tick);
		return BezierValue(a.value, b.value, Clamp01(u), a.right, b.left);
	}
	return baseline;
}

inline double SampleAngleKeys(const std::vector<ScalarKey>& keys, double tick, double baseline,
	bool* outActive) {
	if (outActive) *outActive = false;
	if (keys.size() < 2) return baseline;
	if (tick < keys.front().tick || tick > keys.back().tick) return baseline;
	if (outActive) *outActive = true;
	for (size_t i = 0; i + 1 < keys.size(); ++i) {
		const ScalarKey& a = keys[i];
		const ScalarKey& b = keys[i + 1];
		if (tick < a.tick || tick > b.tick) continue;
		if (b.tick <= a.tick) return a.value;
		const double u = (tick - a.tick) / (b.tick - a.tick);
		const double ease = BezierValue(0.0, 1.0, Clamp01(u), a.right, b.left);
		return a.value + AngleDelta(a.value, b.value, a.angleDir) * ease;
	}
	return baseline;
}

inline double SampleStepKeys(const std::vector<ScalarKey>& keys, double tick, double baseline,
	bool* outActive) {
	if (outActive) *outActive = false;
	if (keys.size() < 2) return baseline;
	if (tick < keys.front().tick || tick > keys.back().tick) return baseline;
	if (outActive) *outActive = true;
	double v = keys.front().value;
	for (const auto& k : keys) {
		if (tick < k.tick) break;
		v = k.value;
	}
	return v;
}

// OKLab helpers (Björn Ottosson).
inline void RgbToLinear(double r, double g, double b, double& lr, double& lg, double& lb) {
	auto f = [](double c) {
		c /= 255.0;
		return (c <= 0.04045) ? (c / 12.92) : std::pow((c + 0.055) / 1.055, 2.4);
	};
	lr = f(r); lg = f(g); lb = f(b);
}

inline void LinearToRgb(double lr, double lg, double lb, double& r, double& g, double& b) {
	auto f = [](double c) {
		c = (c <= 0.0031308) ? (12.92 * c) : (1.055 * std::pow(c, 1.0 / 2.4) - 0.055);
		return Clamp01(c) * 255.0;
	};
	r = f(lr); g = f(lg); b = f(lb);
}

inline void LinearToOklab(double lr, double lg, double lb, double& L, double& a, double& b) {
	const double l = std::cbrt(0.4122214708 * lr + 0.5363325363 * lg + 0.0514459929 * lb);
	const double m = std::cbrt(0.2119034982 * lr + 0.6806995451 * lg + 0.1073969566 * lb);
	const double s = std::cbrt(0.0883024619 * lr + 0.2817188376 * lg + 0.6299787005 * lb);
	L = 0.2104542553 * l + 0.7936177850 * m - 0.0040720468 * s;
	a = 1.9779984951 * l - 2.4285922050 * m + 0.4505937099 * s;
	b = 0.0259040371 * l + 0.7827717662 * m - 0.8086757660 * s;
}

inline void OklabToLinear(double L, double a, double b, double& lr, double& lg, double& lb) {
	const double l_ = L + 0.3963377774 * a + 0.2158037573 * b;
	const double m_ = L - 0.1055613458 * a - 0.0638541728 * b;
	const double s_ = L - 0.0894841775 * a - 1.2914855480 * b;
	const double l = l_ * l_ * l_;
	const double m = m_ * m_ * m_;
	const double s = s_ * s_ * s_;
	lr = +4.0767416621 * l - 3.3077115913 * m + 0.2309699292 * s;
	lg = -1.2684380046 * l + 2.6097574011 * m - 0.3413193965 * s;
	lb = -0.0041960863 * l - 0.7034186147 * m + 1.7076147010 * s;
}

inline void LerpOklabRgb(double r0, double g0, double b0, double r1, double g1, double b1, double t,
	double& r, double& g, double& b) {
	double lr0, lg0, lb0, lr1, lg1, lb1;
	RgbToLinear(r0, g0, b0, lr0, lg0, lb0);
	RgbToLinear(r1, g1, b1, lr1, lg1, lb1);
	double L0, a0, bb0, L1, a1, bb1;
	LinearToOklab(lr0, lg0, lb0, L0, a0, bb0);
	LinearToOklab(lr1, lg1, lb1, L1, a1, bb1);
	double lr, lg, lb;
	OklabToLinear(Lerp(L0, L1, t), Lerp(a0, a1, t), Lerp(bb0, bb1, t), lr, lg, lb);
	LinearToRgb(lr, lg, lb, r, g, b);
}

struct ColourSample {
	double r = 0, g = 0, b = 0, a = 255;
	bool active = false;
};

inline ColourSample SampleColourKeys(const std::vector<ColourKey>& keys, double tick,
	double br, double bg, double bb, double ba) {
	ColourSample out{ br, bg, bb, ba, false };
	if (keys.size() < 2) return out;
	if (tick < keys.front().tick || tick > keys.back().tick) return out;
	out.active = true;
	for (size_t i = 0; i + 1 < keys.size(); ++i) {
		const ColourKey& a = keys[i];
		const ColourKey& b = keys[i + 1];
		if (tick < a.tick || tick > b.tick) continue;
		if (b.tick <= a.tick) {
			out.r = a.r; out.g = a.g; out.b = a.b; out.a = a.a;
			return out;
		}
		const double u = (tick - a.tick) / (b.tick - a.tick);
		const double ease = BezierValue(0.0, 1.0, Clamp01(u), a.right, b.left);
		LerpOklabRgb(a.r, a.g, a.b, b.r, b.g, b.b, ease, out.r, out.g, out.b);
		out.a = Lerp(a.a, b.a, ease);
		return out;
	}
	return out;
}

// Event ticks: union of all property key ticks (timeline diamonds).
inline void CollectEventTicks(const std::vector<double>& propertyTicks, std::vector<double>& out) {
	out = propertyTicks;
	std::sort(out.begin(), out.end());
	out.erase(std::unique(out.begin(), out.end(), [](double a, double b) {
		return std::fabs(a - b) < 1e-9;
	}), out.end());
}

} // namespace SceneKeys
} // namespace Filmmaker
