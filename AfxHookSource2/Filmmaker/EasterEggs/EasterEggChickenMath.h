#pragma once

// Chicken Storm motion: a chicken drops from the sky, tumbles, smacks into the ground, bounces once
// or twice, and sits there looking confused until it is cleaned up. Plus the Source angle ->
// quaternion conversion the scene-node write needs. Engine-free so
// Automation/tests/easter-egg-chicken-math-tests.cpp can pin it.

#include "../Props/PropEditMath.h"

#include <cmath>

namespace Filmmaker {
namespace EasterEggChicken {

constexpr double kGravity = 520.0;        // u/s^2 -- slower than the game's 800: comedic hang time
constexpr double kBounce = 0.38;          // fraction of impact speed kept on a bounce
constexpr double kRestSpeed = 40.0;       // below this an impact does not bounce, it lands
constexpr double kSitSeconds = 3.5;       // time on the ground before it is removed

struct Bird {
	double z = 0.0, vz = 0.0;  // height and vertical speed (up positive)
	double groundZ = 0.0;
	double pitch = 0.0, roll = 0.0, yaw = 0.0;
	double spinPitch = 0.0, spinRoll = 0.0, spinYaw = 0.0; // deg/s while airborne
	bool landed = false;
	double sitTime = 0.0;
};

// Advances one bird by dt seconds. Returns false once it has sat long enough to be removed.
inline bool Step(Bird& b, double dt) {
	if (!(dt > 0.0)) return true;
	if (dt > 0.1) dt = 0.1;
	if (b.landed) {
		b.sitTime += dt;
		return b.sitTime < kSitSeconds;
	}
	b.vz -= kGravity * dt;
	b.z += b.vz * dt;
	b.pitch += b.spinPitch * dt;
	b.roll += b.spinRoll * dt;
	b.yaw += b.spinYaw * dt;
	if (b.z <= b.groundZ) {
		b.z = b.groundZ;
		const double impact = -b.vz;
		if (impact > kRestSpeed) {
			b.vz = impact * kBounce;
			b.spinPitch *= 0.5; b.spinRoll *= 0.5;
		} else {
			// Settle upright, still facing wherever it ended up.
			b.vz = 0.0; b.landed = true; b.pitch = 0.0; b.roll = 0.0;
		}
	}
	return true;
}

// The Source angle -> quaternion conversion lives with the prop editor's maths (Props::AngleQuaternion).
using Props::AngleQuaternion;

// Angles kept in [-180, 180) so they never grow without bound while a bird spins.
inline double WrapDegrees(double a) {
	a = std::fmod(a + 180.0, 360.0);
	if (a < 0.0) a += 360.0;
	return a - 180.0;
}

} // namespace EasterEggChicken
} // namespace Filmmaker
