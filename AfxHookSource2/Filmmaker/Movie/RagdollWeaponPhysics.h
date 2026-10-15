#pragma once
class CEntityInstance;
namespace Filmmaker {
struct RagdollWeaponPhysicsState {
    CEntityInstance* weapon = nullptr;
    void* aggregate = nullptr;
    void* body = nullptr;
    int originalMotion = -1;
    alignas(16) float originalPose[8] = {}, pose[8] = {};
    float originalVelocity[3] = {}, originalAngular[3] = {};
    float velocity[3] = {}, angular[3] = {};
    bool active = false;
};
bool RagdollWeaponPhysics_Release(RagdollWeaponPhysicsState& state, CEntityInstance* weapon,
    const float pose[8], const float velocity[3], const float angular[3], int user);
bool RagdollWeaponPhysics_Read(RagdollWeaponPhysicsState& state, float pose[8]);
void RagdollWeaponPhysics_Restore(RagdollWeaponPhysicsState& state);
// A demo pickup already owns the new pose; undo only our dynamic motion override.
void RagdollWeaponPhysics_Relinquish(RagdollWeaponPhysicsState& state);
void RagdollWeaponPhysics_Probe(RagdollWeaponPhysicsState& state, int user, const char* phase);
}
