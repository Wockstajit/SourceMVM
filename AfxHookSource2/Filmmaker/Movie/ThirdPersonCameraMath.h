#pragma once

#include <cctype>
#include <cmath>

namespace Filmmaker {

inline double ThirdPersonWrapYaw(double yaw) {
	while (yaw > 180.0) yaw -= 360.0;
	while (yaw <= -180.0) yaw += 360.0;
	return yaw;
}

inline double ThirdPersonClampPitch(double pitch) {
	if (pitch < -85.0) return -85.0;
	if (pitch > 85.0) return 85.0;
	return pitch;
}

inline double ThirdPersonClampDistance(double distance) {
	if (distance < 30.0) return 30.0;
	if (distance > 200.0) return 200.0;
	return distance;
}

// Preset orbit offsets are RELATIVE to the player's facing: 0 = behind the player
// (camera looks the same way the player does), 180 = in front looking back at them.
inline bool ThirdPersonPresetYaw(const char* preset, double& outYaw) {
	if (!preset)
		return false;
	auto eq = [](const char* a, const char* b) {
		while (*a && *b) {
			if (std::tolower((unsigned char)*a) != std::tolower((unsigned char)*b))
				return false;
			++a; ++b;
		}
		return *a == 0 && *b == 0;
	};
	if (eq(preset, "front")) { outYaw = 180.0; return true; }
	if (eq(preset, "back")) { outYaw = 0.0; return true; }
	if (eq(preset, "left")) { outYaw = -90.0; return true; }
	if (eq(preset, "right")) { outYaw = 90.0; return true; }
	return false;
}

// Orbit camera pose: the camera sits `distance` units away from the pivot (the player's
// eye) along the direction opposite to (camYaw, camPitch), looking back along that
// direction -- so the pivot is always dead-centre in frame. camYaw is a WORLD yaw (the
// caller composes player yaw + orbit offset), pitch positive = camera above, looking down.
struct ThirdPersonPose {
	double x = 0.0, y = 0.0, z = 0.0;
	double pitch = 0.0;
	double yaw = 0.0;
};

inline ThirdPersonPose ThirdPersonSolvePose(
	const double eye[3], double camYaw, double camPitch, double distance) {
	constexpr double kDegToRad = 3.14159265358979323846 / 180.0;
	ThirdPersonPose out;
	out.yaw = ThirdPersonWrapYaw(camYaw);
	out.pitch = ThirdPersonClampPitch(camPitch);
	// Source forward vector from (pitch, yaw): x = cp*cy, y = cp*sy, z = -sp.
	const double cp = std::cos(out.pitch * kDegToRad);
	const double sp = std::sin(out.pitch * kDegToRad);
	const double cy = std::cos(out.yaw * kDegToRad);
	const double sy = std::sin(out.yaw * kDegToRad);
	out.x = eye[0] - cp * cy * distance;
	out.y = eye[1] - cp * sy * distance;
	out.z = eye[2] + sp * distance;
	return out;
}

// Shortest-arc exponential yaw approach on a wall-clock dt. tau is the smoothing time
// constant in seconds (0 = snap). Used to ease the orbit as the tracked player flicks
// their view, so the camera swings instead of teleporting.
inline double ThirdPersonApproachYaw(double current, double target, double dt, double tau) {
	const double diff = ThirdPersonWrapYaw(target - current);
	if (tau <= 0.0 || dt <= 0.0)
		return ThirdPersonWrapYaw(target);
	const double alpha = 1.0 - std::exp(-dt / tau);
	return ThirdPersonWrapYaw(current + diff * alpha);
}

// Who is the engine's spectator camera watching? In GOTV demos the observer services read mode 0 /
// target -1, so the only signal is the ENGINE's own camera (before any override): in first person it
// sits in the spectated player's eye; in the chase mode third person puts the engine in, it aims at
// that player from behind. Returns a score for one player's eye -- lower is a better match, negative
// is no match -- so a caller picks the lowest over all players:
//   * eye within kEyeMatch of the camera: 0 (in-eye, exact);
//   * otherwise ahead of the camera by 16..400 units and within kChaseRay of its aim ray: the
//     distance from the ray, plus 1 so an in-eye match always wins.
constexpr double kThirdPersonEyeMatchUnits = 1.0;
constexpr double kThirdPersonChaseRayUnits = 48.0;

inline double ThirdPersonWatchScore(const double cam[3], double camPitchDeg, double camYawDeg, const double eye[3]) {
	const double dx = eye[0] - cam[0], dy = eye[1] - cam[1], dz = eye[2] - cam[2];
	const double dist = std::sqrt(dx * dx + dy * dy + dz * dz);
	if (dist <= kThirdPersonEyeMatchUnits) return 0.0;
	const double kPi = 3.14159265358979323846;
	const double p = camPitchDeg * kPi / 180.0, y = camYawDeg * kPi / 180.0;
	// Source angles: pitch positive looks DOWN.
	const double fx = std::cos(p) * std::cos(y), fy = std::cos(p) * std::sin(y), fz = -std::sin(p);
	const double along = dx * fx + dy * fy + dz * fz;
	if (along < 16.0 || along > 400.0) return -1.0;
	const double px = dx - along * fx, py = dy - along * fy, pz = dz - along * fz;
	const double off = std::sqrt(px * px + py * py + pz * pz);
	if (off > kThirdPersonChaseRayUnits) return -1.0;
	return 1.0 + off;
}

} // namespace Filmmaker
