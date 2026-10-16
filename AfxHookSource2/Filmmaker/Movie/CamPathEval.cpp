#include "CamPathEval.h"
#include "CamPathTimingMath.h"

#include "../../../Shared/CamPath.h"   // CamPath / CamPathValue / QEulerAngles (+ g_CamPath)
#include "../../../Shared/AfxConsole.h"

#include <cmath>

// Owned by main.cpp; shared with the mirv_campath console command and the
// in-world keyframe drawer. While the marker system is active it drives this.
extern CamPath g_CamPath;

namespace Filmmaker {

namespace {
	constexpr double kEps = 1e-3;          // min synthetic step (keeps the map strictly increasing)
	constexpr double kConstSpeed = 300.0;  // units/sec at 1.0x for Constant mode
	constexpr double kSegBase = 1.0;       // seconds per segment at 1.0x for Per-Segment

	// Settings ints mirror the CameraPath enums (see PathSettings).
	constexpr int kSpeedConstant = 1;
	constexpr int kSpeedPerSegment = 2;
	constexpr int kInterpSmooth = 1;

	// Ease a normalized progress p in [0,1]. Monotonic in [0,1], so it warps only
	// the TIMING along the segment, never the geometry.
	double ApplyEase(Ease e, double p) {
		if (p < 0.0) p = 0.0; else if (p > 1.0) p = 1.0;
		switch (e) {
		case Ease::In:    return p * p;                       // slow-in
		case Ease::Out:   return 1.0 - (1.0 - p) * (1.0 - p); // slow-out
		case Ease::InOut: return p * p * (3.0 - 2.0 * p);     // smoothstep
		default:          return p;
		}
	}
}

void CamPathEval::Rebuild(const CamMarkers& mk, const PathSettings& st) {
	g_CamPath.Enabled_set(false);
	g_CamPath.Clear();

	const int n = mk.Count();
	const bool wantCubic = (st.interp == kInterpSmooth);
	const bool cubicOk = wantCubic && n >= 3;
	m_smoothProgress = wantCubic && n == 2;
	m_smoothPace = cubicOk;
	m_paceSlopes.clear();
	const CamPath::DoubleInterp di = cubicOk ? CamPath::DI_CUBIC : CamPath::DI_LINEAR;
	const CamPath::QuaternionInterp qi = cubicOk ? CamPath::QI_SCUBIC : CamPath::QI_SLINEAR;
	g_CamPath.PositionInterpMethod_set(di);
	g_CamPath.RotationInterpMethod_set(qi);
	g_CamPath.FovInterpMethod_set(di);

	m_duration = 0.0;
	m_keyT.assign(n, 0.0);
	m_shapeS.assign(n, 0.0);
	if (n == 0) return;

	// TWO SEPARATE axes (the key to keeping the curve shape independent of speed):
	//  * m_shapeS[i] = cumulative CHORD LENGTH. g_CamPath is keyed by THIS, so the
	//    spline tangents depend only on marker POSITIONS, never on speed.
	//  * m_keyT[i]   = cumulative TIMING from the speed mode (Freeze pacing only;
	//    in Live mode the demo's marker times are authoritative).
	double t = 0.0; // timing axis
	double s = 0.0; // shape axis
	for (int i = 0; i < n; ++i) {
		if (i > 0) {
			const CamMarker& a = mk.At(i - 1); // source marker owns this segment
			const CamMarker& b = mk.At(i);

			double dx = b.x - a.x, dy = b.y - a.y, dz = b.z - a.z;
			double chord = std::sqrt(dx * dx + dy * dy + dz * dz);
			if (chord < kEps) chord = kEps; // coincident markers -> tiny step, keep ordered
			s += chord;

			double dt = kEps;
			switch (st.speedMode) {
			case kSpeedConstant: {
				double mul = (st.constSpeed > 0.05f) ? st.constSpeed : 0.05;
				dt = chord / (kConstSpeed * mul);
				if (dt < kEps) dt = kEps;
				break;
			}
			case kSpeedPerSegment: {
				float mul = (a.speedMul > 0.05f) ? a.speedMul : 0.05f;
				dt = kSegBase / mul; // 0.2x -> 5x longer (slower)
				break;
			}
			default: { // Manual
				dt = std::fabs(b.time - a.time);
				if (dt < kEps) dt = kEps;
				break;
			}
			}
			t += dt;
		}
		m_keyT[i] = t;
		m_shapeS[i] = s;
		const CamMarker& m = mk.At(i);
		CamPathValue v(m.x, m.y, m.z, m.pitch, m.yaw, m.roll, m.fov);
		v.Selected = false; // Clear() only fully empties when nothing is Selected
		g_CamPath.Add(s, v); // key by SHAPE so the curve never moves when speed changes
	}
	m_duration = t;
	if (m_smoothPace) m_paceSlopes = CamPathPchipSlopes(m_keyT, m_shapeS);
	// Markers carrying the graph's handles are flown along the graph's curves (EvalSegP).
	m_curves = false;
	for (int i = 0; i < n && !m_curves; ++i) m_curves = mk.At(i).HasCurve();
}

bool CamPathEval::EvalCurves(const CamMarkers& mk, int seg, double p, Pose& out) const {
	// THE GRAPH'S CURVES, EXACTLY (CurvePoseBetween: the per-channel Bezier segments the graph
	// draws, in TICK space). The segment's progress maps linearly onto its ticks, so a speed mode
	// still paces the segment and a marker's ease still warps it, but the SHAPE in time is the
	// graph's.
	const CamMarker& a = mk.At(seg);
	const CamMarker& b = mk.At(seg + 1);
	double v[7];
	CurvePoseBetween(a, b, (double)a.tick + p * (double)(b.tick - a.tick), v);
	out.x = v[0]; out.y = v[1]; out.z = v[2];
	out.pitch = v[3]; out.yaw = v[4]; out.roll = v[5]; out.fov = v[6];
	out.valid = true;
	return true;
}

bool CamPathEval::CurvePointAtShape(const CamMarkers& mk, double s, double outPos[3]) const {
	const int n = mk.Count();
	if (!m_curves || n < 2 || (int)m_shapeS.size() != n)
		return false;
	int seg = 0;
	while (seg < n - 2 && s >= m_shapeS[seg + 1]) ++seg;
	const double s0 = m_shapeS[seg], s1 = m_shapeS[seg + 1];
	double p = (s1 > s0) ? (s - s0) / (s1 - s0) : 1.0;
	if (p < 0.0) p = 0.0; else if (p > 1.0) p = 1.0;
	Pose pose;
	EvalCurves(mk, seg, p, pose);
	outPos[0] = pose.x; outPos[1] = pose.y; outPos[2] = pose.z;
	return true;
}

bool CamPathEval::CanEval() const {
	return g_CamPath.CanEval();
}

CamPathEval::Pose CamPathEval::EvalSegP(const CamMarkers& mk, int seg, double p) const {
	Pose out;
	const int n = mk.Count();
	if (n < 2 || (int)m_shapeS.size() != n)
		return out;
	if (seg < 0) seg = 0; else if (seg > n - 2) seg = n - 2;
	if (p < 0.0) p = 0.0; else if (p > 1.0) p = 1.0;
	out.seg = seg;
	out.p = p;

	// Ease the progress (timing only), then project onto the fixed shape axis.
	double pe = ApplyEase(mk.At(seg).ease, p);
	if (m_curves) {
		EvalCurves(mk, seg, pe, out);
		return out;
	}
	if (m_smoothProgress) pe = pe * pe * (3.0 - 2.0 * pe);
	// Smooth with 3+ keys: the time-to-shape mapping is a monotone cubic through every marker, so
	// the camera's speed flows through the keys; Linear keeps the per-segment linear pacing.
	const double s = (m_smoothPace && (int)m_paceSlopes.size() == n && (int)m_keyT.size() == n)
		? CamPathPchipAt(m_keyT, m_shapeS, m_paceSlopes, seg, pe)
		: m_shapeS[seg] + pe * (m_shapeS[seg + 1] - m_shapeS[seg]);

	if (g_CamPath.CanEval() && g_CamPath.GetLowerBound() <= s && s <= g_CamPath.GetUpperBound()) {
		CamPathValue v = g_CamPath.Eval(s);
		QEulerAngles ang = v.R.ToQREulerAngles().ToQEulerAngles();
		out.x = v.X; out.y = v.Y; out.z = v.Z;
		out.pitch = ang.Pitch; out.yaw = ang.Yaw; out.roll = ang.Roll;
		out.fov = v.Fov;
		out.valid = true;
	}
	return out;
}

CamPathEval::Pose CamPathEval::EvalAtTiming(const CamMarkers& mk, double t) const {
	const int n = mk.Count();
	if (n < 2 || m_duration <= 0.0 || (int)m_keyT.size() != n)
		return Pose{};
	if (t < 0.0) t = 0.0; else if (t > m_duration) t = m_duration;
	int seg = 0;
	while (seg < n - 2 && t >= m_keyT[seg + 1]) ++seg;
	const double la = m_keyT[seg], lb = m_keyT[seg + 1];
	const double p = (lb > la) ? (t - la) / (lb - la) : 1.0;
	return EvalSegP(mk, seg, p);
}

CamPathEval::Pose CamPathEval::EvalAtDemoTime(const CamMarkers& mk, double demoTime) const {
	const int n = mk.Count();
	if (n < 2)
		return Pose{};
	int seg = 0;
	while (seg < n - 2 && demoTime >= mk.At(seg + 1).time) ++seg;
	const double ta = mk.At(seg).time, tb = mk.At(seg + 1).time;
	const double p = (tb > ta) ? (demoTime - ta) / (tb - ta) : 1.0;
	return EvalSegP(mk, seg, p);
}

CamPathEval::Pose CamPathEval::EvalAtTick(const CamMarkers& mk, double tick) const {
	const int n = mk.Count();
	if (n < 2)
		return Pose{};
	int seg = 0;
	while (seg < n - 2 && tick >= (double)mk.At(seg + 1).tick) ++seg;
	const double ta = (double)mk.At(seg).tick, tb = (double)mk.At(seg + 1).tick;
	const double p = (tb > ta) ? (tick - ta) / (tb - ta) : 1.0;
	return EvalSegP(mk, seg, p);
}

double CamPathEval::TimingAtTick(const CamMarkers& mk, double tick) const {
	const int n = mk.Count();
	if (n < 2 || (int)m_keyT.size() != n)
		return 0.0;
	int seg = 0;
	while (seg < n - 2 && tick >= (double)mk.At(seg + 1).tick) ++seg;
	const double ta = (double)mk.At(seg).tick, tb = (double)mk.At(seg + 1).tick;
	double p = (tb > ta) ? (tick - ta) / (tb - ta) : 1.0;
	if (p < 0.0) p = 0.0; else if (p > 1.0) p = 1.0;
	return m_keyT[seg] + p * (m_keyT[seg + 1] - m_keyT[seg]);
}

} // namespace Filmmaker
