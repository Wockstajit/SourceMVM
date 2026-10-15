#include "FollowCameraMath.h"

#include <algorithm>
#include <cmath>

namespace Filmmaker {

double FollowWrapDegrees(double value) {
	while (value > 180.0) value -= 360.0;
	while (value < -180.0) value += 360.0;
	return value;
}

double FollowHalfTimeAlpha(double halfTimeSeconds, double deltaSeconds) {
	if (deltaSeconds <= 0.0)
		return 0.0;
	if (halfTimeSeconds <= 0.0001)
		return 1.0;
	return 1.0 - std::pow(0.5, deltaSeconds / halfTimeSeconds);
}

FollowAngles FollowLookAt(const FollowVec3& camera, const FollowVec3& target) {
	const double dx = target.x - camera.x;
	const double dy = target.y - camera.y;
	const double dz = target.z - camera.z;
	const double horizontal = std::sqrt(dx * dx + dy * dy);
	FollowAngles out;
	out.yaw = std::atan2(dy, dx) * 180.0 / 3.14159265358979323846;
	out.pitch = std::atan2(-dz, horizontal) * 180.0 / 3.14159265358979323846;
	return out;
}

FollowAngles FollowAddAngles(const FollowAngles& base, const FollowAngles& offset) {
	return FollowAngles{
		FollowWrapDegrees(base.pitch + offset.pitch),
		FollowWrapDegrees(base.yaw + offset.yaw),
		FollowWrapDegrees(base.roll + offset.roll)
	};
}

namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr double kRad2Deg = 180.0 / kPi;
constexpr double kDeg2Rad = kPi / 180.0;
}

FollowAngles FollowQuatToAngles(double x, double y, double z, double w) {
	// Source-engine QuaternionAngles (matrix rows m1*, m3*); pitch=X, yaw=Y, roll=Z.
	const double m11 = (2.0 * w * w) + (2.0 * x * x) - 1.0;
	const double m12 = (2.0 * x * y) + (2.0 * w * z);
	double m13 = (2.0 * x * z) - (2.0 * w * y);
	const double m23 = (2.0 * y * z) + (2.0 * w * x);
	const double m33 = (2.0 * w * w) + (2.0 * z * z) - 1.0;
	if (m13 > 1.0) m13 = 1.0; else if (m13 < -1.0) m13 = -1.0; // clamp for asin
	FollowAngles out;
	out.yaw = std::atan2(m12, m11) * kRad2Deg;
	out.pitch = std::asin(-m13) * kRad2Deg;
	out.roll = std::atan2(m23, m33) * kRad2Deg;
	return out;
}

FollowVec3 FollowRotateVector(const FollowVec3& localOffset, const FollowAngles& angles) {
	// Source AngleVectors basis (forward/right/up) from pitch/yaw/roll.
	const double sp = std::sin(angles.pitch * kDeg2Rad), cp = std::cos(angles.pitch * kDeg2Rad);
	const double sy = std::sin(angles.yaw * kDeg2Rad), cy = std::cos(angles.yaw * kDeg2Rad);
	const double sr = std::sin(angles.roll * kDeg2Rad), cr = std::cos(angles.roll * kDeg2Rad);
	const FollowVec3 forward{ cp * cy, cp * sy, -sp };
	const FollowVec3 right{ -sr * sp * cy + cr * sy, -sr * sp * sy - cr * cy, -sr * cp };
	const FollowVec3 up{ cr * sp * cy + sr * sy, cr * sp * sy - sr * cy, cr * cp };
	return FollowVec3{
		forward.x * localOffset.x + right.x * localOffset.y + up.x * localOffset.z,
		forward.y * localOffset.x + right.y * localOffset.y + up.y * localOffset.z,
		forward.z * localOffset.x + right.z * localOffset.y + up.z * localOffset.z
	};
}

FollowVec3 FollowInverseRotateVector(const FollowVec3& worldOffset, const FollowAngles& angles) {
	// Inverse of FollowRotateVector. The forward/right/up basis is orthonormal, so the inverse
	// transform is the dot product of the world offset against each local axis.
	const double sp = std::sin(angles.pitch * kDeg2Rad), cp = std::cos(angles.pitch * kDeg2Rad);
	const double sy = std::sin(angles.yaw * kDeg2Rad), cy = std::cos(angles.yaw * kDeg2Rad);
	const double sr = std::sin(angles.roll * kDeg2Rad), cr = std::cos(angles.roll * kDeg2Rad);
	const FollowVec3 forward{ cp * cy, cp * sy, -sp };
	const FollowVec3 right{ -sr * sp * cy + cr * sy, -sr * sp * sy - cr * cy, -sr * cp };
	const FollowVec3 up{ cr * sp * cy + sr * sy, cr * sp * sy - sr * cy, cr * cp };
	return FollowVec3{
		worldOffset.x * forward.x + worldOffset.y * forward.y + worldOffset.z * forward.z,
		worldOffset.x * right.x + worldOffset.y * right.y + worldOffset.z * right.z,
		worldOffset.x * up.x + worldOffset.y * up.y + worldOffset.z * up.z
	};
}

FollowAngles FollowSmoothAngles(
	const FollowAngles& current,
	const FollowAngles& desired,
	double deltaSeconds,
	double halfTimeSeconds,
	double deadzoneDegrees,
	double maxTurnDegreesPerSecond) {
	FollowAngles out = current;
	double pitchDelta = FollowWrapDegrees(desired.pitch - current.pitch);
	double yawDelta = FollowWrapDegrees(desired.yaw - current.yaw);
	const double error = std::sqrt(pitchDelta * pitchDelta + yawDelta * yawDelta);
	if (error <= std::max(0.0, deadzoneDegrees))
		return out;

	const double alpha = FollowHalfTimeAlpha(halfTimeSeconds, deltaSeconds);
	pitchDelta *= alpha;
	yawDelta *= alpha;

	if (maxTurnDegreesPerSecond > 0.0 && deltaSeconds > 0.0) {
		const double maxStep = maxTurnDegreesPerSecond * deltaSeconds;
		const double step = std::sqrt(pitchDelta * pitchDelta + yawDelta * yawDelta);
		if (step > maxStep && step > 0.0) {
			const double scale = maxStep / step;
			pitchDelta *= scale;
			yawDelta *= scale;
		}
	}

	out.pitch = FollowWrapDegrees(current.pitch + pitchDelta);
	out.yaw = FollowWrapDegrees(current.yaw + yawDelta);
	out.roll = desired.roll;
	return out;
}

FollowVec3 FollowSmoothPosition(
	const FollowVec3& current,
	const FollowVec3& desired,
	double deltaSeconds,
	double halfTimeSeconds) {
	const double alpha = FollowHalfTimeAlpha(halfTimeSeconds, deltaSeconds);
	return FollowVec3{
		current.x + (desired.x - current.x) * alpha,
		current.y + (desired.y - current.y) * alpha,
		current.z + (desired.z - current.z) * alpha
	};
}

double FollowDistance(const FollowVec3& a, const FollowVec3& b) {
	const double dx = b.x - a.x;
	const double dy = b.y - a.y;
	const double dz = b.z - a.z;
	return std::sqrt(dx * dx + dy * dy + dz * dz);
}

} // namespace Filmmaker
