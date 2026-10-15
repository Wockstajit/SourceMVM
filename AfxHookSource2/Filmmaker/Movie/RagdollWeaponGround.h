#pragma once
#include "RagdollWeaponMath.h"

namespace Filmmaker {
// Uses the map collision the Environment loads for every map (independent of the editor UI).
void RagdollWeaponGround_Pump();
const char* RagdollWeaponGround_Status();
bool RagdollWeaponGround_Find(const float position[3], const float rotation[4],
    float& groundZ, RagdollWeaponQuat& groundRotation);
// Unfiltered map ray. Used for stairs (sloped floor)
// and walls; Death Weapon's Find still requires an upward support normal.
bool RagdollWeaponGround_Ray(const float origin[3], const float dir[3], float maxDist,
    float hitPos[3], float hitNormal[3]);
}
