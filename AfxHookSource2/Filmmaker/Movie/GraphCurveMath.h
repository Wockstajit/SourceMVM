#pragma once

// ONE CURVE FOR THE GRAPH AND THE CAMERA. The graph dock edits each camera channel (x, y, z, pitch,
// yaw, roll, fov) as cubic Bezier segments in TICK space: a key's left/right handle is a fraction
// `tx` of the segment toward the neighbour and a value offset `dv` (GraphExpModel.h). The camera
// path used to fly its own natural-cubic spline through the markers and ignore those handles, so
// easing a curve in the graph did nothing to the camera. Markers now carry the handles
// (CamMarker::in/out), and the path evaluates them with exactly these functions, so the curve the
// graph draws is the curve the camera flies -- and the movie renders.
//
// Engine-free and header-only: Automation/tests/graph-curve-math-tests.cpp pins it, and both
// GraphExpModel (the graph) and CamPathEval (the camera) call it.

#include <cmath>

namespace Filmmaker {
namespace GraphCurve {

struct Handle {
	bool active = false; // false => this side of the key is linear
	double tx = 1.0 / 3.0; // 0..1 fraction of the segment toward the neighbour
	double dv = 0.0;       // value offset from the key's value
};

inline double Clamp01(double v) { return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v); }

inline double Bez(double a0, double a1, double a2, double a3, double s) {
	const double u = 1.0 - s;
	return u * u * u * a0 + 3.0 * u * u * s * a1 + 3.0 * u * s * s * a2 + s * s * s * a3;
}

// The four control points of the segment from key A to key B.
struct Segment { double x[4], y[4]; };
inline Segment MakeSegment(double t0, double v0, const Handle& outA, double t1, double v1, const Handle& inB) {
	const double span = t1 - t0;
	const double txR = Clamp01(outA.active ? outA.tx : 1.0 / 3.0);
	const double txL = Clamp01(inB.active ? inB.tx : 1.0 / 3.0);
	Segment s;
	s.x[0] = t0;               s.y[0] = v0;
	s.x[1] = t0 + txR * span;  s.y[1] = v0 + (outA.active ? outA.dv : 0.0);
	s.x[2] = t1 - txL * span;  s.y[2] = v1 + (inB.active ? inB.dv : 0.0);
	s.x[3] = t1;               s.y[3] = v1;
	return s;
}

// The Bezier parameter whose X is `t` (X is monotonic for handles inside the segment).
inline double SolveS(const Segment& g, double t) {
	double lo = 0.0, hi = 1.0, s = 0.5;
	for (int i = 0; i < 40; ++i) {
		s = 0.5 * (lo + hi);
		if (Bez(g.x[0], g.x[1], g.x[2], g.x[3], s) < t) lo = s; else hi = s;
	}
	return s;
}

// The value between two keys at tick t: linear when neither inner handle is active, else the
// cubic Bezier (GraphExpModel::SampleChannel's rule).
inline double SegmentValue(double t0, double v0, const Handle& outA, double t1, double v1, const Handle& inB, double t) {
	if (t1 - t0 <= 1e-9) return v1;
	if (!outA.active && !inB.active) {
		const double f = (t - t0) / (t1 - t0);
		return v0 + Clamp01(f) * (v1 - v0);
	}
	const Segment g = MakeSegment(t0, v0, outA, t1, v1, inB);
	return Bez(g.y[0], g.y[1], g.y[2], g.y[3], SolveS(g, t));
}

// Split the segment A->B at tick t (exact, de Casteljau). The point on the curve becomes a key
// with handles `inK` / `outK`; A's outgoing and B's incoming handles are rewritten for the two
// shorter halves. The two halves together are the same curve as before -- this is how a channel
// without a key at a marker's tick keeps its shape when the marker is baked.
struct Split { double value; Handle outA, inK, outK, inB; };
inline Split SplitAt(double t0, double v0, const Handle& outA, double t1, double v1, const Handle& inB, double t) {
	Split r;
	r.outA = outA; r.inB = inB;
	if (!outA.active && !inB.active) { // a straight segment splits into two straight ones
		r.value = SegmentValue(t0, v0, outA, t1, v1, inB, t);
		return r;
	}
	const Segment g = MakeSegment(t0, v0, outA, t1, v1, inB);
	const double s = SolveS(g, t);
	auto lerp = [](double a, double b, double f) { return a + (b - a) * f; };
	const double x01 = lerp(g.x[0], g.x[1], s), y01 = lerp(g.y[0], g.y[1], s);
	const double x12 = lerp(g.x[1], g.x[2], s), y12 = lerp(g.y[1], g.y[2], s);
	const double x23 = lerp(g.x[2], g.x[3], s), y23 = lerp(g.y[2], g.y[3], s);
	const double x012 = lerp(x01, x12, s), y012 = lerp(y01, y12, s);
	const double x123 = lerp(x12, x23, s), y123 = lerp(y12, y23, s);
	const double xk = lerp(x012, x123, s), yk = lerp(y012, y123, s);
	r.value = yk;
	const double spanL = xk - t0, spanR = t1 - xk;
	if (spanL > 1e-9) {
		r.outA.active = true; r.outA.tx = Clamp01((x01 - t0) / spanL); r.outA.dv = y01 - v0;
		r.inK.active = true;  r.inK.tx = Clamp01((xk - x012) / spanL); r.inK.dv = y012 - yk;
	}
	if (spanR > 1e-9) {
		r.outK.active = true; r.outK.tx = Clamp01((x123 - xk) / spanR); r.outK.dv = y123 - yk;
		r.inB.active = true;  r.inB.tx = Clamp01((t1 - x23) / spanR); r.inB.dv = y23 - v1;
	}
	return r;
}

// Catmull-Rom auto tangent for key i of n (one-sided at the ends), as Bezier handles at 1/3
// influence on the same tangent line: the graph's "Smooth".
inline void AutoTangent(const double* ticks, const double* values, int n, int i, Handle& in, Handle& out) {
	double slope = 0.0;
	if (i > 0 && i < n - 1) {
		const double dt = ticks[i + 1] - ticks[i - 1];
		if (dt > 1e-9) slope = (values[i + 1] - values[i - 1]) / dt;
	} else if (i == 0 && n > 1) {
		const double dt = ticks[1] - ticks[0];
		if (dt > 1e-9) slope = (values[1] - values[0]) / dt;
	} else if (i == n - 1 && n > 1) {
		const double dt = ticks[n - 1] - ticks[n - 2];
		if (dt > 1e-9) slope = (values[n - 1] - values[n - 2]) / dt;
	}
	out = Handle(); in = Handle();
	if (i < n - 1) { out.active = true; out.tx = 1.0 / 3.0; out.dv = slope * (ticks[i + 1] - ticks[i]) / 3.0; }
	if (i > 0) { in.active = true; in.tx = 1.0 / 3.0; in.dv = -slope * (ticks[i] - ticks[i - 1]) / 3.0; }
}

// Angles take the short way round: the next key's value moved by whole turns to within half a
// turn of `prev`. Handles are offsets, so they move with it.
inline double Unwrap(double prev, double v) {
	while (v - prev > 180.0) v -= 360.0;
	while (v - prev < -180.0) v += 360.0;
	return v;
}
inline bool IsAngleChannel(int ch) { return ch >= 3 && ch <= 5; } // pitch, yaw, roll

} // namespace GraphCurve
} // namespace Filmmaker
