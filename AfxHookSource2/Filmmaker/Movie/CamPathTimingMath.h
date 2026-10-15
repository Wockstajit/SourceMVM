#pragma once

// Smooth PACING along a camera path. Engine-free so Automation/tests/campath-timing-math-tests.cpp
// can pin it.
//
// The path's SHAPE is keyed by cumulative chord length s (CamPathEval: the curve stays put when the
// speed changes). Playback maps time to s. Mapping it piecewise-linearly per segment made the
// camera's speed constant inside each segment and jump at every marker -- with "Smooth" selected
// the curve bent, but the motion still lurched at the keys, which reads as linear (user report
// 2026-10-10: three markers whose second segment covers 2.6x the distance of the first in a
// similar time). Here time maps to s through a monotone cubic Hermite (PCHIP, Fritsch-Carlson):
// speed changes smoothly through every marker, the camera never backs up, three markers are enough,
// and equal speeds stay exactly linear.

#include <cmath>
#include <vector>

namespace Filmmaker {

// Slopes ds/dt at each knot (t strictly increasing, s non-decreasing).
inline std::vector<double> CamPathPchipSlopes(const std::vector<double>& t, const std::vector<double>& s) {
	const int n = (int)t.size();
	std::vector<double> m(n, 0.0);
	if (n < 2 || (int)s.size() != n) return m;
	std::vector<double> d(n - 1), delta(n - 1);
	for (int k = 0; k < n - 1; ++k) {
		d[k] = t[k + 1] - t[k];
		delta[k] = d[k] > 1e-12 ? (s[k + 1] - s[k]) / d[k] : 0.0;
	}
	if (n == 2) { m[0] = m[1] = delta[0]; return m; }
	for (int i = 1; i < n - 1; ++i) {
		const double a = delta[i - 1], b = delta[i];
		if (a * b <= 0.0) { m[i] = 0.0; continue; }
		const double w1 = 2.0 * d[i] + d[i - 1], w2 = d[i] + 2.0 * d[i - 1];
		m[i] = (w1 + w2) / (w1 / a + w2 / b);
	}
	// One-sided three-point ends, kept monotone.
	auto end = [](double d0, double d1, double del0, double del1) {
		double v = ((2.0 * d0 + d1) * del0 - d0 * del1) / (d0 + d1);
		if (v * del0 <= 0.0) v = 0.0;
		else if (del0 * del1 <= 0.0 && std::fabs(v) > std::fabs(3.0 * del0)) v = 3.0 * del0;
		return v;
	};
	m[0] = end(d[0], d[1], delta[0], delta[1]);
	m[n - 1] = end(d[n - 2], d[n - 3 >= 0 ? n - 3 : 0], delta[n - 2], delta[n - 3 >= 0 ? n - 3 : 0]);
	return m;
}

// s on segment k at progress p in [0,1] (p is the fraction of the segment's TIME).
inline double CamPathPchipAt(const std::vector<double>& t, const std::vector<double>& s,
                             const std::vector<double>& m, int k, double p) {
	const int n = (int)t.size();
	if (n < 2 || k < 0 || k > n - 2) return 0.0;
	if (p < 0.0) p = 0.0; else if (p > 1.0) p = 1.0;
	const double h = t[k + 1] - t[k];
	const double p2 = p * p, p3 = p2 * p;
	const double h00 = 2 * p3 - 3 * p2 + 1, h10 = p3 - 2 * p2 + p, h01 = -2 * p3 + 3 * p2, h11 = p3 - p2;
	double v = h00 * s[k] + h10 * h * m[k] + h01 * s[k + 1] + h11 * h * m[k + 1];
	// Guard rounding: never leave the segment.
	const double lo = s[k] < s[k + 1] ? s[k] : s[k + 1], hi = s[k] < s[k + 1] ? s[k + 1] : s[k];
	return v < lo ? lo : v > hi ? hi : v;
}

} // namespace Filmmaker
