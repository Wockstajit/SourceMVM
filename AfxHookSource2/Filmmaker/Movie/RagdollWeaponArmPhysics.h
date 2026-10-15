#pragma once

class CEntityInstance;

namespace Filmmaker {

// Queue a shot impulse, then deliver it to CS2's articulated ragdoll during a
// simulation stage. The engine maps the skeleton bone to its physics body.
void RagdollWeaponArmPhysics_Queue(CEntityInstance* corpse, int userId,
	const char* weaponClass, const float position[3], const float rotation[4], double strength = 1800.0);
void RagdollWeaponArmPhysics_BeforeClientStage(int stage);
void RagdollWeaponArmPhysics_Forget(CEntityInstance* entity);
void RagdollWeaponArmPhysics_Clear();

} // namespace Filmmaker
