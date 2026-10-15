#pragma once

// "GO TO PLAYER" -- one pure pose solver, shared by the Deadlock camera editor's "Go To" action.
//
// The action places the FREE camera a short distance behind and above a chosen hero, looking at
// the hero's eyes, so the user can immediately drop a marker or a lock-on camera from right there.
// Same place in the workflow and the same wording as CS2's Follow inspector "Go To" button -- this
// is that feature's Deadlock counterpart. Everything below is engine-free: the caller reads the
// hero's eye position and facing yaw off the entity system, calls SolveGoToPose, and writes the
// returned position/angles straight into the free camera. No engine types, no state, no .cpp.
//
// CONVENTIONS (measured live against Deadlock, 2026-09-13 -- do not re-derive these from CS2):
//   - Angles are Source convention, in degrees: POSITIVE PITCH LOOKS DOWN (mirv_input angles 45 0 0
//     looks at the floor, -45 at the sky). Yaw 0 faces +X, yaw 90 faces +Y. Z is up.
//   - "Behind" the hero and "the hero's right" are both decided by yaw alone, on the XY plane --
//     a hero's pitch/roll (if any) never rotates where the camera goes.
//   - Source right = yaw - 90, i.e. right = (sin(yaw), -cos(yaw), 0). This is the same forward/
//     right/up basis AfxHookSource2/Filmmaker/Movie/FollowCameraMath.cpp's FollowRotateVector uses
//     at pitch = roll = 0. It is reproduced here rather than linked against, so this header stays a
//     single file with no .cpp companion -- the same shape as DeadlockViewMath.h and
//     DeadlockViewOwnerMath.h next door.

#include <algorithm>
#include <cmath>

namespace Deadlock {

// Sane bounds for the two placement distances. 0 is a legal "stand exactly at eye height / eye
// position" request -- the degenerate case in SolveGoToPose covers it -- and the upper bound is
// generous headroom for any real Deadlock map while still refusing to fling the camera to a
// garbage position a caller-supplied huge value would otherwise produce.
constexpr double kGoToDistanceMin = 0.0;
constexpr double kGoToDistanceMax = 100000.0;

struct GoToVec3 {
	double x = 0.0, y = 0.0, z = 0.0;
};

struct GoToPose {
	bool ok = false;  // false on non-finite input; position and angles are then all left at zero.
	GoToVec3 position;
	double pitch = 0.0, yaw = 0.0, roll = 0.0;  // roll is always 0 -- "Go To" never banks the camera.
};

namespace detail {
constexpr double kPi = 3.14159265358979323846;
constexpr double kDeg2Rad = kPi / 180.0;
constexpr double kRad2Deg = 180.0 / kPi;
}

// Wrap to (-180, 180], via fmod rather than the codebase's usual repeated-subtraction wrap
// (FollowWrapDegrees): a "Go To" click can carry a hero yaw the caller forgot to normalize, and a
// repeated-subtraction wrap over a pathological finite value (a corrupt yaw read, say) would loop
// long enough to look like a hang. fmod is O(1) for any finite input.
inline double GoToWrapDegrees(double value) {
	// fmod keeps the sign of `value`, so this starts in (-360, 360) and one step lands it in
	// (-180, 180]: +180 and -180 both come out as +180.
	double wrapped = std::fmod(value, 360.0);
	if (wrapped > 180.0) wrapped -= 360.0;
	else if (wrapped <= -180.0) wrapped += 360.0;
	return wrapped;
}

// The hero-facing basis on the XY plane. Yaw alone decides "behind" and "right"; pitch/roll play
// no part, matching how the game itself only ever asks for a hero's horizontal facing here.
inline GoToVec3 GoToForward(double yawDegrees) {
	const double yaw = yawDegrees * detail::kDeg2Rad;
	return {std::cos(yaw), std::sin(yaw), 0.0};
}
inline GoToVec3 GoToRight(double yawDegrees) {
	const double yaw = yawDegrees * detail::kDeg2Rad;
	return {std::sin(yaw), -std::cos(yaw), 0.0};
}

// The full Source forward vector for a pitch/yaw pair (roll only rotates the horizon, never the
// look direction). Callers and tests use this to check where a resulting pose actually looks.
inline GoToVec3 GoToForwardFromAngles(double pitchDegrees, double yawDegrees) {
	const double pitch = pitchDegrees * detail::kDeg2Rad;
	const double yaw = yawDegrees * detail::kDeg2Rad;
	const double cosPitch = std::cos(pitch);
	return {cosPitch * std::cos(yaw), cosPitch * std::sin(yaw), -std::sin(pitch)};
}

// The pose solver. "Behind" = opposite the hero's facing yaw on the XY plane; "above" = eye height
// plus upHeight; the optional lateral offset slides the camera along the hero's right vector
// BEFORE the look-at is taken, so a nonzero offset still ends up aimed at the hero, not past them.
inline GoToPose SolveGoToPose(
		double eyeX, double eyeY, double eyeZ,
		double heroYawDegrees,
		double backDistance = 150.0,
		double upHeight = 60.0,
		double lateralOffset = 0.0) {
	GoToPose pose;
	// Guard rail: any non-finite input fails closed. A caller must check `ok` before applying this
	// -- there is no "best effort" position for a NaN eye or a NaN yaw.
	if (!std::isfinite(eyeX) || !std::isfinite(eyeY) || !std::isfinite(eyeZ) ||
		!std::isfinite(heroYawDegrees) || !std::isfinite(backDistance) ||
		!std::isfinite(upHeight) || !std::isfinite(lateralOffset))
		return pose;

	// Clamp the two distances into the documented sane range; lateral keeps its sign (left vs.
	// right) but is bounded the same way so an extreme caller value can't do the same off-map trick
	// sideways instead of backward.
	const double back = std::clamp(backDistance, kGoToDistanceMin, kGoToDistanceMax);
	const double up = std::clamp(upHeight, kGoToDistanceMin, kGoToDistanceMax);
	const double lateral = std::clamp(lateralOffset, -kGoToDistanceMax, kGoToDistanceMax);

	const GoToVec3 forward = GoToForward(heroYawDegrees);
	const GoToVec3 right = GoToRight(heroYawDegrees);

	pose.position.x = eyeX - back * forward.x + lateral * right.x;
	pose.position.y = eyeY - back * forward.y + lateral * right.y;
	pose.position.z = eyeZ + up;

	const double dx = eyeX - pose.position.x;
	const double dy = eyeY - pose.position.y;
	const double dz = eyeZ - pose.position.z;
	const double horizontal = std::sqrt(dx * dx + dy * dy);
	const double distance = std::sqrt(dx * dx + dy * dy + dz * dz);

	if (distance < 0.0001) {
		// back == up == lateral == 0 (after clamping): the camera sits exactly on the eye, so
		// "look at the eye" is undefined -- every direction is zero distance away, and atan2(0, 0)
		// would silently hand back world yaw 0 regardless of which way the hero is even facing.
		// Fall back to looking level, along the hero's own facing, instead.
		pose.pitch = 0.0;
		pose.yaw = GoToWrapDegrees(heroYawDegrees);
		pose.roll = 0.0;
		pose.ok = true;
		return pose;
	}

	pose.yaw = std::atan2(dy, dx) * detail::kRad2Deg;
	pose.pitch = std::atan2(-dz, horizontal) * detail::kRad2Deg;
	pose.roll = 0.0;
	pose.ok = true;
	return pose;
}

// How far a candidate pose's POSITION sits from where the same inputs (clamped the same way) would
// place the camera. Two uses: a test proving the solver is deterministic, and a live check proving
// a pose read back from the engine after the fact still matches what was actually requested rather
// than having been nudged by something else that also writes the free camera. A non-ok pose on
// either side has no meaningful distance, so this returns a large sentinel rather than folding a
// second failure mode into the same NaN a bad geometric input would produce.
inline double GoToPositionError(
		const GoToPose& pose,
		double eyeX, double eyeY, double eyeZ,
		double heroYawDegrees,
		double backDistance = 150.0,
		double upHeight = 60.0,
		double lateralOffset = 0.0) {
	if (!pose.ok) return 1e18;
	const GoToPose expected =
		SolveGoToPose(eyeX, eyeY, eyeZ, heroYawDegrees, backDistance, upHeight, lateralOffset);
	if (!expected.ok) return 1e18;
	const double dx = pose.position.x - expected.position.x;
	const double dy = pose.position.y - expected.position.y;
	const double dz = pose.position.z - expected.position.z;
	return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// Angle, in degrees, between a pose's look direction and the direction from its own position to
// `target`. 0 means the pose looks exactly at the target. Same two uses as GoToPositionError above,
// aimed at the ANGLE half of "go to and look at" rather than the position half.
inline double GoToLookErrorDegrees(const GoToPose& pose, const GoToVec3& target) {
	if (!pose.ok) return 180.0;
	const double dx = target.x - pose.position.x;
	const double dy = target.y - pose.position.y;
	const double dz = target.z - pose.position.z;
	const double distance = std::sqrt(dx * dx + dy * dy + dz * dz);
	if (distance < 0.0001) return 0.0;  // sitting on the target: every look direction is "correct".
	const GoToVec3 want{dx / distance, dy / distance, dz / distance};
	const GoToVec3 have = GoToForwardFromAngles(pose.pitch, pose.yaw);
	double dot = want.x * have.x + want.y * have.y + want.z * have.z;
	dot = std::clamp(dot, -1.0, 1.0);
	return std::acos(dot) * detail::kRad2Deg;
}

} // namespace Deadlock
