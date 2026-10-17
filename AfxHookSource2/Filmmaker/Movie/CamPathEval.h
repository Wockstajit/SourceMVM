#pragma once

// CamPathEval: the path-evaluation slice carved out of the old CameraPath
// god-object. It owns rebuilding the shared g_CamPath on the speed-INDEPENDENT
// "shape" axis (cumulative chord length) and the tick / timing / shape mapping,
// and exposes a set of PURE evaluators.
//
// "Pure" means: no clock, no smoothing, no side effects on the engine. The same
// inputs always yield the same pose. This is exactly what tick-perfect scrubbing
// (plan Part C) needs, and what playback (CamPlayback) layers its smoothed clock
// + output low-pass on top of.
//
// The curve math itself stays in Shared/CamPath + Shared/AfxMath (natural cubic
// spline for position/FOV, quaternion slerp/qspline for rotation). We match
// IWXMVM's natural-cubic position curve but KEEP our quaternion rotation.

#include "CamMarkers.h"

namespace Filmmaker {

class CamPathEval {
public:
	// A fully-evaluated camera pose at a point on the path. Angles are already
	// converted from the path quaternion to pitch/yaw/roll (degrees).
	struct Pose {
		double x = 0.0, y = 0.0, z = 0.0;
		double pitch = 0.0, yaw = 0.0, roll = 0.0;
		double fov = 0.0;
		bool valid = false; // false => off the evaluable range (no pose this call)
		int seg = 0;        // segment index used (0-based)
		double p = 0.0;     // un-eased normalized progress within the segment [0,1]
	};

	// Push the markers into g_CamPath on the shape axis using the given settings.
	// Selects Linear vs cubic ("Smooth"). Two-key paths use smooth progress
	// over linear geometry; three or more use cubic position/FOV and quaternion rotation.
	void Rebuild(const CamMarkers& markers, const PathSettings& settings);

	bool CanEval() const;                         // g_CamPath has an evaluable range
	double Duration() const { return m_duration; } // timing-axis end (Freeze pacing)
	int SegmentCount() const { return m_keyT.empty() ? 0 : (int)m_keyT.size() - 1; }

	const std::vector<double>& KeyT() const { return m_keyT; }    // timing of each marker
	const std::vector<double>& ShapeS() const { return m_shapeS; } // shape param of each marker

	// --- pure evaluators (no clock, no smoothing) ---
	// Map a TIMING-axis cursor t in [0, Duration()] (Freeze pacing) to a pose.
	Pose EvalAtTiming(const CamMarkers& markers, double t) const;
	// Map a demo TIME (seconds) to a pose using the markers' recorded times (Live).
	Pose EvalAtDemoTime(const CamMarkers& markers, double demoTime) const;
	// Map a demo TICK (may be fractional) to a pose using the markers' recorded
	// ticks. Deterministic: identical tick => identical pose (used for scrubbing).
	Pose EvalAtTick(const CamMarkers& markers, double tick) const;
	// Map a demo tick onto the Freeze timing axis, preserving the same segment/progress.
	double TimingAtTick(const CamMarkers& markers, double tick) const;

	// Map an explicit (segment, normalized progress) to a pose: ease the progress,
	// project onto the shape axis, sample the spline. The shared core of the above.
	// When the markers carry the graph's handles (CamMarker::HasCurve), the pose comes from the
	// graph's per-channel Bezier curves instead (EvalCurves), so the graph is what flies.
	Pose EvalSegP(const CamMarkers& markers, int seg, double p) const;

	// The path follows the graph's curves (any marker carries handles).
	bool FollowsCurves() const { return m_curves; }
	// The position at shape-axis parameter s, from the graph's curves -- for the in-world
	// trajectory line, which samples the shape axis. False when the path has no curves.
	bool CurvePointAtShape(const CamMarkers& markers, double s, double outPos[3]) const;

private:
	bool EvalCurves(const CamMarkers& markers, int seg, double p, Pose& out) const;
	bool m_curves = false;
	std::vector<double> m_keyT;   // cumulative TIMING of each marker (speed-mode durations)
	std::vector<double> m_shapeS; // cumulative SHAPE (chord length); g_CamPath is keyed by this
	double m_duration = 0.0;      // == m_keyT.back()
	bool m_smoothProgress = false; // Smooth mode with 2 keys: eased linear geometry
	// Smooth mode with 3+ keys: time maps to the shape axis through a monotone cubic (CamPathTimingMath.h),
	// so the speed changes smoothly through each marker instead of jumping. These are its slopes.
	bool m_smoothPace = false;
	std::vector<double> m_paceSlopes;
};

} // namespace Filmmaker
