#pragma once

#include <cfloat>
#include <cmath>

namespace Filmmaker {

struct FollowRenderClock {
	int frame = -1;
	double time = 0.0;
	bool initialized = false;
};
struct FollowRenderDelta {
	bool newFrame = false;
	double smoothing = 0.0;
	double playback = 0.0;
};

// Integrate once per rendered frame, using interpolated playback time, never a mixture of
// wall frames and full demo ticks. Paused editing may settle the filter on wall time, but
// cannot generate target velocity. Repeated view passes must reuse the same camera solve.
inline FollowRenderDelta FollowAdvanceRenderClock(FollowRenderClock& clock,
	int frame, double time, double wallDt, bool paused) {
	if (clock.initialized && clock.frame == frame) return {};
	const bool hadTime = clock.initialized;
	const double elapsed = hadTime ? time - clock.time : 0.0;
	clock.frame = frame;
	clock.initialized = std::isfinite(time);
	if (clock.initialized) {
		// A small pause-boundary rewind is not extra playback when time catches up.
		if (!hadTime || paused || elapsed > 0.0 || elapsed < -0.1) clock.time = time;
	}
	const double playback = !paused && hadTime && std::isfinite(elapsed) && elapsed > 0.0 && elapsed <= 0.1
		? elapsed : 0.0;
	const double wall = std::isfinite(wallDt) && wallDt > 0.0 ? (wallDt < 0.1 ? wallDt : 0.1) : 0.0;
	return {true, paused || !hadTime ? wall : playback, playback};
}

// A demo can render many frames per tick. Count each forward tick once; never charge paused
// frames or seek discontinuities to a death hold. The caller resets its window on a seek.
inline bool FollowTickDiscontinuity(int previous, int current) {
	// CS2 can report the preceding tick when pausing at an interpolation boundary.
	return previous >= 0 && (current < previous - 1 || current - previous >= 128);
}
inline double FollowPlaybackDelta(int previous, int current, double interval) {
	if (previous < 0 || current <= previous || FollowTickDiscontinuity(previous, current)) return 0.0;
	if (!std::isfinite(interval) || interval <= 0.00001) interval = 1.0 / 64.0;
	return (current - previous) * interval;
}

struct FollowVec3 {
	double x = 0.0;
	double y = 0.0;
	double z = 0.0;
};

struct FollowAngles {
	double pitch = 0.0;
	double yaw = 0.0;
	double roll = 0.0;
};

// A usable world position: finite, and not the FLT_MAX an engine read leaves when it fails.
inline bool IsFiniteOrigin(const FollowVec3& p) {
	return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z)
		&& std::fabs(p.x) < FLT_MAX && std::fabs(p.y) < FLT_MAX && std::fabs(p.z) < FLT_MAX;
}

double FollowWrapDegrees(double value);
double FollowHalfTimeAlpha(double halfTimeSeconds, double deltaSeconds);
FollowAngles FollowLookAt(const FollowVec3& camera, const FollowVec3& target);
FollowAngles FollowAddAngles(const FollowAngles& base, const FollowAngles& offset);

// Convert an attachment/bone quaternion (x, y, z, w) to engine view angles
// (pitch, yaw, roll in degrees). Used by rigid attach mode so the camera adopts
// the attachment's orientation, not just its position.
FollowAngles FollowQuatToAngles(double x, double y, double z, double w);

// Rotate a LOCAL-space offset (x = forward, y = right, z = up) by the given
// angles into world space. Lets an attach offset stay "behind the muzzle" /
// "above the head" as the target turns, instead of being a fixed world vector.
FollowVec3 FollowRotateVector(const FollowVec3& localOffset, const FollowAngles& angles);
FollowVec3 FollowInverseRotateVector(const FollowVec3& worldOffset, const FollowAngles& angles);

inline FollowAngles FollowAnglesFromBasis(const FollowVec3& forward, const FollowVec3& up) {
	constexpr double radToDeg = 57.29577951308232;
	const double pitch = std::atan2(-forward.z, std::hypot(forward.x, forward.y)) * radToDeg;
	const double yaw = std::atan2(forward.y, forward.x) * radToDeg;
	const FollowAngles level{pitch, yaw, 0.0};
	const FollowVec3 localUp = FollowInverseRotateVector(up, level);
	return {pitch, yaw, std::atan2(localUp.y, localUp.z) * radToDeg};
}

// Rotation composition uses a right-handed XYZ frame (Y = left). Position sliders use
// Source's forward/right/up basis, whose Y has the opposite sign. Flip Y at that boundary;
// adding/subtracting Euler components only works when the mount rotates around one axis.
inline FollowAngles FollowComposeRotation(const FollowAngles& parent, const FollowAngles& local) {
	const auto forward = FollowRotateVector({1,0,0}, local);
	const auto up = FollowRotateVector({0,0,1}, local);
	return FollowAnglesFromBasis(
		FollowRotateVector({forward.x, -forward.y, forward.z}, parent),
		FollowRotateVector({up.x, -up.y, up.z}, parent));
}

inline FollowAngles FollowRelativeRotation(const FollowAngles& world, const FollowAngles& parent) {
	const auto forward = FollowInverseRotateVector(FollowRotateVector({1,0,0}, world), parent);
	const auto up = FollowInverseRotateVector(FollowRotateVector({0,0,1}, world), parent);
	return FollowAnglesFromBasis({forward.x, -forward.y, forward.z}, {up.x, -up.y, up.z});
}
FollowAngles FollowSmoothAngles(
	const FollowAngles& current,
	const FollowAngles& desired,
	double deltaSeconds,
	double halfTimeSeconds,
	double deadzoneDegrees,
	double maxTurnDegreesPerSecond);
FollowVec3 FollowSmoothPosition(
	const FollowVec3& current,
	const FollowVec3& desired,
	double deltaSeconds,
	double halfTimeSeconds);
double FollowDistance(const FollowVec3& a, const FollowVec3& b);

} // namespace Filmmaker
