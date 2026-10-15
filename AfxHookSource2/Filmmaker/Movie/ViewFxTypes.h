#pragma once

// Shared value types for the ViewFx effect stack.
//
// Deliberately a tiny standalone header, exactly like CameraShakeTypes.h: each effect module
// depends only on this contract, never on ViewFx.h and never on each other. The stack owns the
// pose tracker (planar velocity + aim deltas) and the summation; the modules own nothing but
// their own knobs and their own maths.
//
// Angles are DEGREES. Viewmodel offsets are in the viewmodel-local axes mirv_viewmodel already
// uses (x = right, y = forward, z = up).

namespace Filmmaker {

// Every intensity knob in this subsystem is one continuous 0-150% slider (0 = off, 100 = the
// reference tuning), matching the shake stack's convention so one Config widget drives all.
constexpr double kViewFxMaxIntensity = 150.0;

// The frame's tracked motion/aim state, computed once by the ViewFx stack and handed to every
// module. Only produced on an ACCEPTED tick step -- never on a same-tick hold, a rejected
// teleport, or a fresh sample after a seek, so no module can integrate a discontinuity.
struct ViewFxInput {
	double dt = 0.0;           // seconds since the previous accepted sample (> 0)
	double x = 0.0, y = 0.0;   // planar view origin this frame
	double pitchDeg = 0.0, yawDeg = 0.0; // view angles this frame
	double dPitch = 0.0, dYaw = 0.0;     // wrapped aim delta since the last accepted sample
	double velX = 0.0, velY = 0.0;       // smoothed world-space planar velocity, units/sec
	double speed = 0.0;                  // |velX, velY|
};

// Pose deltas for THIS frame, to ADD to the engine's unmodified view. All zero when every
// effect is off (or the tracker has no data yet), so callers can cheaply skip the write.
struct ViewFxPoseDeltas {
	double pitch = 0.0;   // deadzone camera lag, degrees
	double yaw = 0.0;     // deadzone camera lag, degrees
	bool Any() const { return pitch != 0.0 || yaw != 0.0; }
};

// A viewmodel-space offset contribution, in mirv_viewmodel's own axes.
struct ViewFxVmOffset {
	double x = 0.0; // right
	double y = 0.0; // forward
	double z = 0.0; // up
	void Add(const ViewFxVmOffset& o) { x += o.x; y += o.y; z += o.z; }
};

// Clamp helper shared by the percent knobs. Also catches NaN.
inline double ViewFxClampIntensity(double percent) {
	if (!(percent > 0.0)) return 0.0;
	return percent > kViewFxMaxIntensity ? kViewFxMaxIntensity : percent;
}

// Real-unit clamp for knobs expressed in degrees / rate constants. Also catches NaN.
inline double ViewFxClamp(double v, double lo, double hi) {
	if (!(v > lo)) return lo;
	return v > hi ? hi : v;
}

// A one-sample aim delta larger than this is a spectator cut or seek, not a turn. Used by
// weapon float, which integrates aim deltas.
constexpr double kViewFxCutDeltaDeg = 45.0;

} // namespace Filmmaker
