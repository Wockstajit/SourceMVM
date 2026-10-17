#pragma once

// THE FOLLOW-CAMERA MATH CORE, taken from CS2's spec instead of guessed at.
//
// This header replaces the math Deadlock's own DeadlockFollowCamera.cpp::FollowCamera_Solve()
// currently does inline (as of this writing, lines 290-341 of that file): an ORBIT model that
// recomputes the camera's world position every frame from yaw/pitch/dist/height/lateral sliders
// around the (possibly moving) target. That is wrong for lock-on. The requirement, from CS2's own
// Movie/FollowCamera.cpp, is:
//
//   LOCK-ON keeps the placed camera POSITION fixed forever and only turns to aim.
//   ATTACH moves the camera WITH the target (mount origin + a local offset that rotates with it).
//
// WHERE EVERY RULE BELOW COMES FROM (CS2, read in full before writing this file):
//   - AfxHookSource2/Filmmaker/Movie/FollowCamera.cpp
//       lines 124-142  PlaceCamera(): m_state.cameraPosition is written ONCE, from the current free
//                       camera, and never touched again by any per-frame math.
//       lines 828-939  ViewSetupLockOnOverride(): the whole lock-on per-frame solve. Note in
//                       particular lines 904 and 907-910/926: the pose pushed to the view every
//                       frame uses m_state.cameraPosition.x/y/z directly -- the position is READ,
//                       never recomputed, anywhere in this function.
//       lines 865-869  desiredTarget = rawTarget + m_state.offset (WORLD axes) + velocity*prediction.
//                       This is the "aim point", not the camera position.
//       lines 883-890  first-frame snap (no history yet) vs. FollowSmoothPosition on later frames.
//       lines 892      desiredAngles = FollowLookAt(m_state.cameraPosition [FIXED], m_smoothedTarget).
//       lines 893-896  FollowSmoothAngles gate (mathematically a no-op shortcut, see note below).
//       lines 898-903  aimedAngles: pitch/yaw get m_state.rotationOffset ADDED (a trim), and roll is
//                       m_state.cameraAngles.roll (the camera's OWN placed roll, not the aim result)
//                       plus rotationOffset.roll -- i.e. lock-on's rotation offset never goes through
//                       FollowComposeRotation, unlike attach.
//       lines 706-821  RunFrame()'s Attach branch (the whole ride-along solve):
//         line  715      worldOffset = FollowRotateVector(m_state.offset, baseAng)  -- LOCAL offset
//                         (forward/right/up in the mount's own frame) rotated into world space.
//         lines 716-720  desiredPos = target + worldOffset + velocity*prediction.
//         line  721      desiredAng = FollowComposeRotation(baseAng, m_state.rotationOffset) -- here
//                         the rotation offset IS composed (parent/local), unlike lock-on's trim.
//         lines 722-732  first-frame snap, else FollowSmoothPosition/FollowSmoothAngles, then
//                         `m_outputAngles.roll = desiredAng.roll;` -- smoothing only ever touches
//                         pitch/yaw; roll always snaps straight to the desired value.
//       lines 100-107  ApplyAttachTrackingDefaults(): attach mode runs with lookSmoothing =
//                       positionSmoothing = prediction = deadzone = maxTurnSpeed = 0 BY DEFAULT, but
//                       nothing in the math requires that -- the functions below still accept and
//                       apply nonzero smoothing/prediction/deadzone for attach, per the task brief.
//       lines 629-653  the death/hold/release decision (targetDead, m_deathRetargetElapsed,
//                       autoDisableOnDeath, deathHoldSeconds, holdLastKnownPosition) -- see the long
//                       comment on EvaluateDeathHold() below for exactly how this maps (and does NOT
//                       map) onto Deadlock's own releaseOnDeath/holdLast flags.
//   - AfxHookSource2/Filmmaker/Movie/FollowCameraMath.h / .cpp (both read in full):
//       FollowWrapDegrees, FollowLookAt, FollowRotateVector, FollowInverseRotateVector,
//       FollowSmoothAngles, FollowSmoothPosition, FollowDistance, FollowAddAngles and
//       FollowQuatToAngles are DECLARED in the .h (lines 62, 64-65, 70, 75-76, 103-115) but their
//       BODIES live only in FollowCameraMath.cpp (lines 8-145 there). FollowAnglesFromBasis (.h
//       lines 78-85), FollowComposeRotation (.h lines 90-96) and FollowRelativeRotation (.h lines
//       98-102) DO have inline bodies in the header, but those bodies call FollowRotateVector /
//       FollowInverseRotateVector, which are declaration-only in that same header -- so even these
//       three are not actually callable from a translation unit that only includes the .h. Hence
//       this file cannot `#include` FollowCameraMath.h and link against nothing else; every formula
//       below is reproduced from the .cpp bodies (or, for AnglesFromBasis/ComposeRotation, copied
//       verbatim from the .h's own inline text) as static, header-only functions, exactly the
//       precedent set by AfxHookSource2/Deadlock/DeadlockOverlayMath.h (see its own "WHY THE BASIS
//       MATH IS DUPLICATED HERE" note, lines 20-28) and
//       AfxHookSource2/Filmmaker/Deadlock/DeadlockGoToMath.h (lines 17-21): this repo's existing
//       *Math.h contract is header-only, C++17, no .cpp companion, so a standalone test can compile
//       against nothing but the one file.
//   - AfxHookSource2/Filmmaker/Panorama/CameraEditorFollowInspectorJs.h (the Framing page):
//       lines 310-312 (Off X), 313-315 (Off Y), 316-318 (Off Z), 319-327 (Pitch/Yaw/Roll) and the
//       per-frame summary note text at lines 405-407 all say the same thing the C++ does: for
//       LOCK-ON, Off X/Y/Z is a WORLD-axis offset on the AIM POINT and Pitch/Yaw/Roll TRIMS the
//       resulting look; for ATTACH, Off X/Y/Z is a LOCAL offset (forward/right/up in the mount's own
//       axes) and Pitch/Yaw/Roll is a rotation offset COMPOSED onto the base orientation. No
//       disagreement was found between the JS comment and the C++ it describes.
//
// WHAT THIS FILE DELIBERATELY DOES NOT COVER (gaps, stated rather than silently dropped):
//   - Teleport-distance detection (CS2's kTeleportDistance / Deadlock's own distance check before
//     trusting a velocity estimate) is a per-frame HISTORY decision the caller's solve-state owns,
//     not a formula -- EstimateVelocity below is the raw divide; deciding whether to trust it (and
//     resetting the smoothing filter on a teleport) stays engine-side, same as it already is in
//     both FollowCamera.cpp and DeadlockFollowCamera.cpp.
//   - CS2's dropped-weapon/dropped-bomb death RETARGET (switchToDroppedWeaponOnDeath /
//     switchToDroppedBombOnDeath, and the extra +0.75s grace while waiting for one) has no Deadlock
//     equivalent target type (see DeadlockFollowCamera.h: FollowSettings has no such fields), so it
//     is not reproduced here. EvaluateDeathHold() below covers only the hold/release timing core.
//
// Namespace mirrors the sibling headers (Deadlock::) with its own sub-namespace so the plain names
// (Vec3, Angles) don't collide with Deadlock::OverlayVec3 or Filmmaker::FollowVec3 -- this is a
// LOCAL copy of the small vector/angle shape, not a shared type, same call this repo already made
// for DeadlockOverlayMath.h's OverlayVec3.

#include <algorithm>
#include <cmath>

namespace Deadlock {
namespace FollowSolve {

struct Vec3 {
	double x = 0.0;
	double y = 0.0;
	double z = 0.0;
};

struct Angles {
	double pitch = 0.0;
	double yaw = 0.0;
	double roll = 0.0;
};

namespace detail {
constexpr double kPi = 3.14159265358979323846;
constexpr double kDeg2Rad = kPi / 180.0;
constexpr double kRad2Deg = 180.0 / kPi;
}

// ============================================================================================
// Small pure primitives, reproduced from FollowCameraMath.cpp (see the file header for exactly
// which lines). Each one is independently testable and is exactly what CS2 does bit-for-bit,
// not an approximation, so a caller can swap this file in without changing behaviour.
// ============================================================================================

// FollowCameraMath.cpp lines 8-12.
inline double WrapDegrees(double value) {
	// In one step: A wrap that walks 360 at a time never finishes on a huge value (a saved yaw of 4.7e10 froze the game on every load).
	return std::isfinite(value) ? std::remainder(value, 360.0) : 0.0;
}

// FollowCameraMath.cpp lines 14-20.
inline double HalfTimeAlpha(double halfTimeSeconds, double deltaSeconds) {
	if (deltaSeconds <= 0.0) return 0.0;
	if (halfTimeSeconds <= 0.0001) return 1.0;
	return 1.0 - std::pow(0.5, deltaSeconds / halfTimeSeconds);
}

// FollowCameraMath.cpp lines 22-31. Roll is left at Angles' default (0) -- FollowLookAt never sets
// one; CS2's own callers (FollowCamera.cpp line 892, and the lock-on trim at 898-903) supply roll
// from elsewhere.
inline Angles LookAt(const Vec3& camera, const Vec3& target) {
	const double dx = target.x - camera.x;
	const double dy = target.y - camera.y;
	const double dz = target.z - camera.z;
	const double horizontal = std::sqrt(dx * dx + dy * dy);
	Angles out;
	out.yaw = std::atan2(dy, dx) * detail::kRad2Deg;
	out.pitch = std::atan2(-dz, horizontal) * detail::kRad2Deg;
	return out;
}

// FollowCameraMath.cpp lines 62-75. Source AngleVectors basis (forward/right/up) from
// pitch/yaw/roll; rotates a LOCAL offset (x=forward, y=right, z=up) into world space.
inline Vec3 RotateVector(const Vec3& localOffset, const Angles& angles) {
	const double sp = std::sin(angles.pitch * detail::kDeg2Rad), cp = std::cos(angles.pitch * detail::kDeg2Rad);
	const double sy = std::sin(angles.yaw * detail::kDeg2Rad), cy = std::cos(angles.yaw * detail::kDeg2Rad);
	const double sr = std::sin(angles.roll * detail::kDeg2Rad), cr = std::cos(angles.roll * detail::kDeg2Rad);
	const Vec3 forward{ cp * cy, cp * sy, -sp };
	const Vec3 right{ -sr * sp * cy + cr * sy, -sr * sp * sy - cr * cy, -sr * cp };
	const Vec3 up{ cr * sp * cy + sr * sy, cr * sp * sy - sr * cy, cr * cp };
	return Vec3{
		forward.x * localOffset.x + right.x * localOffset.y + up.x * localOffset.z,
		forward.y * localOffset.x + right.y * localOffset.y + up.y * localOffset.z,
		forward.z * localOffset.x + right.z * localOffset.y + up.z * localOffset.z
	};
}

// FollowCameraMath.cpp lines 77-91. Inverse of RotateVector via the same orthonormal basis.
inline Vec3 InverseRotateVector(const Vec3& worldOffset, const Angles& angles) {
	const double sp = std::sin(angles.pitch * detail::kDeg2Rad), cp = std::cos(angles.pitch * detail::kDeg2Rad);
	const double sy = std::sin(angles.yaw * detail::kDeg2Rad), cy = std::cos(angles.yaw * detail::kDeg2Rad);
	const double sr = std::sin(angles.roll * detail::kDeg2Rad), cr = std::cos(angles.roll * detail::kDeg2Rad);
	const Vec3 forward{ cp * cy, cp * sy, -sp };
	const Vec3 right{ -sr * sp * cy + cr * sy, -sr * sp * sy - cr * cy, -sr * cp };
	const Vec3 up{ cr * sp * cy + sr * sy, cr * sp * sy - sr * cy, cr * cp };
	return Vec3{
		worldOffset.x * forward.x + worldOffset.y * forward.y + worldOffset.z * forward.z,
		worldOffset.x * right.x + worldOffset.y * right.y + worldOffset.z * right.z,
		worldOffset.x * up.x + worldOffset.y * up.y + worldOffset.z * up.z
	};
}

// FollowCameraMath.h lines 78-85, copied verbatim (renamed to this file's types) rather than
// linked against -- see the file header. Recovers pitch/yaw/roll from a forward+up basis.
inline Angles AnglesFromBasis(const Vec3& forward, const Vec3& up) {
	const double pitch = std::atan2(-forward.z, std::hypot(forward.x, forward.y)) * detail::kRad2Deg;
	const double yaw = std::atan2(forward.y, forward.x) * detail::kRad2Deg;
	const Angles level{ pitch, yaw, 0.0 };
	const Vec3 localUp = InverseRotateVector(up, level);
	return Angles{ pitch, yaw, std::atan2(localUp.y, localUp.z) * detail::kRad2Deg };
}

// FollowCameraMath.h lines 87-96, copied verbatim. Composes a LOCAL rotation offset onto a PARENT
// orientation (attach's rotation-offset rule, FollowCamera.cpp line 721). The Y flip is the header's
// own documented boundary between the right-handed XYZ frame rotation composition uses and Source's
// forward/right/up basis, whose Y has the opposite sign -- reproduced exactly, not re-derived.
inline Angles ComposeRotation(const Angles& parent, const Angles& local) {
	const Vec3 forward = RotateVector(Vec3{ 1, 0, 0 }, local);
	const Vec3 up = RotateVector(Vec3{ 0, 0, 1 }, local);
	return AnglesFromBasis(
		RotateVector(Vec3{ forward.x, -forward.y, forward.z }, parent),
		RotateVector(Vec3{ up.x, -up.y, up.z }, parent));
}

// FollowCameraMath.cpp lines 140-145. Plain Euclidean distance; used by callers to gate teleport
// detection (see the "what this file does not cover" note above) and by this file's own tests.
inline double Distance(const Vec3& a, const Vec3& b) {
	const double dx = b.x - a.x, dy = b.y - a.y, dz = b.z - a.z;
	return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// FollowCameraMath.cpp lines 93-125, reproduced EXACTLY including its one quirk: inside the
// deadzone the function returns `current` completely unchanged -- roll included -- even though
// outside the deadzone roll always snaps straight to `desired.roll` (never smoothed). Getting this
// "wrong in the same way as CS2" matters more than getting it "right": a caller that alternates
// just inside/outside the deadzone must see the same roll behaviour CS2 produces.
inline Angles SmoothAngles(const Angles& current, const Angles& desired, double deltaSeconds,
	double halfTimeSeconds, double deadzoneDegrees, double maxTurnDegreesPerSecond) {
	Angles out = current;
	double pitchDelta = WrapDegrees(desired.pitch - current.pitch);
	double yawDelta = WrapDegrees(desired.yaw - current.yaw);
	const double error = std::sqrt(pitchDelta * pitchDelta + yawDelta * yawDelta);
	if (error <= (std::max)(0.0, deadzoneDegrees))
		return out;

	const double alpha = HalfTimeAlpha(halfTimeSeconds, deltaSeconds);
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

	out.pitch = WrapDegrees(current.pitch + pitchDelta);
	out.yaw = WrapDegrees(current.yaw + yawDelta);
	out.roll = desired.roll;
	return out;
}

// FollowCameraMath.cpp lines 127-138.
inline Vec3 SmoothPosition(const Vec3& current, const Vec3& desired, double deltaSeconds, double halfTimeSeconds) {
	const double alpha = HalfTimeAlpha(halfTimeSeconds, deltaSeconds);
	return Vec3{
		current.x + (desired.x - current.x) * alpha,
		current.y + (desired.y - current.y) * alpha,
		current.z + (desired.z - current.z) * alpha
	};
}

// FollowCamera.cpp lines 722-726 (attach) and 883-886 (lock-on): the very first frame after a
// (re)place/retarget has no history to smooth from, so it snaps straight to the desired pose
// instead of easing in from an unrelated position. `initialized` is the caller's own
// "do I have a previous smoothed value" flag (CS2's m_motionInitialized).
//
// NOTE: CS2 also has a redundant-looking shortcut on LATER frames -- lines 888-889 skip
// FollowSmoothPosition entirely when positionSmoothing<=0.0001, and lines 893-896 skip
// FollowSmoothAngles when lookSmoothing<=0.0001 && maxTurnSpeed<=0.0 && deadzone<=0.0. Both
// shortcuts are mathematically redundant, not semantically different: HalfTimeAlpha already
// returns exactly 1.0 for halfTimeSeconds<=0.0001 (line 17-18 above), which makes
// SmoothPosition/SmoothAngles collapse to "= desired" on their own in that case (SmoothAngles'
// deadzone/maxTurn checks are likewise no-ops when deadzone<=0 and maxTurn<=0, since a deadzone of
// exactly 0 only short-circuits on an exact zero-error match and the maxTurn clamp requires
// maxTurnDegreesPerSecond>0.0 to engage at all). So this file does not reproduce those two
// early-return branches separately -- calling SmoothPosition/SmoothAngles unconditionally on every
// non-first frame already matches CS2's output.
inline Vec3 SmoothOrSnapPosition(bool initialized, const Vec3& current, const Vec3& desired,
	double deltaSeconds, double halfTimeSeconds) {
	return initialized ? SmoothPosition(current, desired, deltaSeconds, halfTimeSeconds) : desired;
}
inline Angles SmoothOrSnapAngles(bool initialized, const Angles& current, const Angles& desired,
	double deltaSeconds, double halfTimeSeconds, double deadzoneDegrees, double maxTurnDegreesPerSecond) {
	return initialized ? SmoothAngles(current, desired, deltaSeconds, halfTimeSeconds, deadzoneDegrees, maxTurnDegreesPerSecond)
		: desired;
}

// FollowCamera.cpp lines 685-690 (attach) and 855-858 (lock-on): both branches estimate target
// velocity the same way, a plain backward difference guarded by dt>0. Whether to TRUST the result
// (teleport detection, freeze on a paused demo) is the caller's history to keep; this is only the
// arithmetic.
inline Vec3 EstimateVelocity(const Vec3& previous, const Vec3& current, double deltaSeconds) {
	if (!(deltaSeconds > 0.0)) return Vec3{};
	return Vec3{
		(current.x - previous.x) / deltaSeconds,
		(current.y - previous.y) / deltaSeconds,
		(current.z - previous.z) / deltaSeconds
	};
}

// ============================================================================================
// LOCK-ON. The camera's own position is a caller-supplied constant (wherever PlaceCamera() /
// CameraBridge_GetCurrentCamera put it) and is NEVER computed here -- there is deliberately no
// function in this file that takes yaw/pitch/dist/height/lateral orbit sliders, because that model
// is exactly the bug being retired. LockOnCameraPosition below exists only so a test (and a caller)
// has a named, obviously-correct seam to assert the identity against, instead of quietly inlining
// "just don't call anything" and losing the guarantee if someone later adds orbit math back in.
// ============================================================================================

// FollowCamera.cpp lines 124-131 (written once, by PlaceCamera) and lines 904, 907-910, 926 (read
// every frame, never written): the lock-on camera position this frame IS the placed position.
inline Vec3 LockOnCameraPosition(const Vec3& placedCameraPosition) {
	return placedCameraPosition;
}

// FollowCamera.cpp lines 865-869: the AIM POINT (not the camera position) is the target plus a
// WORLD-axis offset (the Off X/Y/Z sliders in lock-on -- CameraEditorFollowInspectorJs.h lines
// 310-318) plus a velocity*prediction lead.
inline Vec3 LockOnAimPoint(const Vec3& targetPosition, const Vec3& targetVelocity,
	double predictionSeconds, const Vec3& worldAimOffset) {
	return Vec3{
		targetPosition.x + worldAimOffset.x + targetVelocity.x * predictionSeconds,
		targetPosition.y + worldAimOffset.y + targetVelocity.y * predictionSeconds,
		targetPosition.z + worldAimOffset.z + targetVelocity.z * predictionSeconds
	};
}

// FollowCamera.cpp line 892: aim FROM the fixed camera position AT the (smoothed) aim point.
inline Angles LockOnDesiredAngles(const Vec3& fixedCameraPosition, const Vec3& smoothedAimPoint) {
	return LookAt(fixedCameraPosition, smoothedAimPoint);
}

// FollowCamera.cpp lines 898-903: the rotation offset TRIMS pitch/yaw (plain addition, not
// FollowComposeRotation -- that is attach's rule, not lock-on's, per the C++; confirmed against
// CameraEditorFollowInspectorJs.h lines 319-327 and the per-frame note at 405-407, which say the
// same thing ("rotation trims the aim") and do not disagree with the C++). Roll is NOT the smoothed
// aim's roll (LookAt never produces one) -- it is `placedCameraRoll` (m_state.cameraAngles.roll,
// the camera's own roll from when it was placed) plus the roll trim.
inline Angles LockOnTrimAngles(const Angles& smoothedAimAngles, const Angles& rotationTrim, double placedCameraRoll) {
	return Angles{
		WrapDegrees(smoothedAimAngles.pitch + rotationTrim.pitch),
		WrapDegrees(smoothedAimAngles.yaw + rotationTrim.yaw),
		WrapDegrees(placedCameraRoll + rotationTrim.roll)
	};
}

// ============================================================================================
// Keep untrimmed tracking history separate from the returned view. Feeding the final
// trimmed angle back into smoothing adds the trim every render frame, even at dt=0.
struct LockOnAngleFilter {
	Angles aim;
	bool initialized = false;

	Angles Sample(const Angles& desired, double deltaSeconds, double halfTimeSeconds,
		double deadzoneDegrees, double maxTurnDegreesPerSecond,
		const Angles& trim, double placedRoll) {
		aim = SmoothOrSnapAngles(initialized, aim, desired, deltaSeconds,
			halfTimeSeconds, deadzoneDegrees, maxTurnDegreesPerSecond);
		initialized = true;
		return LockOnTrimAngles(aim, trim, placedRoll);
	}
};

// ATTACH. The camera rides the target: position = mount origin + a LOCAL offset (rotated into the
// mount's own frame) + a velocity lead; rotation = the mount's base orientation with a rotation
// offset COMPOSED on top (not trimmed -- that is lock-on's rule, not attach's).
// ============================================================================================

// FollowCamera.cpp line 715: the Off X/Y/Z sliders in attach mode are a LOCAL offset (forward=x,
// right=y, up=z per CameraEditorFollowInspectorJs.h lines 310-318 / 405-406) rotated into world
// space by the mount's own base angles.
inline Vec3 AttachWorldOffset(const Vec3& localOffset, const Angles& baseAngles) {
	return RotateVector(localOffset, baseAngles);
}

// FollowCamera.cpp lines 716-720: desiredPos = mount origin + the rotated local offset + a
// velocity*prediction lead.
inline Vec3 AttachDesiredPosition(const Vec3& mountOrigin, const Vec3& worldOffset,
	const Vec3& targetVelocity, double predictionSeconds) {
	return Vec3{
		mountOrigin.x + worldOffset.x + targetVelocity.x * predictionSeconds,
		mountOrigin.y + worldOffset.y + targetVelocity.y * predictionSeconds,
		mountOrigin.z + worldOffset.z + targetVelocity.z * predictionSeconds
	};
}

// FollowCamera.cpp line 721: the rotation offset (Pitch/Yaw/Roll sliders) is COMPOSED onto the base
// orientation via FollowComposeRotation's parent/local rule -- reproduced as ComposeRotation above.
inline Angles AttachDesiredAngles(const Angles& baseAngles, const Angles& rotationOffset) {
	return ComposeRotation(baseAngles, rotationOffset);
}

// FollowCamera.cpp lines 727-731: smoothing (via SmoothOrSnapAngles) covers pitch/yaw only; roll
// always snaps straight to the desired value on every frame, smoothed or not (line 731: "smoothing
// covers pitch/yaw only").
inline Angles AttachOutputAngles(bool initialized, const Angles& currentOutputAngles, const Angles& desiredAngles,
	double deltaSeconds, double lookHalfTimeSeconds, double deadzoneDegrees, double maxTurnDegreesPerSecond) {
	Angles out = SmoothOrSnapAngles(initialized, currentOutputAngles, desiredAngles,
		deltaSeconds, lookHalfTimeSeconds, deadzoneDegrees, maxTurnDegreesPerSecond);
	out.roll = desiredAngles.roll;
	return out;
}

// ============================================================================================
// DEATH HOLD / RELEASE.
//
// FollowCamera.cpp lines 629-653 is CS2's rule: `stop` (= release) depends ONLY on
// `autoDisableOnDeath` and `deathHoldSeconds` (line 641:
// `stop = !retargeted && autoDisableOnDeath && holdExpired && (!wantsRetarget || elapsed >=
// deathHoldSeconds+0.75)`). `holdLastKnownPosition` never appears in that expression at all -- in
// CS2 it instead gates an UNRELATED case, "the provider can't resolve a transform right now" (lines
// 655 and 845: `if (!provider->IsValid() && (!holdLastKnownPosition || !haveLastKnownTarget)) stop`),
// which has nothing to do with the death timer. Since Deadlock has no dropped-weapon/dropped-bomb
// retarget target (see the file header's "what this does not cover"), `wantsRetarget` is always
// false for Deadlock and CS2's rule reduces to: `releaseOnDeath=true` ALWAYS releases once
// `deathHoldSeconds` elapses, REGARDLESS of `holdLast`.
//
// THIS IS NOT WHAT THIS FUNCTION IMPLEMENTS. Deadlock's own current, shipped
// DeadlockFollowCamera.cpp (lines 247-253) makes `holdLast` override that release:
//
//     const bool holdingDead = g_solve.targetDead && g_s.releaseOnDeath && g_solve.deadFor >= g_s.deathHold;
//     if (holdingDead && !g_s.holdLast) { g_solve.released = true; return false; }
//
// i.e. `releaseOnDeath=true` + `holdLast=true` keeps holding the last pose FOREVER past deathHold,
// never releasing. That is a genuinely different shape from CS2's, not just a naming difference.
// Per this task's own instructions ("match Deadlock's CURRENT holdLast semantics, unless CS2 means
// something importantly different, in which case follow CS2 and note the change") and per the
// task's own acceptance tests (which ask for exactly Deadlock's current combined behaviour), this
// function reproduces DEADLOCK'S rule, not CS2's -- this comment is the record of the disagreement
// and why it is intentional. The CORE timing rule both share (a target is not released until it has
// been dead for at least deathHoldSeconds) is preserved either way.
struct DeathHoldDecision {
	bool release = false;      // stop claiming the view entirely this frame (Deadlock: `return false`)
	bool holdLastPose = false; // dead, past deathHold, but not released: freeze the last pose
};

// FollowCamera.cpp lines 630-631 (`m_deathRetargetElapsed += deathDt;` while targetDead) and line
// 652 (`m_deathRetargetElapsed = 0.0;` the instant it is not) / DeadlockFollowCamera.cpp lines
// 242-245 (`g_solve.deadFor += dt;` / reset to 0 on `!dead`). Both engines run the identical rule.
inline double AdvanceDeadElapsed(double previousElapsed, bool isDead, double deltaSeconds) {
	if (!isDead) return 0.0;
	return previousElapsed + ((deltaSeconds > 0.0) ? deltaSeconds : 0.0);
}

inline DeathHoldDecision EvaluateDeathHold(bool isDead, double deadElapsedSeconds, double deathHoldSeconds,
	bool releaseOnDeath, bool holdLast) {
	DeathHoldDecision out;
	if (!isDead) return out;                              // alive: neither release nor hold
	if (!releaseOnDeath) return out;                       // "Keep following": never releases
	if (deadElapsedSeconds < deathHoldSeconds) return out; // still inside the hold window: keep tracking
	if (holdLast) { out.holdLastPose = true; return out; } // past the hold window, but told to keep holding
	out.release = true;                                    // past the hold window: let go
	return out;
}

} // namespace FollowSolve
} // namespace Deadlock
