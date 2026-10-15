#include "../../AfxHookSource2/Filmmaker/Deadlock/DeadlockFollowSolveMath.h"
#include <cmath>
#include <cstdio>

// Deadlock's follow camera math core, checked against CS2's own Movie/FollowCamera.cpp spec: see
// DeadlockFollowSolveMath.h's file header for exactly which CS2 lines each rule below is grounded
// in. The one requirement this whole file exists to lock in: LOCK-ON keeps the placed camera
// POSITION fixed and only turns to aim; ATTACH moves the camera with the target. The old (wrong)
// Deadlock model recomputed the lock-on position every frame by orbiting sliders around the
// (possibly moving) target -- the "would fail against the old model" section below reproduces that
// formula just far enough to prove the new API does not have its bug.
int main() {
	int failures = 0;
	auto check = [&](bool value, const char* what) {
		if (!value) { ++failures; std::printf("  FAIL %s\n", what); }
	};

	using namespace Deadlock::FollowSolve;
	constexpr double kEps = 1e-6;
	constexpr double kPi = 3.14159265358979323846;

	// ===================================================================================
	// LOCK-ON: the returned camera position is the placed position, verbatim, every call,
	// regardless of target motion. FollowCamera.cpp lines 124-131 / 904 / 907-910 / 926.
	// ===================================================================================
	{
		const Vec3 placed{ 512.0, -128.0, 64.0 };
		const Vec3 targetFrame1{ 0.0, 0.0, 0.0 };
		const Vec3 targetFrame2{ 900.0, 400.0, 200.0 }; // wildly different target position
		const Vec3 targetFrame3{ -900.0, -400.0, -50.0 };

		const Vec3 pos1 = LockOnCameraPosition(placed);
		const Vec3 pos2 = LockOnCameraPosition(placed);
		const Vec3 pos3 = LockOnCameraPosition(placed);
		(void)targetFrame1; (void)targetFrame2; (void)targetFrame3;

		check(pos1.x == placed.x && pos1.y == placed.y && pos1.z == placed.z,
			"lock-on: camera position equals the placed position (frame 1)");
		check(pos2.x == placed.x && pos2.y == placed.y && pos2.z == placed.z,
			"lock-on: camera position equals the placed position (frame 2, different target)");
		check(pos3.x == placed.x && pos3.y == placed.y && pos3.z == placed.z,
			"lock-on: camera position equals the placed position (frame 3, different target again)");
	}

	// ===================================================================================
	// This test would FAIL against the OLD (orbit-based) Deadlock lock-on model. Reproduced
	// from DeadlockFollowCamera.cpp lines 319-327 ONLY to demonstrate the contrast; this repo's
	// production code no longer contains this formula once DeadlockFollowSolveMath.h is wired in.
	// ===================================================================================
	{
		auto OldOrbitPosition = [&](const Vec3& target, double yawDeg, double pitchDeg,
			double dist, double lateral, double height) {
			const double yawRad = yawDeg * kPi / 180.0;
			const double pitchRad = pitchDeg * kPi / 180.0;
			const double horiz = dist * std::cos(pitchRad);
			return Vec3{
				target.x - horiz * std::cos(yawRad) - lateral * std::sin(yawRad),
				target.y - horiz * std::sin(yawRad) + lateral * std::cos(yawRad),
				target.z + dist * std::sin(pitchRad) + height
			};
		};
		const Vec3 targetBefore{ 0.0, 0.0, 0.0 };
		const Vec3 targetAfterSidestep{ 0.0, 500.0, 0.0 }; // hero sidesteps 500 units on Y only
		const Vec3 oldBefore = OldOrbitPosition(targetBefore, 35.0, 22.0, 340.0, 90.0, 60.0);
		const Vec3 oldAfter = OldOrbitPosition(targetAfterSidestep, 35.0, 22.0, 340.0, 90.0, 60.0);
		check(std::fabs(oldAfter.y - oldBefore.y) > 100.0,
			"sanity: the OLD orbit formula DOES move the camera when the target sidesteps (the bug)");

		// The new API takes no target at all for the camera's position -- it is structurally
		// impossible for a target sidestep to move it.
		const Vec3 placed{ 512.0, -128.0, 64.0 };
		const Vec3 newBefore = LockOnCameraPosition(placed);
		const Vec3 newAfter = LockOnCameraPosition(placed);
		check(newBefore.x == newAfter.x && newBefore.y == newAfter.y && newBefore.z == newAfter.z,
			"lock-on: NEW model is invariant to the same target sidestep that broke the OLD model");
	}

	// ===================================================================================
	// LOCK-ON: a world-space aim-point offset actually shifts the look-at target, and in the
	// expected direction. FollowCamera.cpp lines 865-869 (offset) and 892 (LookAt using it).
	// ===================================================================================
	{
		const Vec3 camera{ 0.0, 0.0, 0.0 };
		const Vec3 target{ 100.0, 0.0, 0.0 }; // level with the camera
		const Vec3 noOffset{ 0.0, 0.0, 0.0 };
		const Vec3 upOffset{ 0.0, 0.0, 50.0 }; // Off Z = +50: aim point raised above the target

		const Vec3 aimNoOffset = LockOnAimPoint(target, Vec3{}, 0.0, noOffset);
		const Vec3 aimUpOffset = LockOnAimPoint(target, Vec3{}, 0.0, upOffset);
		check(aimUpOffset.z > aimNoOffset.z + kEps, "lock-on: +Z aim offset actually raises the aim point");

		const Angles angNoOffset = LockOnDesiredAngles(camera, aimNoOffset);
		const Angles angUpOffset = LockOnDesiredAngles(camera, aimUpOffset);
		check(std::fabs(angNoOffset.pitch) < kEps, "lock-on: level target with no offset pitches level");
		// Positive pitch looks DOWN (FollowLookAt: pitch = atan2(-dz, horizontal); dz>0 means the aim
		// point is ABOVE the camera, which must produce a NEGATIVE pitch, i.e. looking up).
		check(angUpOffset.pitch < angNoOffset.pitch - kEps,
			"lock-on: raising the aim point makes the camera pitch UP (more negative pitch)");
	}

	// ===================================================================================
	// LOCK-ON: a pitch/yaw/roll trim of (0, 90, 0) rotates the resulting yaw by 90 degrees from
	// what a zero trim gives. FollowCamera.cpp lines 898-903 (plain addition, not composition).
	// ===================================================================================
	{
		const Vec3 camera{ 0.0, 0.0, 0.0 };
		const Vec3 aimPoint{ 100.0, 37.0, -12.0 }; // an arbitrary non-axis-aligned direction
		const Angles baseAim = LockOnDesiredAngles(camera, aimPoint);

		const Angles zeroTrim = LockOnTrimAngles(baseAim, Angles{ 0.0, 0.0, 0.0 }, 0.0);
		const Angles yaw90Trim = LockOnTrimAngles(baseAim, Angles{ 0.0, 90.0, 0.0 }, 0.0);

		const double yawDiff = WrapDegrees(yaw90Trim.yaw - zeroTrim.yaw);
		check(std::fabs(yawDiff - 90.0) < 1e-4, "lock-on: a yaw trim of 90 rotates the result by exactly 90");
		check(std::fabs(WrapDegrees(zeroTrim.pitch - baseAim.pitch)) < kEps,
			"lock-on: zero trim leaves pitch unchanged from the raw aim");
		check(std::fabs(WrapDegrees(yaw90Trim.pitch - baseAim.pitch)) < kEps,
			"lock-on: a YAW-only trim does not disturb pitch (plain per-axis addition, not composition)");

		// Roll comes from the PLACED camera's own roll, not from the (roll-less) look-at result.
		const Angles rolledTrim = LockOnTrimAngles(baseAim, Angles{ 0.0, 0.0, 0.0 }, /*placedCameraRoll=*/15.0);
		check(std::fabs(rolledTrim.roll - 15.0) < kEps, "lock-on: roll comes from the placed camera's own roll");
	}

	// ===================================================================================
	// ATTACH: convention check. FollowRotateVector's forward at pitch=yaw=roll=0 is (1,0,0), so
	// yaw 0 faces +X (DeadlockFollowSolveMath.h RotateVector, matching FollowCameraMath.cpp
	// lines 62-75's `forward{ cp*cy, cp*sy, -sp }` at cp=cy=1, sp=sy=0).
	// A local offset of (forward=-100, right=0, up=0) -- "back 100 units" -- at heroYaw=0 must
	// therefore land 100 units in the -X direction.
	// ===================================================================================
	{
		const Vec3 mountOrigin{ 0.0, 0.0, 0.0 };
		const Angles heroYaw0{ 0.0, 0.0, 0.0 };
		const Vec3 localBack{ -100.0, 0.0, 0.0 };

		const Vec3 worldOffset = AttachWorldOffset(localBack, heroYaw0);
		check(std::fabs(worldOffset.x - (-100.0)) < kEps && std::fabs(worldOffset.y) < kEps
			&& std::fabs(worldOffset.z) < kEps,
			"attach: yaw 0, local 'back 100' offset is -100 on world X (confirms yaw 0 faces +X)");

		const Vec3 desired = AttachDesiredPosition(mountOrigin, worldOffset, Vec3{}, 0.0);
		check(std::fabs(desired.x - (-100.0)) < kEps && std::fabs(desired.y) < kEps && std::fabs(desired.z) < kEps,
			"attach: mount at origin + 'back 100' offset produces world position (-100, 0, 0)");
	}

	// ===================================================================================
	// ATTACH: the SAME local offset at heroYaw=90 rotates into a different world direction --
	// the axis changes from X to Y, matching the hero having turned 90 degrees.
	// ===================================================================================
	{
		const Angles heroYaw90{ 0.0, 90.0, 0.0 };
		const Vec3 localBack{ -100.0, 0.0, 0.0 };
		const Vec3 worldOffset = AttachWorldOffset(localBack, heroYaw90);
		check(std::fabs(worldOffset.x) < kEps, "attach: at heroYaw=90 the same offset no longer moves X");
		check(worldOffset.y < -100.0 + kEps && worldOffset.y > -100.0 - kEps,
			"attach: at heroYaw=90 'back 100' now lands on world Y = -100 (forward rotated to +Y)");
	}

	// ===================================================================================
	// ATTACH: rotation offset composes onto the base angles via ComposeRotation (FollowCamera.cpp
	// line 721 / FollowCameraMath.h lines 90-96). Three hand-verified cases.
	// ===================================================================================
	{
		// Case A: base yaw=0 + rotation-offset yaw=45 -> desired yaw=45.
		// By hand: local={pitch0,yaw45,roll0} gives forward=(cos45,sin45,0), up=(0,0,1); the Y-flip
		// leaves both unchanged on their (zero) components that matter, and rotating by an identity
		// parent (yaw 0) leaves them as-is, so AnglesFromBasis((.7071,.7071,0), (0,0,1)) reads back
		// pitch=atan2(0, 1)=0, yaw=atan2(.7071,.7071)=45, roll=0.
		{
			const Angles result = AttachDesiredAngles(Angles{ 0.0, 0.0, 0.0 }, Angles{ 0.0, 45.0, 0.0 });
			check(std::fabs(WrapDegrees(result.yaw - 45.0)) < 1e-4, "attach compose: base yaw 0 + offset yaw 45 -> yaw 45");
			check(std::fabs(result.pitch) < 1e-4, "attach compose: pure yaw offset leaves pitch at 0");
		}
		// Case B: base yaw=90 + rotation-offset yaw=0 (identity local) -> desired yaw=90 unchanged.
		// By hand: an identity local rotation composed onto ANY parent must return that parent's own
		// angles, since forward/up pass through RotateVector(_, identity) unchanged and the Y-flip on
		// zero-Y components is a no-op.
		{
			const Angles result = AttachDesiredAngles(Angles{ 0.0, 90.0, 0.0 }, Angles{ 0.0, 0.0, 0.0 });
			check(std::fabs(WrapDegrees(result.yaw - 90.0)) < 1e-4, "attach compose: identity offset leaves base yaw 90 unchanged");
			check(std::fabs(result.pitch) < 1e-4, "attach compose: identity offset leaves base pitch at 0");
		}
		// Case C (non-trivial, mixes pitch and yaw): base yaw=90 + rotation-offset pitch=45 ->
		// desired pitch=45, yaw=90 unchanged. Worked by hand (see DeadlockFollowSolveMath.h's
		// ComposeRotation comment for the formula being exercised):
		//   local={pitch45,yaw0,roll0}: forward=(cos45,0,-sin45)=(.7071,0,-.7071), up=(.7071,0,.7071).
		//   Y-flip is a no-op (both have y=0).
		//   parent={yaw90}: RotateVector(forward, parent) picks parent's own forward*.7071 + up*(-.7071)
		//     = (0,1,0)*.7071 + (0,0,1)*(-.7071) = (0, .7071, -.7071).
		//     RotateVector(up, parent) = forward*.7071 + up*.7071 = (0,.7071,0)+(0,0,.7071) = (0,.7071,.7071).
		//   AnglesFromBasis((0,.7071,-.7071), (0,.7071,.7071)):
		//     pitch = atan2(.7071, hypot(0,.7071)) = atan2(.7071,.7071) = 45.
		//     yaw   = atan2(.7071, 0) = 90.
		//     roll  = 0 (localUp comes out as (0,0,1) once re-expressed in the level(45,90) frame).
		{
			const Angles result = AttachDesiredAngles(Angles{ 0.0, 90.0, 0.0 }, Angles{ 45.0, 0.0, 0.0 });
			check(std::fabs(WrapDegrees(result.pitch - 45.0)) < 1e-3, "attach compose: mixed case yields pitch 45");
			check(std::fabs(WrapDegrees(result.yaw - 90.0)) < 1e-3, "attach compose: mixed case leaves yaw 90 unchanged");
			check(std::fabs(WrapDegrees(result.roll)) < 1e-3, "attach compose: mixed case leaves roll 0");
		}
	}

	// ===================================================================================
	// ATTACH: roll always snaps to the desired value, never smoothed (FollowCamera.cpp line 731).
	// ===================================================================================
	{
		const Angles current{ 0.0, 0.0, 10.0 };
		const Angles desired{ 5.0, 5.0, 80.0 };
		const Angles out = AttachOutputAngles(/*initialized=*/true, current, desired,
			/*dt=*/0.016, /*lookHalfTime=*/1.0, /*deadzone=*/0.0, /*maxTurn=*/0.0);
		check(std::fabs(out.roll - 80.0) < kEps, "attach: roll snaps straight to desired even with slow pitch/yaw smoothing");
	}

	// ===================================================================================
	// SmoothAngles quirk, reproduced faithfully from FollowCameraMath.cpp lines 93-125: inside
	// the deadzone the ENTIRE current pose (roll included) passes through unchanged, even though
	// outside the deadzone roll always snaps to `desired.roll` regardless of pitch/yaw smoothing.
	// ===================================================================================
	{
		const Angles current{ 0.0, 0.0, 10.0 };
		const Angles desiredInsideDeadzone{ 0.5, 0.5, 99.0 }; // small pitch/yaw error, big "roll change"
		const Angles insideOut = SmoothAngles(current, desiredInsideDeadzone, 0.1, 0.2, /*deadzone=*/5.0, 0.0);
		check(std::fabs(insideOut.roll - 10.0) < kEps,
			"SmoothAngles quirk: inside the deadzone, roll is untouched (stays at `current`), not `desired`");

		const Angles desiredOutsideDeadzone{ 40.0, 40.0, 99.0 }; // large pitch/yaw error
		const Angles outsideOut = SmoothAngles(current, desiredOutsideDeadzone, 0.1, 0.2, /*deadzone=*/5.0, 0.0);
		check(std::fabs(outsideOut.roll - 99.0) < kEps,
			"SmoothAngles quirk: outside the deadzone, roll snaps straight to `desired.roll`");
	}

	// ===================================================================================
	// DEATH HOLD: releaseOnDeath=false never releases, regardless of dead time.
	// ===================================================================================
	{
		check(!EvaluateDeathHold(true, 0.0, 2.0, /*releaseOnDeath=*/false, /*holdLast=*/false).release,
			"death: releaseOnDeath=false, dt=0 -> no release");
		check(!EvaluateDeathHold(true, 2.0, 2.0, false, false).release,
			"death: releaseOnDeath=false, dt==deathHold -> still no release");
		check(!EvaluateDeathHold(true, 1000.0, 2.0, false, true).release,
			"death: releaseOnDeath=false, dt far past deathHold, holdLast either way -> still no release");
	}

	// ===================================================================================
	// DEATH HOLD: releaseOnDeath=true + holdLast=false releases the instant deathHold elapses.
	// ===================================================================================
	{
		const DeathHoldDecision before = EvaluateDeathHold(true, 1.999, 2.0, true, false);
		check(!before.release && !before.holdLastPose, "death: releaseOnDeath=true, just under deathHold -> not released yet");
		const DeathHoldDecision atThreshold = EvaluateDeathHold(true, 2.0, 2.0, true, false);
		check(atThreshold.release, "death: releaseOnDeath=true, holdLast=false, dt==deathHold -> releases");
		const DeathHoldDecision after = EvaluateDeathHold(true, 5.0, 2.0, true, false);
		check(after.release, "death: releaseOnDeath=true, holdLast=false, dt well past deathHold -> released");
	}

	// ===================================================================================
	// DEATH HOLD: releaseOnDeath=true + holdLast=true keeps holding (never releases) past
	// deathHold, matching Deadlock's current holdLast semantics (see the long comment on
	// EvaluateDeathHold in the header for how/why this differs from CS2's own mechanism).
	// ===================================================================================
	{
		const DeathHoldDecision atThreshold = EvaluateDeathHold(true, 2.0, 2.0, true, true);
		check(!atThreshold.release && atThreshold.holdLastPose,
			"death: releaseOnDeath=true, holdLast=true, dt==deathHold -> holds, does not release");
		const DeathHoldDecision wayPast = EvaluateDeathHold(true, 500.0, 2.0, true, true);
		check(!wayPast.release && wayPast.holdLastPose,
			"death: releaseOnDeath=true, holdLast=true, dt far past deathHold -> still holds, never releases");
	}

	// ===================================================================================
	// DEATH HOLD: AdvanceDeadElapsed accumulates while dead and resets the instant it is not.
	// FollowCamera.cpp lines 630-631/652; DeadlockFollowCamera.cpp lines 242-245.
	// ===================================================================================
	{
		double elapsed = 0.0;
		elapsed = AdvanceDeadElapsed(elapsed, /*isDead=*/true, 0.5);
		check(std::fabs(elapsed - 0.5) < kEps, "deadElapsed: accumulates dt while dead (frame 1)");
		elapsed = AdvanceDeadElapsed(elapsed, true, 0.5);
		check(std::fabs(elapsed - 1.0) < kEps, "deadElapsed: accumulates dt while dead (frame 2)");
		elapsed = AdvanceDeadElapsed(elapsed, /*isDead=*/false, 0.5);
		check(elapsed == 0.0, "deadElapsed: resets to 0 the instant the target is no longer dead");
		elapsed = AdvanceDeadElapsed(elapsed, true, 0.1);
		check(std::fabs(elapsed - 0.1) < kEps, "deadElapsed: restarts cleanly from 0 on the next death");
	}

	// Repeated paused renders apply trim once; editing trim never alters tracking history.
	{
		LockOnAngleFilter filter;
		const Angles target{21.8, 175, 0};
		for (int i = 0; i < 1000; ++i) {
			const Angles out = filter.Sample(target, 0, .35, 1.5, 420, {5, 10, -20}, 15);
			check(std::fabs(out.pitch - 26.8) < kEps && std::fabs(out.yaw + 175) < kEps
				&& std::fabs(out.roll + 5) < kEps, "paused trim is stable across repeated frames and wrap");
		}
		const Angles edited = filter.Sample(target, 0, .35, 1.5, 420, {-5, -10, 20}, 15);
		check(std::fabs(edited.pitch - 16.8) < kEps && std::fabs(edited.yaw - 165) < kEps
			&& std::fabs(edited.roll - 35) < kEps, "paused trim edit takes effect once");
		const Angles cleared = filter.Sample(target, 0, .35, 1.5, 420, {}, 15);
		check(std::fabs(cleared.pitch - target.pitch) < kEps && std::fabs(cleared.yaw - target.yaw) < kEps,
			"clearing trim restores unmodified tracking aim immediately");
	}
	// Track a moving target with smoothing, speed limiting and duplicate renders per tick.
	// Trimmed output must equal untrimmed output plus trim, independent of frame count.
	{
		LockOnAngleFilter plain, trimmed;
		for (int i = 0; i < 1000; ++i) {
			const Angles target{20 * std::sin(i * .03), WrapDegrees(170 + i * .8), 0};
			const double dt = i % 3 == 0 ? 1.0 / 64 : 0;
			const Angles a = plain.Sample(target, dt, .35, 1.5, 50, {}, 12);
			const Angles b = trimmed.Sample(target, dt, .35, 1.5, 50, {5, 10, -20}, 12);
			check(std::fabs(WrapDegrees(b.pitch - a.pitch) - 5) < kEps
				&& std::fabs(WrapDegrees(b.yaw - a.yaw) - 10) < kEps
				&& std::fabs(b.roll + 8) < kEps, "moving trim never feeds the tracking filter");
		}
		trimmed = LockOnAngleFilter();
		const Angles reset = trimmed.Sample({-30, -40, 0}, 0, .35, 1.5, 50, {5, 10, 0}, 0);
		check(std::fabs(reset.pitch + 25) < kEps && std::fabs(reset.yaw + 30) < kEps,
			"seek, retarget and mode reset snap to a fresh aim");
	}

	std::printf("Deadlock follow-solve math: %d failures\n", failures);
	return failures ? 1 : 0;
}
