#pragma once

// CamMarkers: the authoritative, ordered list of camera markers plus the
// path-wide settings that are persisted alongside them. Plain data (no engine
// types) so this header stays light and (de)serialization is trivial.
//
// Owned by the CameraPath facade; READ by CamPathEval (to rebuild g_CamPath) and
// CamPlayback (to evaluate). This is the "marker data" slice carved out of the
// old CameraPath god-object.

#include "GraphCurveMath.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace Filmmaker {

// Per-keyframe easing, applied to the OUTGOING segment's progress only. It warps
// TIMING within the segment (slow-in / slow-out), never the curve GEOMETRY, so it
// composes with the shape/timing split. IWXMVM does not expose this; it is one of
// our extras (see plan Part B).
enum class Ease { None = 0, In = 1, Out = 2, InOut = 3 };

// One camera marker. Doubles for position/angles keep the smoothing math clean.
struct CamMarker {
	double x = 0.0, y = 0.0, z = 0.0;          // world position
	double pitch = 0.0, yaw = 0.0, roll = 0.0; // view angles (degrees)
	double fov = 90.0;
	int tick = 0;                              // demo tick at placement
	double time = 0.0;                         // demo time (seconds) at placement
	// Speed multiplier for this marker's OUTGOING segment (Per-Segment mode only);
	// the last marker has no outgoing segment, so its value is ignored.
	float speedMul = 1.0f;
	Ease ease = Ease::None;                    // easing of this marker's outgoing segment
	// The graph's Bezier handles per channel, in GraphExpModel's channel order (x, y, z, pitch,
	// yaw, roll, fov): `in` toward the previous marker, `out` toward the next (GraphCurveMath.h).
	// A path whose markers carry any is flown along those curves (CamPathEval); a path with none
	// keeps the spline it always had.
	GraphCurve::Handle in[7], out[7];
	bool HasCurve() const {
		for (int c = 0; c < 7; ++c) if (in[c].active || out[c].active) return true;
		return false;
	}
};

// THE LIMITS ON A MARKER. A camera path once saved yaw = 4.7e10 degrees with graph handles of
// +-1.6e10 (user report 2026-10-17: "the roll, pitch and stuff can go to really high numbers ... it
// crashes your game, then it doesn't load"). A marker is checked wherever it enters the list -- a
// placement, a typed or dragged value, a graph edit, an undo, a load -- so a value like that is never
// kept. Angles may still spin a path through many turns; past kMarkerMaxAngle a value is folded back
// to the same direction within one turn. Returns true when something had to change.
constexpr double kMarkerMaxAngle = 36000.0;   // 100 turns either way
constexpr double kMarkerMaxCoord = 1.0e6;     // world units (CS2 maps sit within +-16384)
constexpr double kMarkerMaxHandle = 1.0e6;    // a handle's value offset, any channel
inline double SanitizeMarkerAngle(double v) {
	if (!std::isfinite(v)) return 0.0;
	return std::fabs(v) > kMarkerMaxAngle ? std::remainder(v, 360.0) : v;
}
inline bool SanitizeMarker(CamMarker& m) {
	bool changed = false;
	auto set = [&](double& v, double next) { if (!(v == next)) { v = next; changed = true; } };
	auto coord = [&](double& v) {
		set(v, std::isfinite(v) ? (std::min)((std::max)(v, -kMarkerMaxCoord), kMarkerMaxCoord) : 0.0);
	};
	coord(m.x); coord(m.y); coord(m.z);
	set(m.pitch, SanitizeMarkerAngle(m.pitch));
	set(m.yaw, SanitizeMarkerAngle(m.yaw));
	set(m.roll, SanitizeMarkerAngle(m.roll));
	set(m.fov, std::isfinite(m.fov) ? (std::min)((std::max)(m.fov, 1.0), 179.0) : 90.0);
	if (!std::isfinite(m.time)) { m.time = 0.0; changed = true; }
	if (!std::isfinite(m.speedMul) || m.speedMul <= 0.0f) { m.speedMul = 1.0f; changed = true; }
	for (int c = 0; c < 7; ++c) {
		const double limit = GraphCurve::IsAngleChannel(c) ? kMarkerMaxAngle : kMarkerMaxHandle;
		for (GraphCurve::Handle* h : { &m.in[c], &m.out[c] }) {
			set(h->tx, std::isfinite(h->tx) ? (std::min)((std::max)(h->tx, 0.0), 1.0) : 1.0 / 3.0);
			set(h->dv, std::isfinite(h->dv) && std::fabs(h->dv) <= limit ? h->dv : 0.0);
		}
	}
	return changed;
}

// The pose between markers a and b at `tick` along the graph's curves: per channel the Bezier
// segment from a's `out` handle to b's `in` handle (GraphCurve::SegmentValue -- what the graph
// draws), angles the short way round. CamPathEval::EvalCurves flies this. Order: x, y, z, pitch,
// yaw, roll, fov.
inline void CurvePoseBetween(const CamMarker& a, const CamMarker& b, double tick, double out[7]) {
	const double va[7] = { a.x, a.y, a.z, a.pitch, a.yaw, a.roll, a.fov };
	const double vb[7] = { b.x, b.y, b.z, b.pitch, b.yaw, b.roll, b.fov };
	for (int c = 0; c < 7; ++c) {
		const double end = GraphCurve::IsAngleChannel(c) ? GraphCurve::Unwrap(va[c], vb[c]) : vb[c];
		out[c] = GraphCurve::SegmentValue((double)a.tick, va[c], a.out[c], (double)b.tick, end, b.in[c], tick);
	}
}

// Path-wide settings persisted in the SAME sidecar as the markers. Kept here so
// the on-disk schema lives in one place; the ints mirror the CameraPath enums
// (SpeedMode / Timing / Interp) to keep this header free of the facade's types.
struct PathSettings {
	int speedMode = 0;   // CameraPath::SpeedMode (0 Manual, 1 Constant, 2 PerSegment)
	int timing = 0;      // CameraPath::Timing    (0 Live, 1 Freeze)
	int interp = 0;      // CameraPath::Interp    (0 Linear, 1 Smooth/cubic)
	float constSpeed = 1.0f;
	bool autoSnap = false;
};

class CamMarkers {
public:
	int Count() const { return (int)m_markers.size(); }
	bool Empty() const { return m_markers.empty(); }
	const std::vector<CamMarker>& All() const { return m_markers; }

	const CamMarker& At(int i) const { return m_markers[i]; }
	CamMarker& At(int i) { return m_markers[i]; }
	const CamMarker& Front() const { return m_markers.front(); }
	const CamMarker& Back() const { return m_markers.back(); }

	int Selected() const { return m_selected; }
	void SetSelected(int i);                                  // clamps into range
	bool ValidIndex(int i) const { return i >= 0 && i < (int)m_markers.size(); }
	bool SelectedIsLast() const { return !m_markers.empty() && m_selected == (int)m_markers.size() - 1; }

	int Add(const CamMarker& m);                              // append, select it, return index
	bool DeleteIndex(int i);                                  // erase + fix selection; false if OOB
	void DeleteAll();
	void Restore(const std::vector<CamMarker>& markers, int selected);

	// JSON payload: markers + settings + selection in one document. The project store
	// carries it as the "campath" section of the demo's project.mvm.
	//   Serialize: writes the JSON text.
	//   Parse: false if the text is malformed (list left untouched); on a valid document
	//         the list is cleared + repopulated.
	std::string Serialize(const PathSettings& settings, int selected) const;
	bool Parse(const std::string& text, PathSettings& settings, int& selected);

private:
	std::vector<CamMarker> m_markers;
	int m_selected = -1;
};

// A marker's value on one of the graph's channels: x, y, z, pitch, yaw, roll, fov.
inline double CamMarkerChannelValue(const CamMarker& m, int c) {
	switch (c) {
	case 0: return m.x; case 1: return m.y; case 2: return m.z;
	case 3: return m.pitch; case 4: return m.yaw; case 5: return m.roll;
	default: return m.fov;
	}
}

// The path flies the graph's curves: some marker carries handles (CamPathEval::EvalCurves).
inline bool CamMarkersHaveCurves(const CamMarkers& data) {
	for (int i = 0; i < data.Count(); ++i) if (data.At(i).HasCurve()) return true;
	return false;
}

// The graph's Smooth on the markers themselves: Catmull-Rom handles per channel
// (GraphCurve::AutoTangent, the function GraphExpModel uses), on marker `only` or on all (-1).
inline void CamMarkersAutoTangent(CamMarkers& data, int only) {
	const int n = data.Count();
	std::vector<double> ticks(n), values(n);
	for (int i = 0; i < n; ++i) ticks[i] = (double)data.At(i).tick;
	for (int c = 0; c < 7; ++c) {
		for (int i = 0; i < n; ++i) {
			values[i] = CamMarkerChannelValue(data.At(i), c);
			if (i > 0 && GraphCurve::IsAngleChannel(c)) values[i] = GraphCurve::Unwrap(values[i - 1], values[i]);
		}
		for (int i = 0; i < n; ++i)
			if (only < 0 || i == only) GraphCurve::AutoTangent(ticks.data(), values.data(), n, i, data.At(i).in[c], data.At(i).out[c]);
	}
}

} // namespace Filmmaker
