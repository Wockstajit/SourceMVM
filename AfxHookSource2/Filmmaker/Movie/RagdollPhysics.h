#pragma once

// Ragdoll PHYSICS controls: gravity, friction, how many bodies persist, whether they collide
// with each other, and how long they survive before the engine recycles them.
//
// and from RagdollWeapon. Different capability, different lifetime,
// different failure modes -- so its own file and its own command, per the repo's
// "never couple a new thing into an existing thing" rule.
//
// Every knob here is a CONVAR, not a memory write. CS2 already exposes the whole surface
// (`find ragdoll`), so there is nothing to reverse and nothing version-coupled beyond the convar
// names themselves -- the most update-resilient shape available. The reference thread
// (reference/coding help/gravity-physics/652059-gravity-ragdolls.md) hooks the convar-value
// GETTER purely to hide the change from VAC; offline demo work has no such need, so we simply
// set the convars.
//
// All of them are cheat-flagged, so the module unlocks cheats once before its first write.

#include <sstream>
#include <string>

namespace advancedfx { class ICommandArgs; }

namespace Filmmaker {

enum RagdollPhysParam {
	kRpGravity = 0,   // ragdoll_gravity_scale   -- 1 = normal, <1 floaty, >1 heavy, <0 inverts
	kRpFriction,      // ragdoll_friction_scale  -- 0.6 default; low = slides, high = sticks
	kRpLimit,         // cl_ragdoll_limit        -- max corpses shown; -1 = unlimited
	kRpCollide,       // cl_player_ragdolls_collide -- bodies pile instead of interpenetrating
	kRpMinAge,        // ragdoll_lru_min_age     -- seconds before a body may be recycled
	kRpParamCount
};

// Main/UI thread, once per frame from Filmmaker::RunMainThreadFrame. Cheap no-op while nothing
// is overridden; re-asserts overridden convars periodically so a map change cannot silently
// revert the look mid-edit.
void RagdollPhysics_RunFrame();

void  RagdollPhysics_Set(RagdollPhysParam p, float value);
float RagdollPhysics_Get(RagdollPhysParam p);      // current value (override or default)
bool  RagdollPhysics_IsOverridden(RagdollPhysParam p);
float RagdollPhysics_Default(RagdollPhysParam p);

// Restores every overridden convar to its CS2 default and stops asserting.
void RagdollPhysics_ResetAll();

// `ragdoll_cleanup_all` -- removes every corpse currently in the world. An action, not a state.
void RagdollPhysics_CleanupAll();

void RagdollPhysics_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd);

// Appends this module's fields (leading comma included) to the Config panel state JSON, the
// same contract HandheldCamRef()/WeaponRecoilRef() use -- so adding a knob is a one-line change
// here rather than an edit inside ConfigHud.cpp.
void RagdollPhysics_AppendStateJson(std::ostringstream& o);

} // namespace Filmmaker
