#pragma once

// A corpse's fifteen vphysics2 ragdoll bodies: finding them, reading them and steering them.
// Shared by the features that move a dying player's body -- Death Animations (clip playback) and the
// Nervous System (post-death reactions) -- so there is one verified copy of the engine access.
//
// vphysics2 body vtable (installed build, verified by implementation prefix at every acquire, never
// by absolute address):
//   3 SetMotionType(int)   4 GetMotionType()      53 SetTransform(CTransform w/o scale)
//   56 GetTransform -> CTransform: position floats 0-2, scale float 3, quaternion xyzw 4-7
//   64 SetVelocity(vec3)   66 GetVelocity          68 SetAngularVelocity(vec3, degrees/s)
// Aggregate: body count at +0x38, 24 GetBody(int). Ragdoll control: 8 descriptor (count), 4 GetBody.

#include "DeathAnimationsMath.h"

#include <cstddef>
#include <string>

class CEntityInstance;
namespace SOURCESDK { namespace CS2 { class CGameEvent; } }

namespace Filmmaker {
struct JsonValue;

namespace RagdollBodies {

using DeathAnimationsMath::kBodies;
using DeathAnimationsMath::Pose;

// vphysics2 motion type 2 = dynamic. Bodies we steer stay dynamic: CS2 skips drawing a ragdoll whose
// bodies are keyframed (1) on most frames.
constexpr int kDynamic = 2;

// Where a corpse's bodies came from. `route` 0 = the carrier's model-state aggregate, 1 = the pawn's
// ragdoll control; `object` is that aggregate or control.
struct BodySet {
	CEntityInstance* carrier = nullptr;
	// The carrier's entity slot, read while it was alive: whether it still exists can then be checked
	// in the entity list without reading the carrier (DeathAnimations.cpp, CarrierAlive).
	int carrierIndex = -1;
	int route = 0;
	void* object = nullptr;
	void* bodies[kBodies] = {};
};

CEntityInstance* EntityAt(int index);
// The entity in any slot of the 15-bit handle range, client-only ones (a client-side ragdoll sits
// above EntityAt's networked ceiling) included. Null when the slot is empty.
CEntityInstance* EntityInSlot(int index);
CEntityInstance* PawnOfUser(int userid, int* pawnIndex);
CEntityInstance* ClientRagdollOf(CEntityInstance* entity);

// Finds the fifteen verified bodies on the pawn, its client-side ragdoll child, or its ragdoll control.
// `why` describes every route tried, for the log when none works.
bool Acquire(CEntityInstance* pawn, BodySet& out, char* why, size_t whySize);
// The carrier still resolves to the same object and bodies (they were not freed or rebuilt).
bool Current(const BodySet& set);

bool ReadBody(void* body, Pose& out, int& motion);
// Linear velocity (units/s) and angular velocity (world axes, degrees/s) the last physics step left.
bool ReadVelocity(void* body, float linear[3], float angularDeg[3]);
// Place a body on `pose`, moving with the given velocity. Returns false on a fault; `reasserted`
// reports a motion type that had to be set back to dynamic.
bool HoldBody(void* body, const Pose& pose, const float linear[3], const float angularDeg[3], bool& reasserted);
// Steer a body by velocity only, so the physics step still resolves floors, walls and joints.
bool DriveBody(void* body, const float linear[3], const float angularDeg[3], bool& reasserted);
bool ReleaseBody(void* body, const Pose& pose, const float linear[3], const float angularDeg[3]);
// Set the motion type to dynamic even when it already is (diagnostics: does it wake a resting body?).
bool ForceDynamic(void* body);
bool DampHorizontal(void* body, float factor);

bool SafeOrigin(CEntityInstance* e, float out[3]);
const char* SafeClassName(CEntityInstance* e);

// A game event's keys as JSON (tier0's KV3 writer).
bool EventJson(SOURCESDK::CS2::CGameEvent* event, JsonValue& out);
int JInt(const JsonValue& r, const char* k, int def = -1);
bool JBool(const JsonValue& r, const char* k);
std::string JStr(const JsonValue& r, const char* k);

} // namespace RagdollBodies
} // namespace Filmmaker
