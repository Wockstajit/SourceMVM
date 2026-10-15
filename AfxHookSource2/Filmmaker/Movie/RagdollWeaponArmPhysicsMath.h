#pragma once

#include "RagdollWeaponMath.h"
#include <cmath>

namespace Filmmaker {

struct RagdollWeaponArmImpulse {
	float force[3] = {};
};

// Source weapon attachments point down local +X. Recoil pushes opposite the barrel
// with a small upward component so the elbow and shoulder visibly absorb the shot.
inline RagdollWeaponArmImpulse RagdollWeaponMakeArmImpulse(
	const RagdollWeaponQuat& muzzleRotation, double strength) {
	RagdollWeaponQuat q = muzzleRotation;
	if (!RagdollWeaponNormalize(q) || !std::isfinite(strength) || strength <= 0.0)
		return {};
	const RagdollWeaponQuat local{-1.0, 0.0, 0.18, 0.0};
	const RagdollWeaponQuat conjugate{-q.x, -q.y, -q.z, q.w};
	const auto world = RagdollWeaponQuatMul(RagdollWeaponQuatMul(q, local), conjugate);
	const double length = std::sqrt(world.x*world.x + world.y*world.y + world.z*world.z);
	if (!std::isfinite(length) || length < 1e-9) return {};
	return {{(float)(world.x*strength/length), (float)(world.y*strength/length),
		(float)(world.z*strength/length)}};
}

} // namespace Filmmaker
