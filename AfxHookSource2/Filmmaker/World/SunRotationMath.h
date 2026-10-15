#pragma once

#include <cmath>

namespace Filmmaker {
namespace SunRotationMath {

struct Vec3 {
	float x, y, z;
};

// Small, dependency-free quaternion used by both the scene-node and GPU sky paths.  Keeping the
// representation here avoids the old split where SkyOrient wrote Euler angles while the draw hook
// rebuilt a second Euler matrix with a different axis convention.
struct Quat {
	float x, y, z, w;
};

inline Quat IdentityQuat() { return { 0.0f, 0.0f, 0.0f, 1.0f }; }

inline Quat Normalize(Quat q) {
	const float length = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
	if (!std::isfinite(length) || length < 1.0e-6f) return IdentityQuat();
	const float inv = 1.0f / length;
	return { q.x * inv, q.y * inv, q.z * inv, q.w * inv };
}

// Hamilton product.  qA * qB applies qB first and qA second, which is the useful order for a
// world-space sky offset followed by the map's authored orientation.
inline Quat Multiply(Quat qA, Quat qB) {
	return Normalize({
		qA.w * qB.x + qA.x * qB.w + qA.y * qB.z - qA.z * qB.y,
		qA.w * qB.y - qA.x * qB.z + qA.y * qB.w + qA.z * qB.x,
		qA.w * qB.z + qA.x * qB.y - qA.y * qB.x + qA.z * qB.w,
		qA.w * qB.w - qA.x * qB.x - qA.y * qB.y - qA.z * qB.z,
	});
}

inline Quat AxisAngle(Vec3 axis, float degrees) {
	constexpr double kPi = 3.14159265358979323846;
	const double radians = (double)degrees * kPi / 180.0 * 0.5;
	const float s = (float)std::sin(radians);
	const float c = (float)std::cos(radians);
	return Normalize({ axis.x * s, axis.y * s, axis.z * s, c });
}

// Source 2 is Z-up and QAngle is (pitch about Y, yaw about Z, roll about X).  Compose yaw and
// pitch as quaternions instead of adding Euler components; this stays continuous through the
// poles and has no gimbal-flip discontinuity.
inline Quat FromYawPitch(float yawDegrees, float pitchDegrees) {
	return Multiply(AxisAngle({ 0.0f, 0.0f, 1.0f }, yawDegrees),
		AxisAngle({ 0.0f, 1.0f, 0.0f }, pitchDegrees));
}

inline Vec3 Rotate(Quat q, Vec3 v) {
	q = Normalize(q);
	const Vec3 u{ q.x, q.y, q.z };
	const Vec3 uv{
		u.y * v.z - u.z * v.y,
		u.z * v.x - u.x * v.z,
		u.x * v.y - u.y * v.x,
	};
	const Vec3 uuv{
		u.y * uv.z - u.z * uv.y,
		u.z * uv.x - u.x * uv.z,
		u.x * uv.y - u.y * uv.x,
	};
	return {
		v.x + 2.0f * (q.w * uv.x + uuv.x),
		v.y + 2.0f * (q.w * uv.y + uuv.y),
		v.z + 2.0f * (q.w * uv.z + uuv.z),
	};
}

// Row-major 3x3 rotation matrix.  The GPU path consumes this as a post-multiply correction, so
// the caller can apply its one measured consumer-basis correction without exposing another UI
// slider.
inline void ToMatrix3(Quat q, float out[9]) {
	q = Normalize(q);
	const float xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
	const float xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
	const float wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
	out[0] = 1.0f - 2.0f * (yy + zz); out[1] = 2.0f * (xy - wz);       out[2] = 2.0f * (xz + wy);
	out[3] = 2.0f * (xy + wz);       out[4] = 1.0f - 2.0f * (xx + zz); out[5] = 2.0f * (yz - wx);
	out[6] = 2.0f * (xz - wy);       out[7] = 2.0f * (yz + wx);       out[8] = 1.0f - 2.0f * (xx + yy);
}

// Diagnostic/legacy QAngle projection only.  The quaternion remains the authoritative value;
// this projection is written to engine fields that still expose angles to other systems.
inline void ToSourceAngles(Quat q, float outPitchYawRoll[3]) {
	q = Normalize(q);
	const float sinPitch = 2.0f * (q.w * q.y - q.z * q.x);
	const float clamped = sinPitch < -1.0f ? -1.0f : (sinPitch > 1.0f ? 1.0f : sinPitch);
	const float roll = std::atan2(2.0f * (q.w * q.x + q.y * q.z),
		1.0f - 2.0f * (q.x * q.x + q.y * q.y));
	const float yaw = std::atan2(2.0f * (q.w * q.z + q.x * q.y),
		1.0f - 2.0f * (q.y * q.y + q.z * q.z));
	constexpr float kRadToDeg = 57.29577951308232f;
	outPitchYawRoll[0] = std::asin(clamped) * kRadToDeg;
	outPitchYawRoll[1] = yaw * kRadToDeg;
	outPitchYawRoll[2] = roll * kRadToDeg;
}

inline float WrapYaw(float degrees) {
	if (!std::isfinite(degrees)) return 0.0f;
	const float wrapped = std::fmod(degrees, 360.0f);
	if (wrapped < 0.0f) return wrapped + 360.0f;
	// Keep the UI's right-hand endpoint visible. 360° is directionally identical to 0° but
	// represents a completed full sweep, so letting it survive the normalization makes the
	// control and state JSON tell the truth about that endpoint.
	if (wrapped == 0.0f && degrees > 0.0f) return 360.0f;
	return wrapped;
}

inline float ClampPitch(float degrees) {
	if (!std::isfinite(degrees)) return 0.0f;
	return degrees < -90.0f ? -90.0f : (degrees > 90.0f ? 90.0f : degrees);
}

// The sun's two orientation controls, relative to the map's own sun (`reference`, the authored sun
// direction before our rotation; Source 2 is Z-up):
//  - yaw   = azimuth: the sun circles the horizon at the SAME height, about world up;
//  - pitch = elevation: the sun rises (+) or sinks (-) straight toward/away from the zenith, about
//    the horizontal axis perpendicular to the sun's own azimuth.
// Turning pitch about a FIXED world axis (the old implementation) swung a sun that happened to sit
// along that axis sideways across the sky, so both sliders read as "the same rotation". The result
// is one rigid rotation, so the sun disc, the sky sprite's vertices and the light/shadow direction
// (which points the opposite way) all turn together. The elevation is held inside +-89 degrees, so
// the sun never flips over the zenith. A degenerate reference falls back to azimuth 0, elevation 0.
struct SunAngles { float azimuth, elevation; };
inline SunAngles ReferenceAngles(Vec3 reference) {
	constexpr double kRadToDeg = 57.29577951308232;
	const double length = std::sqrt((double)reference.x * reference.x + (double)reference.y * reference.y
		+ (double)reference.z * reference.z);
	if (!std::isfinite(length) || length < 1.0e-6) return { 0.0f, 0.0f };
	const double horizontal = std::sqrt((double)reference.x * reference.x + (double)reference.y * reference.y);
	const double azimuth = horizontal > 1.0e-6 ? std::atan2((double)reference.y, (double)reference.x) : 0.0;
	return { (float)(azimuth * kRadToDeg), (float)(std::asin(std::fmax(-1.0, std::fmin(1.0, reference.z / length))) * kRadToDeg) };
}
inline constexpr float kMaxSunElevation = 89.0f;
// The elevation change actually applied: the requested pitch, held so the sun stays within +-89.
inline float EffectivePitch(float pitchDegrees, Vec3 reference) {
	const float elevation = ReferenceAngles(reference).elevation;
	float target = elevation + pitchDegrees;
	target = target < -kMaxSunElevation ? -kMaxSunElevation : (target > kMaxSunElevation ? kMaxSunElevation : target);
	return target - elevation;
}
inline Quat SunRotation(float yawDegrees, float pitchDegrees, Vec3 reference) {
	constexpr double kDegToRad = 3.14159265358979323846 / 180.0;
	const double azimuth = (double)ReferenceAngles(reference).azimuth * kDegToRad;
	// Rotating a direction at `azimuth` about h = (-sin, cos, 0) by +a LOWERS it; raise = -pitch.
	const Quat raise = AxisAngle({ (float)-std::sin(azimuth), (float)std::cos(azimuth), 0.0f },
		-EffectivePitch(pitchDegrees, reference));
	return Multiply(AxisAngle({ 0.0f, 0.0f, 1.0f }, yawDegrees), raise); // elevation first, then circle
}
inline Vec3 Rotate(Vec3 v, float yawDegrees, float pitchDegrees, Vec3 reference) {
	return Rotate(SunRotation(yawDegrees, pitchDegrees, reference), v);
}

// Rigidly rotates a sky vertex around the camera. Rotating only the billboard centre while leaving
// its corner basis behind makes the plane edge-on/back-facing after a large yaw or pitch change;
// rotating every vertex preserves the authored camera-facing relationship for the whole sweep.
inline Vec3 RotateAroundPoint(Vec3 point, Vec3 pivot, float yawDegrees, float pitchDegrees, Vec3 reference) {
	const Vec3 relative{ point.x - pivot.x, point.y - pivot.y, point.z - pivot.z };
	const Vec3 rotated = Rotate(relative, yawDegrees, pitchDegrees, reference);
	return { pivot.x + rotated.x, pivot.y + rotated.y, pivot.z + rotated.z };
}

inline bool BoundsContain(Vec3 minimum, Vec3 maximum, Vec3 point, float tolerance = 0.0f) {
	return point.x >= minimum.x - tolerance && point.x <= maximum.x + tolerance
		&& point.y >= minimum.y - tolerance && point.y <= maximum.y + tolerance
		&& point.z >= minimum.z - tolerance && point.z <= maximum.z + tolerance;
}

inline Vec3 AnglesToDirection(float yawDegrees, float pitchDegrees) {
	constexpr double kPi = 3.14159265358979323846;
	const double yaw = (double)yawDegrees * kPi / 180.0;
	const double pitch = (double)pitchDegrees * kPi / 180.0;
	const double cp = std::cos(pitch);
	return { (float)(cp * std::cos(yaw)), (float)(cp * std::sin(yaw)), (float)std::sin(pitch) };
}

inline float Length(Vec3 v) {
	return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
}

} // namespace SunRotationMath
} // namespace Filmmaker
