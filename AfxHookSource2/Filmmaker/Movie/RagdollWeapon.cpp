#include "RagdollWeapon.h"
#include "RagdollWeaponMath.h"
#include "RagdollWeaponRates.h"
#include "RagdollWeaponArmPhysics.h"
#include "RagdollWeaponPhysics.h"

#include "../Debug/MvmDebugLog.h"
#include "../Platform/JsonParser.h"
#include "../../ClientEntitySystem.h"
#include "../../MirvTime.h"
#include "../../SchemaSystem.h"
#include "../../SceneSystem.h"
#include "../../../Shared/AfxConsole.h"
#include "../../../Deps/release/prop/AfxHookSource/SourceSdkShared.h"
#include "../../../Deps/release/prop/cs2/sdk_src/public/entityhandle.h"
#include "../../../Deps/release/prop/cs2/sdk_src/public/igameevents.h"
#include "../../../Deps/release/prop/cs2/sdk_src/public/tier1/utlstring.h"

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <atomic>

// Shared engine entry points resolved by MirvColors; feature policy remains here.
struct ParticleData;
extern unsigned char* (__fastcall *g_Original_getParticleManager)();
extern int* (__fastcall *g_Original_createParticle)(unsigned char*, int*, const char*, int,
	int64_t, int64_t, int64_t, int64_t);
extern void (__fastcall *g_Original_updateParticle)(unsigned char*, int, int, ParticleData*, int);
extern void (__fastcall *g_Original_dispatchGrenadeParticle)(const char*, int,
	CEntityInstance*, const char*, bool, int, void*, bool);

namespace Filmmaker {
namespace {

constexpr int kMaxDeaths = 64;
constexpr int kMaxCandidates = 32;
constexpr int kMaxSavedNodes = 64;
constexpr int kMaxUsers = 128;
constexpr double kPairRadius = 180.0;
constexpr double kResolveWindow = 3.0;
double g_fireChance = 100.0;
double g_fireDurationMs = 250.0;
double g_holdDurationMs = 250.0;

bool g_keep = false;
bool g_fire = false;
constexpr const char* kFlash = "particles/filmmaker/modern/arc9_fas_muzzleflashes/muzzleflash_ar.vpcf";
constexpr const char* kShell = "particles/filmmaker/modern/weapons/cs_weapon_fx/weapon_shell_casing_rifle.vpcf";
constexpr const char* kTracer = "particles/filmmaker/insurgency/weapon_fx_tracers/weapon_tracers_akm.vpcf";
constexpr const char* kPistolFlash = "particles/filmmaker/modern/arc9_fas_muzzleflashes/muzzleflash_pistol.vpcf";
constexpr const char* kPistolShell = "particles/filmmaker/modern/weapons/cs_weapon_fx/weapon_shell_casing_9mm.vpcf";
constexpr const char* kPistolTracer = "particles/filmmaker/insurgency/weapon_fx_tracers/weapon_tracers_pistol.vpcf";
bool g_effectsReady = false;

bool PrecacheFireEffects() {
	if (!g_pCResourceSystem) return false;
	__try {
		return g_pCResourceSystem->PreCache(kFlash) && g_pCResourceSystem->PreCache(kShell)
			&& g_pCResourceSystem->PreCache(kTracer) && g_pCResourceSystem->PreCache(kPistolFlash)
			&& g_pCResourceSystem->PreCache(kPistolShell) && g_pCResourceSystem->PreCache(kPistolTracer);
	} __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

struct SavedNode {
	unsigned char* node = nullptr;
	float transform[8] = {};
	float origin[3] = {}, rotation[3] = {};
	float absOrigin[3] = {};
	bool haveAbsOrigin = false;
	float wrappedOrigin[3] = {}, wrappedAngles[3] = {}, absAngles[3] = {};
};

struct DeathSlot {
	bool used = false;
	int userId = -1;
	uint32_t seed = 0;
	double deathTime = 0.0;
	double lastShotTime = -1000.0;
	double nextShotTime = 1000.0;
	unsigned shotNumber = 0;
	unsigned weaponItem=0;
	const RagdollWeaponFireProfile* fireProfile=nullptr;
	bool willFire = false;
	double holdSeconds = 1.0, fireSeconds = 0.5;
	double attachedTime = -1.0, fireStartTime = -1.0;
	bool released = false, physicsWarned = false;
	double nextPhysicsTry = 0.0;
	ULONGLONG dropProbeMs = 0;
	RagdollWeaponPhysicsState physics;
	float previousHeldPose[8] = {}, releaseVelocity[3] = {}, releaseAngular[3] = {};
	double previousHeldTime = -1.0;
	float deathOrigin[3] = {};
	char weaponClass[96] = {};
	CEntityInstance* weapon = nullptr;
	CEntityInstance* victimPawn = nullptr;
	CEntityInstance* ragdoll = nullptr;
	CEntityInstance* candidates[kMaxCandidates] = {};
	int candidateCount = 0;
	SavedNode saved[kMaxSavedNodes];
	int savedCount = 0;
	bool wroteTransform = false;
	float expected[8] = {};
	ULONGLONG probeMs = 0;
	ULONGLONG finalProbeMs = 0;
	bool savedMovement = false;
	unsigned char moveType = 0, actualMoveType = 0;
	bool savedNoInterpolate = false, noInterpolate = false;
	bool haveMuzzleLocal = false, haveShellLocal = false;
	float muzzleLocal[3] = {}, shellLocal[3] = {};
	float muzzleLocalQ[4] = {0,0,0,1};
	int armProbePasses = 0;
	float armProbeHand[3] = {};
	unsigned armProbeShot = 0;
	double armWatchUntil = -1.0;
	double armWatchStart = -1.0;
};

DeathSlot g_deaths[kMaxDeaths];

struct LastHeld {
	double sampleTime = -1000.0;
	CEntityInstance* pawn = nullptr;
	CEntityInstance* weapon = nullptr;
	float origin[3] = {};
	char weaponClass[96] = {};
	bool haveMuzzleLocal = false, haveShellLocal = false;
	float muzzleLocal[3] = {}, shellLocal[3] = {};
	float muzzleLocalQ[4] = {0,0,0,1};
};
LastHeld g_lastHeld[kMaxUsers];
const char* g_writePhase = "unknown";
struct DrawProbeTarget {
	unsigned char* node;
	float expected[8];
	int user;
};
std::mutex g_drawProbeMutex;
DrawProbeTarget g_drawProbeTargets[kMaxDeaths] = {};
int g_drawProbeCount = 0;

typedef bool (*SaveKV3AsJSON_t)(const SOURCESDK::CS2::KeyValues3*,
	SOURCESDK::CS2::CUtlString*, SOURCESDK::CS2::CUtlString*);

SaveKV3AsJSON_t GetSaveKV3AsJSON() {
	static bool tried = false;
	static SaveKV3AsJSON_t fn = nullptr;
	if (!tried) {
		tried = true;
		if (HMODULE tier0 = GetModuleHandleA("tier0.dll"))
			fn = (SaveKV3AsJSON_t)GetProcAddress(tier0,
				"?SaveKV3AsJSON@@YA_NPEBVKeyValues3@@PEAVCUtlString@@1@Z");
	}
	return fn;
}

bool DeathUserId(SOURCESDK::CS2::CGameEvent* event, int& out) {
	SaveKV3AsJSON_t fn = GetSaveKV3AsJSON();
	if (!event || !fn) return false;
	SOURCESDK::CS2::CUtlString error, text;
	if (!fn(event->GetDataKeys(), &error, &text) || !text.Get()) return false;
	JsonValue root;
	if (!JsonParse(text.Get(), root) || root.type != JsonValue::Type::Object) return false;
	const JsonValue* value = root.Find("userid");
	if (!value) return false;
	out = value->AsInt(-1);
	return out >= 0;
}

CEntityInstance* EntityAt(int index) {
	if (index < 0 || index > GetHighestEntityIndex() || !g_pEntityList || !*g_pEntityList || !g_GetEntityFromIndex)
		return nullptr;
	return (CEntityInstance*)g_GetEntityFromIndex(*g_pEntityList, index);
}

bool SafeOrigin(CEntityInstance* entity, float out[3]) {
	if (!entity || !out) return false;
	__try {
		entity->GetOrigin(out[0], out[1], out[2]);
		return std::isfinite(out[0]) && std::isfinite(out[1]) && std::isfinite(out[2]);
	} __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool SafeHealth(CEntityInstance* entity, unsigned& out) {
	if (!entity) return false;
	__try { out = entity->GetHealth(); return true; }
	__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

CEntityInstance* SafeActiveWeapon(CEntityInstance* pawn) {
	if (!pawn) return nullptr;
	__try {
		const auto handle = pawn->GetActiveWeaponHandle();
		return handle.IsValid() ? EntityAt(handle.GetEntryIndex()) : nullptr;
	} __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

bool SafeAttachment(CEntityInstance* entity, const char* name, float outPos[3], float outQ[4]) {
	if (!entity || !name) return false;
	SOURCESDK::Vector p;
	SOURCESDK::Quaternion q;
	bool ok = false;
	__try {
		const uint8_t index = entity->LookupAttachment(name);
		if (index) ok = entity->GetAttachment(index, p, q);
	} __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
	if (!ok) return false;
	outPos[0] = p.x; outPos[1] = p.y; outPos[2] = p.z;
	outQ[0] = q.x; outQ[1] = q.y; outQ[2] = q.z; outQ[3] = q.w;
	return true;
}

const char* SafeClass(CEntityInstance* entity) {
	__try { return entity ? entity->GetClassName() : nullptr; }
	__except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

bool IsGunClass(const char* cls) {
	if (!cls || !*cls) return false;
	if (!std::strstr(cls, "Weapon") && !std::strstr(cls, "weapon_")) return false;
	const char* excluded[] = { "Knife", "knife", "Bayonet", "bayonet", "Grenade", "grenade",
		"Molotov", "molotov", "Flashbang", "flashbang", "Decoy", "decoy", "C4", "Taser",
		"Healthshot" };
	for (const char* word : excluded) if (std::strstr(cls, word)) return false;
	return true;
}

unsigned SafeWeaponItem(CEntityInstance* weapon) {
	const auto& o=g_clientDllOffsets;
	if(!weapon || !o.C_EconEntity.m_AttributeManager || !o.C_AttributeContainer.m_Item || !o.C_EconItemView.m_iItemDefinitionIndex) return 0;
	__try {
		return *(unsigned short*)((unsigned char*)weapon+o.C_EconEntity.m_AttributeManager+
			o.C_AttributeContainer.m_Item+o.C_EconItemView.m_iItemDefinitionIndex);
	} __except(EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

// Probe only: a pointer to the control object does not establish its physics ABI,
// body ordering or whether force fields are consumed after the death frame.
void ProbeArmPhysics(const DeathSlot& slot) {
	if(!MvmDebugLog_Active() || !slot.ragdoll) return;
	const auto& o=g_clientDllOffsets.DeathWeaponRagdoll;
	if(!o.m_pRagdollControl || !o.m_vecForce || !o.m_nForceBone || !o.m_bRagdollEnabled) {
		MvmDebugLog_LinefAlways("ragdoll.weapon.arm","userid=%d unavailable controlOff=%td forceOff=%td boneOff=%td enabledOff=%td",
			slot.userId,o.m_pRagdollControl,o.m_vecForce,o.m_nForceBone,o.m_bRagdollEnabled);
		return;
	}
	__try {
		auto* base=(unsigned char*)slot.ragdoll;
		if(o.m_pClientsideRagdoll) {
			auto* child=*(CEntityInstance**)(base+o.m_pClientsideRagdoll);
			void* control=child?*(void**)((unsigned char*)child+o.m_pRagdollControl):nullptr;
			MvmDebugLog_LinefAlways("ragdoll.weapon.armchild","userid=%d pawn=%p childOff=%td child=%p class=%s control=%p poseOff=%td",
				slot.userId,slot.ragdoll,o.m_pClientsideRagdoll,child,child?SafeClass(child):"-",control,o.m_RagdollPose);
			if(control && slot.shotNumber==0) {
				void** vtable=*(void***)control;
				for(int i=0;i<32;++i) MvmDebugLog_LinefAlways("ragdoll.weapon.armvtable","control=%p table=%p slot=%d fn=%p",control,vtable,i,vtable[i]);
			}
		}
		const float* force=(const float*)(base+o.m_vecForce);
		MvmDebugLog_LinefAlways("ragdoll.weapon.arm","userid=%d corpse=%p controlOff=%td control=%p forceOff=%td force=(%.3f %.3f %.3f) boneOff=%td bone=%d enabledOff=%td enabled=%d impulseApplied=0",
			slot.userId,slot.ragdoll,o.m_pRagdollControl,*(void**)(base+o.m_pRagdollControl),o.m_vecForce,
			force[0],force[1],force[2],o.m_nForceBone,*(int*)(base+o.m_nForceBone),o.m_bRagdollEnabled,*(bool*)(base+o.m_bRagdollEnabled));
	} __except(EXCEPTION_EXECUTE_HANDLER) {
		MvmDebugLog_LinefAlways("ragdoll.weapon.arm","userid=%d read failed",slot.userId);
	}
}

bool IsPistolClass(const char* cls) {
	if (!cls) return false;
	const char* names[]={"glock","hkp2000","usp","p250","elite","fiveseven","tec9","cz75","deagle","revolver"};
	for(const char* name:names) if(std::strstr(cls,name)) return true;
	return false;
}

double DistanceSq(const float a[3], const float b[3]) {
	const double x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2];
	return x*x + y*y + z*z;
}

unsigned char* SceneNode(CEntityInstance* entity) {
	const ptrdiff_t off = g_clientDllOffsets.C_BaseEntity.m_pGameSceneNode;
	if (!entity || !off) return nullptr;
	__try {
		unsigned char* node = *(unsigned char**)((unsigned char*)entity + off);
		return (uintptr_t)node > 0x10000 ? node : nullptr;
	} __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

void ProbeWeaponTree(CEntityInstance* weapon, int user, const char* phase) {
	if (!MvmDebugLog_Active()) return;
	const auto& o = g_clientDllOffsets;
	auto* root = SceneNode(weapon);
	if (!root || !o.CGameSceneNode.m_nodeToWorld || !o.CGameSceneNode.m_pOwner) return;
	__try {
		unsigned char* stack[kMaxSavedNodes] = {}; int top=0, count=0;
		stack[top++]=root;
		while (top && count++ < kMaxSavedNodes) {
			auto* node=stack[--top];
			auto* owner=*(CEntityInstance**)(node+o.CGameSceneNode.m_pOwner);
			const float* x=(const float*)(node+o.CGameSceneNode.m_nodeToWorld);
			unsigned char* state=nullptr; const char* model="-"; void* aggregate=nullptr; int motion=-1;
			if (owner && o.ModelChain.m_CBodyComponent && o.ModelChain.m_skeletonInstance && o.ModelChain.m_modelState) {
				auto* body=*(unsigned char**)((unsigned char*)owner+o.ModelChain.m_CBodyComponent);
				if (body) {
					state=body+o.ModelChain.m_skeletonInstance+o.ModelChain.m_modelState;
					if (o.ModelChain.m_ModelName) model=*(const char**)(state+o.ModelChain.m_ModelName);
					if (o.DeathWeaponPhysics.m_pVPhysicsAggregate) aggregate=*(void**)(state+o.DeathWeaponPhysics.m_pVPhysicsAggregate);
					if (o.DeathWeaponPhysics.m_nIdealMotionType) motion=*(unsigned char*)(state+o.DeathWeaponPhysics.m_nIdealMotionType);
				}
			}
			MvmDebugLog_LinefAlways("ragdoll.weapon.tree",
				"phase=%s user=%d weapon=%p root=%p node=%p parent=%p owner=%p class=%s model=%s worldOff=%td pos=(%.4f %.4f %.4f) q=(%.6f %.6f %.6f %.6f) state=%p aggregateOff=%td aggregate=%p motionOff=%td motion=%d",
				phase,user,weapon,root,node,*(void**)(node+o.CGameSceneNode.m_pParent),owner,SafeClass(owner),model?model:"-",o.CGameSceneNode.m_nodeToWorld,
					x[0],x[1],x[2],x[4],x[5],x[6],x[7],state,o.DeathWeaponPhysics.m_pVPhysicsAggregate,aggregate,o.DeathWeaponPhysics.m_nIdealMotionType,motion);
			if (o.DeathWeaponPhysics.m_nParentAttachmentOrBone && o.DeathWeaponPhysics.m_nHierarchyType) {
				const auto* local=(const float*)(node+o.CGameSceneNode.m_vecWrappedLocalOrigin);
				const auto* angles=(const float*)(node+o.CGameSceneNode.m_angWrappedLocalRotation);
				MvmDebugLog_LinefAlways("ragdoll.weapon.hierarchy",
					"phase=%s node=%p attachmentOff=%td attachment=%d typeOff=%td type=%u localOff=%td local=(%.6f %.6f %.6f) angles=(%.6f %.6f %.6f)",
					phase,node,o.DeathWeaponPhysics.m_nParentAttachmentOrBone,*(short*)(node+o.DeathWeaponPhysics.m_nParentAttachmentOrBone),
					o.DeathWeaponPhysics.m_nHierarchyType,*(unsigned char*)(node+o.DeathWeaponPhysics.m_nHierarchyType),o.CGameSceneNode.m_vecWrappedLocalOrigin,
					local[0],local[1],local[2],angles[0],angles[1],angles[2]);
			}
			if (aggregate && 0==std::strcmp(phase,"capture")) {
				void** vt=*(void***)aggregate;
				for (int i=0; i<16; ++i) MvmDebugLog_LinefAlways("ragdoll.weapon.aggregate","ptr=%p slot=%d fn=%p",aggregate,i,vt[i]);
			}
			for (auto* child=*(unsigned char**)(node+o.CGameSceneNode.m_pChild); child && top<kMaxSavedNodes; child=*(unsigned char**)(child+o.CGameSceneNode.m_pNextSibling)) stack[top++]=child;
		}
	} __except(EXCEPTION_EXECUTE_HANDLER) { MvmDebugLog_LinefAlways("ragdoll.weapon.tree","phase=%s user=%d readFault=1",phase,user); }
}

RagdollWeaponQuat Mul(const RagdollWeaponQuat& a, const RagdollWeaponQuat& b) {
	return RagdollWeaponQuatMul(a, b);
}

RagdollWeaponQuat Conjugate(const RagdollWeaponQuat& q) { return {-q.x,-q.y,-q.z,q.w}; }

void Rotate(const RagdollWeaponQuat& q, const double in[3], double out[3]) {
	const RagdollWeaponQuat v{in[0], in[1], in[2], 0.0};
	const RagdollWeaponQuat r = Mul(Mul(q, v), Conjugate(q));
	out[0] = r.x; out[1] = r.y; out[2] = r.z;
}

bool SnapshotSubtree(DeathSlot& slot, unsigned char* root) {
	ProbeWeaponTree(slot.weapon, slot.userId, "capture");
	const auto& o = g_clientDllOffsets.CGameSceneNode;
	if (!root || !o.m_nodeToWorld || !o.m_pChild || !o.m_pNextSibling
		|| !o.m_vecOrigin || !o.m_angRotation || !o.m_vecWrappedLocalOrigin
		|| !o.m_angWrappedLocalRotation || !o.m_angAbsRotation || !o.m_pParent) return false;
	slot.savedCount = 0;
	MvmDebugLog_Linef("ragdoll.weapon.sources", "weapon=%p root=%p worldOff=%td wrappedOriginOff=%td wrappedAnglesOff=%td absAnglesOff=%td parentOff=%td",
		slot.weapon,root,o.m_nodeToWorld,o.m_vecWrappedLocalOrigin,o.m_angWrappedLocalRotation,o.m_angAbsRotation,o.m_pParent);
	unsigned char* stack[kMaxSavedNodes] = {};
	int top = 0;
	stack[top++] = root;
	__try {
		while (top && slot.savedCount < kMaxSavedNodes) {
			unsigned char* node = stack[--top];
			SavedNode& saved = slot.saved[slot.savedCount++];
			saved.node = node;
			std::memcpy(saved.transform, node + o.m_nodeToWorld, sizeof(saved.transform));
			std::memcpy(saved.origin, node + o.m_vecOrigin, sizeof(saved.origin));
			std::memcpy(saved.rotation, node + o.m_angRotation, sizeof(saved.rotation));
			std::memcpy(saved.wrappedOrigin, node + o.m_vecWrappedLocalOrigin, sizeof(saved.wrappedOrigin));
			std::memcpy(saved.wrappedAngles, node + o.m_angWrappedLocalRotation, sizeof(saved.wrappedAngles));
			std::memcpy(saved.absAngles, node + o.m_angAbsRotation, sizeof(saved.absAngles));
			if (o.m_vecAbsOrigin) {
				std::memcpy(saved.absOrigin, node + o.m_vecAbsOrigin, sizeof(saved.absOrigin));
				saved.haveAbsOrigin = true;
			}
			for (unsigned char* child = *(unsigned char**)(node + o.m_pChild);
				child && top < kMaxSavedNodes; child = *(unsigned char**)(child + o.m_pNextSibling))
				stack[top++] = child;
		}
		return slot.savedCount > 0;
	} __except (EXCEPTION_EXECUTE_HANDLER) { slot.savedCount = 0; return false; }
}

void Restore(DeathSlot& slot) {
	RagdollWeaponPhysics_Restore(slot.physics);
	const auto& o = g_clientDllOffsets.CGameSceneNode;
	__try {
		if (slot.weapon && slot.savedMovement) {
			*(unsigned char*)((unsigned char*)slot.weapon + g_clientDllOffsets.C_BaseEntity.m_MoveType) = slot.moveType;
			*(unsigned char*)((unsigned char*)slot.weapon + g_clientDllOffsets.C_BaseEntity.m_nActualMoveType) = slot.actualMoveType;
		}
		if (slot.weapon && slot.savedNoInterpolate && g_clientDllOffsets.C_BaseModelEntity.m_bNoInterpolate)
			*(bool*)((unsigned char*)slot.weapon + g_clientDllOffsets.C_BaseModelEntity.m_bNoInterpolate) = slot.noInterpolate;
		for (int i = 0; i < slot.savedCount; ++i) {
			SavedNode& saved = slot.saved[i];
			if (!saved.node) continue;
			std::memcpy(saved.node + o.m_nodeToWorld, saved.transform, sizeof(saved.transform));
			std::memcpy(saved.node + o.m_vecOrigin, saved.origin, sizeof(saved.origin));
			std::memcpy(saved.node + o.m_angRotation, saved.rotation, sizeof(saved.rotation));
			std::memcpy(saved.node + o.m_vecWrappedLocalOrigin, saved.wrappedOrigin, sizeof(saved.wrappedOrigin));
			std::memcpy(saved.node + o.m_angWrappedLocalRotation, saved.wrappedAngles, sizeof(saved.wrappedAngles));
			std::memcpy(saved.node + o.m_angAbsRotation, saved.absAngles, sizeof(saved.absAngles));
			if (saved.haveAbsOrigin && o.m_vecAbsOrigin)
				std::memcpy(saved.node + o.m_vecAbsOrigin, saved.absOrigin, sizeof(saved.absOrigin));
		}
	} __except (EXCEPTION_EXECUTE_HANDLER) {}
	slot.savedCount = 0;
	slot.wroteTransform = false;
	slot.savedMovement = false;
	slot.savedNoInterpolate = false;
}

void SuspendMovement(DeathSlot& slot) {
	const auto& o = g_clientDllOffsets.C_BaseEntity;
	if (!slot.weapon || !o.m_MoveType || !o.m_nActualMoveType) return;
	__try {
		auto* move = (unsigned char*)slot.weapon + o.m_MoveType;
		auto* actual = (unsigned char*)slot.weapon + o.m_nActualMoveType;
		if (!slot.savedMovement) {
			slot.moveType = *move; slot.actualMoveType = *actual;
			slot.savedMovement = true;
			MvmDebugLog_LinefAlways("ragdoll.weapon.physics", "weapon=%p moveOff=%td actualOff=%td before=%u/%u write=0/0",
				slot.weapon, o.m_MoveType, o.m_nActualMoveType, *move, *actual);
		}
		if (g_clientDllOffsets.C_BaseModelEntity.m_bNoInterpolate) {
			auto* noInterpolate=(bool*)((unsigned char*)slot.weapon + g_clientDllOffsets.C_BaseModelEntity.m_bNoInterpolate);
			if(!slot.savedNoInterpolate) {
				slot.noInterpolate=*noInterpolate;
				slot.savedNoInterpolate=true;
				MvmDebugLog_LinefAlways("ragdoll.weapon.physics", "weapon=%p noInterpolateOff=%td before=%d write=1",
					slot.weapon,g_clientDllOffsets.C_BaseModelEntity.m_bNoInterpolate,*noInterpolate?1:0);
			}
			*noInterpolate=true;
		}
		// MOVETYPE_NONE, as defined by the bundled CS2 public/const.h.
		*move = 0; *actual = 0;
	} __except (EXCEPTION_EXECUTE_HANDLER) {}
}

bool ReleaseIfPickedUp(DeathSlot& slot) {
	if (!slot.weapon || !g_clientDllOffsets.C_BaseEntity.m_hOwnerEntity) return false;
	__try {
		const auto ownerHandle = slot.weapon->GetOwnerEntityHandle();
		CEntityInstance* owner = ownerHandle.IsValid() ? EntityAt(ownerHandle.GetEntryIndex()) : nullptr;
		if (!owner || !owner->IsPlayerPawn() || owner->GetHealth() == 0) return false;
		RagdollWeaponPhysics_Relinquish(slot.physics);
		// The demo has reassigned this entity to a living player. Restoring the corpse's
		// saved transform here would clobber the new owner's current pose as well.
		// Only undo movement fields that still contain our override, then relinquish it.
		if (slot.savedMovement) {
			auto* move=(unsigned char*)slot.weapon + g_clientDllOffsets.C_BaseEntity.m_MoveType;
			auto* actual=(unsigned char*)slot.weapon + g_clientDllOffsets.C_BaseEntity.m_nActualMoveType;
			if (*move == 0) *move=slot.moveType;
			if (*actual == 0) *actual=slot.actualMoveType;
		}
		if(slot.savedNoInterpolate && g_clientDllOffsets.C_BaseModelEntity.m_bNoInterpolate)
			*(bool*)((unsigned char*)slot.weapon + g_clientDllOffsets.C_BaseModelEntity.m_bNoInterpolate)=slot.noInterpolate;
		MvmDebugLog_LinefAlways("ragdoll.weapon.release", "userid=%d weapon=%p owner=%p ownerHandle=%u ownerOff=%td health=%u reason=pickup",
			slot.userId,slot.weapon,owner,ownerHandle.ToInt(),g_clientDllOffsets.C_BaseEntity.m_hOwnerEntity,owner->GetHealth());
		slot = DeathSlot{};
		return true;
	} __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool ApplyToHand(DeathSlot& slot, const float handPos[3], const float handQ[4], double kick) {
	unsigned char* root = SceneNode(slot.weapon);
	const auto& o = g_clientDllOffsets.CGameSceneNode;
	if (!root || !o.m_nodeToWorld || !o.m_pChild || !o.m_pNextSibling) return false;
	if (!slot.wroteTransform && !SnapshotSubtree(slot, root)) return false;

	float current[8] = {};
	__try { std::memcpy(current, root + o.m_nodeToWorld, sizeof(current)); }
	__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
	const float* base = slot.saved[0].transform;
	RagdollWeaponQuat from{base[4], base[5], base[6], base[7]};
	if (!RagdollWeaponNormalize(from)) return false;
	if (MvmDebugLog_Active() && GetTickCount64() - slot.probeMs >= 100) {
		slot.probeMs = GetTickCount64();
		const float* directOrigin=o.m_vecOrigin?(const float*)(root+o.m_vecOrigin):nullptr;
		const float* directAngles=o.m_angRotation?(const float*)(root+o.m_angRotation):nullptr;
		MvmDebugLog_LinefAlways("ragdoll.weapon.pose",
			"phase=%s userid=%d weapon=%p node=%p xformOff=%td originOff=%td before=(%.3f %.3f %.3f) q=(%.6f %.6f %.6f %.6f) hand=(%.3f %.3f %.3f) handQ=(%.6f %.6f %.6f %.6f) directOriginOff=%td direct=(%.3f %.3f %.3f) directAnglesOff=%td angles=(%.3f %.3f %.3f)",
			g_writePhase,
			slot.userId, slot.weapon, root, o.m_nodeToWorld, o.m_vecAbsOrigin,
			current[0], current[1], current[2], current[4], current[5], current[6], current[7],
			handPos[0], handPos[1], handPos[2], handQ[0], handQ[1], handQ[2], handQ[3],
			o.m_vecOrigin,directOrigin?directOrigin[0]:0,directOrigin?directOrigin[1]:0,directOrigin?directOrigin[2]:0,
			o.m_angRotation,directAngles?directAngles[0]:0,directAngles?directAngles[1]:0,directAngles?directAngles[2]:0);
	}
	RagdollWeaponQuat target{handQ[0], handQ[1], handQ[2], handQ[3]};
	if (!RagdollWeaponNormalize(target)) return false;
	for (int i = 0; i < 3; ++i) if (!std::isfinite(handPos[i])) return false;
	if (kick > 0.0) target = Mul(target, RagdollWeaponPitchQuat(kick));
	const RagdollWeaponQuat delta = Mul(target, Conjugate(from));
	const double rootPos[3] = {base[0], base[1], base[2]};

	__try {
		unsigned char* stack[kMaxSavedNodes] = {};
		int top = 0;
		stack[top++] = root;
		while (top) {
			unsigned char* node = stack[--top];
			// Bone/attachment/bonemerge children are evaluated by CS2 from their
			// original attachment-local pose. Reinterpreting those values relative to
			// the root displaces charms and StatTrak modules (hierarchy types 4/2),
			// and a death-frame world snapshot can still contain the living pose.
			if(node!=root && g_clientDllOffsets.DeathWeaponPhysics.m_nHierarchyType
				&& *(unsigned char*)(node+g_clientDllOffsets.DeathWeaponPhysics.m_nHierarchyType)!=0) continue;
			const SavedNode* reference = nullptr;
			for (int i = 0; i < slot.savedCount; ++i)
				if (slot.saved[i].node == node) { reference = &slot.saved[i]; break; }
			if (!reference) continue;
			const float* original = reference->transform;
			float* x = (float*)(node + o.m_nodeToWorld);
			const double rel[3] = {original[0]-rootPos[0], original[1]-rootPos[1], original[2]-rootPos[2]};
			RagdollWeaponQuat nodeQ{original[4],original[5],original[6],original[7]};
			if (!RagdollWeaponNormalize(nodeQ)) continue;
			double rotated[3]; Rotate(delta, rel, rotated);
			x[0] = (float)(handPos[0] + rotated[0]);
			x[1] = (float)(handPos[1] + rotated[1]);
			x[2] = (float)(handPos[2] + rotated[2]);
			RagdollWeaponQuat outQ = node == root ? target : Mul(delta, nodeQ);
			RagdollWeaponNormalize(outQ);
			x[4]=(float)outQ.x; x[5]=(float)outQ.y; x[6]=(float)outQ.z; x[7]=(float)outQ.w;
			// A hardware write watchpoint identified CGameSceneNode's lazy world-transform
			// rebuild copying these interpolated sources over m_nodeToWorld between our
			// post-frame write and scene drawing. Updating only the derived transform lets
			// the recorded dropped pose come back. Keep its sources coherent as well.
			float worldAngles[3]; RagdollWeaponQuatToAngles(outQ, worldAngles);
			if(node==root) {
				// m_vecOrigin/m_angRotation are the network interpolation inputs. Leaving
				// the recorded dropped pose here lets CS2 rebuild the weapon at that pose
				// between our frame phases, which is the visible death flicker.
				std::memcpy(node+o.m_vecOrigin,x,3*sizeof(float));
				std::memcpy(node+o.m_angRotation,worldAngles,sizeof(worldAngles));
			}
			std::memcpy(node + o.m_angAbsRotation, worldAngles, sizeof(worldAngles));
			unsigned char* parent = *(unsigned char**)(node + o.m_pParent);
			if (!parent) {
				std::memcpy(node + o.m_vecWrappedLocalOrigin, x, 3*sizeof(float));
				std::memcpy(node + o.m_angWrappedLocalRotation, worldAngles, sizeof(worldAngles));
			} else {
				const float* px = (float*)(parent + o.m_nodeToWorld);
				RagdollWeaponQuat parentQ{px[4],px[5],px[6],px[7]};
				if (RagdollWeaponNormalize(parentQ) && std::isfinite(px[3]) && std::abs(px[3]) > 1e-6) {
					const double offset[3] = {x[0]-px[0],x[1]-px[1],x[2]-px[2]};
					double local[3]; Rotate(Conjugate(parentQ), offset, local);
					float* localOrigin = (float*)(node + o.m_vecWrappedLocalOrigin);
					for (int j=0;j<3;++j) localOrigin[j]=(float)(local[j]/px[3]);
					const auto localQ = Mul(Conjugate(parentQ),outQ);
					float localAngles[3]; RagdollWeaponQuatToAngles(localQ,localAngles);
					std::memcpy(node + o.m_angWrappedLocalRotation,localAngles,sizeof(localAngles));
				}
			}
			if (o.m_vecAbsOrigin) {
				float* abs = (float*)(node + o.m_vecAbsOrigin);
				abs[0]=x[0]; abs[1]=x[1]; abs[2]=x[2];
			}
			for (unsigned char* child = *(unsigned char**)(node + o.m_pChild);
				child && top < kMaxSavedNodes; child = *(unsigned char**)(child + o.m_pNextSibling))
				stack[top++] = child;
		}
	} __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
	slot.wroteTransform = true;
	// Preserve the intended pose independently of the engine memory being tested.
	std::memcpy(slot.expected,handPos,3*sizeof(float)); slot.expected[3]=base[3];
	slot.expected[4]=(float)target.x; slot.expected[5]=(float)target.y;
	slot.expected[6]=(float)target.z; slot.expected[7]=(float)target.w;
	if(!slot.released && 0==std::strcmp(g_writePhase,"post")) {
		const double now=g_MirvTime.curtime_get();
		if(now>slot.previousHeldTime+0.001) {
			RagdollWeaponReleaseVelocity(slot.previousHeldPose,slot.expected,now-slot.previousHeldTime,slot.releaseVelocity,slot.releaseAngular);
			std::memcpy(slot.previousHeldPose,slot.expected,sizeof(slot.previousHeldPose));
			slot.previousHeldTime=now;
		}
	}
	if (MvmDebugLog_Active() && 0 == std::strcmp(g_writePhase, "post")
		&& GetTickCount64() - slot.finalProbeMs >= 100) {
		slot.finalProbeMs = GetTickCount64();
		float final[8] = {};
		__try { std::memcpy(final, root + o.m_nodeToWorld, sizeof(final)); }
		__except (EXCEPTION_EXECUTE_HANDLER) { return true; }
		MvmDebugLog_LinefAlways("ragdoll.weapon.final",
			"phase=post userid=%d weapon=%p final=(%.3f %.3f %.3f) q=(%.6f %.6f %.6f %.6f) hand=(%.3f %.3f %.3f) handQ=(%.6f %.6f %.6f %.6f)",
			slot.userId, slot.weapon, final[0], final[1], final[2], final[4], final[5], final[6], final[7],
			handPos[0], handPos[1], handPos[2], handQ[0], handQ[1], handQ[2], handQ[3]);
	}
	return true;
}

// The engine advances the native body. Mirror its pose into the scene sources so
// the demo's network interpolation cannot restore the recorded drop trajectory.
bool ApplyReleased(DeathSlot& slot, double now) {
	if (!slot.released) return false;
	alignas(16) float pose[8];
	if(!RagdollWeaponPhysics_Read(slot.physics,pose)) {
		if(!slot.physicsWarned) MvmDebugLog_LinefAlways("ragdoll.weapon.native","phase=lost userid=%d weapon=%p",slot.userId,slot.weapon);
		slot.physicsWarned=true;
		return true;
	}
	if(MvmDebugLog_Active() && GetTickCount64()-slot.dropProbeMs>=100) {
		slot.dropProbeMs=GetTickCount64();
		RagdollWeaponPhysics_Probe(slot.physics,slot.userId,g_writePhase);
		ProbeWeaponTree(slot.weapon,slot.userId,"released");
	}
	ApplyToHand(slot,pose,pose+4,0);
	return true;
}

void BeginRelease(DeathSlot& slot, double now) {
	if(slot.released || !slot.wroteTransform || slot.attachedTime<0 || now-slot.attachedTime<slot.holdSeconds) return;
	if(now<slot.nextPhysicsTry) return;
	slot.nextPhysicsTry=now+0.5;
	if(!RagdollWeaponPhysics_Release(slot.physics,slot.weapon,slot.expected,slot.releaseVelocity,slot.releaseAngular,slot.userId)) {
		if(!slot.physicsWarned) {
			slot.physicsWarned=true;
			MvmDebugLog_LinefAlways("ragdoll.weapon.drop","userid=%d waiting for verified native physics body",slot.userId);
		}
		return;
	}
	slot.released=true; slot.physicsWarned=false;
	MvmDebugLog_LinefAlways("ragdoll.weapon.release","userid=%d weapon=%p reason=hold_expired heldMs=%.3f requestedMs=%.3f origin=(%.3f %.3f %.3f) physics=native",
		slot.userId,slot.weapon,(now-slot.attachedTime)*1000,slot.holdSeconds*1000,
		slot.expected[0],slot.expected[1],slot.expected[2]);
}

// Capturing these offsets while the weapon is still equipped avoids CS2's stale dropped-weapon
// attachment cache during the first part of a death animation.
void CaptureAttachmentLocal(CEntityInstance* weapon, const char* primary, const char* fallback,
	bool& have, float local[3], float* localQ) {
	have = false;
	unsigned char* node = SceneNode(weapon);
	const ptrdiff_t off = g_clientDllOffsets.CGameSceneNode.m_nodeToWorld;
	if (!node || !off) return;
	float root[8] = {}, pos[3] = {}, qf[4] = {};
	// GetAttachment may lazily rebuild the root. Read both transforms from that same
	// updated pose, otherwise movement between the reads pollutes the local offset.
	if (!SafeAttachment(weapon, primary, pos, qf) && !SafeAttachment(weapon, fallback, pos, qf)) return;
	__try { std::memcpy(root, node + off, sizeof(root)); }
	__except (EXCEPTION_EXECUTE_HANDLER) { return; }
	RagdollWeaponQuat rootQ{root[4],root[5],root[6],root[7]};
	RagdollWeaponQuat attachQ{qf[0],qf[1],qf[2],qf[3]};
	if (!RagdollWeaponNormalize(rootQ) || !RagdollWeaponNormalize(attachQ)) return;
	const double delta[3] = {pos[0]-root[0], pos[1]-root[1], pos[2]-root[2]};
	double rotated[3]; Rotate(Conjugate(rootQ), delta, rotated);
	for (int i=0;i<3;++i) local[i]=(float)rotated[i];
	if (localQ) {
		RagdollWeaponQuat rel=Mul(Conjugate(rootQ),attachQ);
		if (!RagdollWeaponNormalize(rel)) return;
		localQ[0]=(float)rel.x; localQ[1]=(float)rel.y; localQ[2]=(float)rel.z; localQ[3]=(float)rel.w;
	}
	have = true;
}

// CP1 is a visual endpoint only, never a hitscan or damage operation.
bool DispatchFireEffects(const DeathSlot& slot, const float handPos[3], const float handQ[4], double kick) {
	if (!g_effectsReady || !g_Original_getParticleManager
		|| !g_Original_createParticle || !g_Original_updateParticle) return false;
	float liveMuzzle[3]={},liveQ[4]={},liveShell[3]={},shellQ[4]={};
	const char* muzzleAttachment="muzzle_flash";
	bool liveOk=SafeAttachment(slot.weapon,muzzleAttachment,liveMuzzle,liveQ);
	if (!liveOk) { muzzleAttachment="muzzle"; liveOk=SafeAttachment(slot.weapon,muzzleAttachment,liveMuzzle,liveQ); }
	const char* shellAttachment="shell_eject";
	bool shellOk=SafeAttachment(slot.weapon,shellAttachment,liveShell,shellQ);
	if (!shellOk) { shellAttachment="shell"; shellOk=SafeAttachment(slot.weapon,shellAttachment,liveShell,shellQ); }
	if (!liveOk && !slot.haveMuzzleLocal) return false;
	RagdollWeaponQuat q{handQ[0], handQ[1], handQ[2], handQ[3]};
	if (!RagdollWeaponNormalize(q)) return false;
	if (kick > 0.0) q=Mul(q,RagdollWeaponPitchQuat(kick));
	RagdollWeaponQuat muzzleRel{slot.muzzleLocalQ[0],slot.muzzleLocalQ[1],slot.muzzleLocalQ[2],slot.muzzleLocalQ[3]};
	if (!RagdollWeaponNormalize(muzzleRel)) return false;
	RagdollWeaponQuat muzzleQ=Mul(q,muzzleRel);
	double muzzleOffset[3], shellOffset[3];
	const double muzzleLocal[3]={slot.muzzleLocal[0],slot.muzzleLocal[1],slot.muzzleLocal[2]};
	Rotate(q,muzzleLocal,muzzleOffset);
	float muzzle[3]={(float)(handPos[0]+muzzleOffset[0]),(float)(handPos[1]+muzzleOffset[1]),(float)(handPos[2]+muzzleOffset[2])};
	float shell[3]={muzzle[0],muzzle[1],muzzle[2]};
	if(slot.haveShellLocal){const double local[3]={slot.shellLocal[0],slot.shellLocal[1],slot.shellLocal[2]}; Rotate(q,local,shellOffset); for(int i=0;i<3;++i)shell[i]=(float)(handPos[i]+shellOffset[i]);}
	// Equipped bone-merge offsets differ from the dropped model's own attachments.
	// Once its source pose is coherent, use that model's live muzzle for both the
	// native flash attachment and the tracer endpoint. Cached offsets are fallback only.
	if (liveOk) {
		std::memcpy(muzzle,liveMuzzle,sizeof(muzzle));
		muzzleQ={liveQ[0],liveQ[1],liveQ[2],liveQ[3]};
		if (!RagdollWeaponNormalize(muzzleQ)) return false;
	}
	if (shellOk) std::memcpy(shell,liveShell,sizeof(shell));
	const double forward[3] = {512,0,0};
	double direction[3]; Rotate(muzzleQ, forward, direction);
	float end[3] = {(float)(muzzle[0]+direction[0]), (float)(muzzle[1]+direction[1]), (float)(muzzle[2]+direction[2])};
	int flash=-1, casing=-1, tracer=-1;
	bool nativeFlash=false,nativeShell=false;
	const bool pistol=IsPistolClass(slot.fireProfile?slot.fireProfile->name:slot.weaponClass);
	const char* flashName=pistol?kPistolFlash:kFlash;
	const char* shellName=pistol?kPistolShell:kShell;
	const char* tracerName=pistol?kPistolTracer:kTracer;
	__try {
		unsigned char* manager = g_Original_getParticleManager();
		if (!manager) return false;
		if (liveOk && g_Original_dispatchGrenadeParticle) {
			// The existing native dispatcher resolves position AND orientation and keeps
			// the flash on the muzzle as the corpse moves (PATTACH_POINT_FOLLOW).
			g_Original_dispatchGrenadeParticle(flashName,5,slot.weapon,muzzleAttachment,false,-1,nullptr,false);
			nativeFlash=true;
		} else {
			g_Original_createParticle(manager,&flash,flashName,8,0,0,0,0);
			if(flash>=0){g_Original_updateParticle(manager,flash,0,reinterpret_cast<ParticleData*>(muzzle),0); g_Original_updateParticle(manager,flash,1,reinterpret_cast<ParticleData*>(end),0);}
		}
		if (shellOk && g_Original_dispatchGrenadeParticle) {
			g_Original_dispatchGrenadeParticle(shellName,5,slot.weapon,shellAttachment,false,-1,nullptr,false);
			nativeShell=true;
		} else {
			g_Original_createParticle(manager,&casing,shellName,8,0,0,0,0);
			if(casing>=0){g_Original_updateParticle(manager,casing,0,reinterpret_cast<ParticleData*>(shell),0); g_Original_updateParticle(manager,casing,1,reinterpret_cast<ParticleData*>(end),0);}
		}
		g_Original_createParticle(manager, &tracer, tracerName, 8, 0, 0, 0, 0);
		if (tracer >= 0) {
			g_Original_updateParticle(manager, tracer, 0, reinterpret_cast<ParticleData*>(muzzle), 0);
			g_Original_updateParticle(manager, tracer, 1, reinterpret_cast<ParticleData*>(end), 0);
		}
	} __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
	MvmDebugLog_LinefAlways("ragdoll.weapon.effects", "weapon=%p pistol=%d liveMuzzle=%d liveShell=%d nativeFlash=%d nativeShell=%d flash=%d shell=%d tracer=%d pos=(%.3f %.3f %.3f) live=(%.3f %.3f %.3f)",
		slot.weapon,pistol?1:0,liveOk,shellOk,nativeFlash,nativeShell,flash,casing,tracer,muzzle[0],muzzle[1],muzzle[2],liveMuzzle[0],liveMuzzle[1],liveMuzzle[2]);
	return (nativeFlash || flash>=0) && (nativeShell || casing>=0) && tracer>=0;
}

void ResolveRagdoll(DeathSlot& slot) {
	if (slot.ragdoll) return;
	// CS2 demo playback normally turns the existing player pawn into the corpse instead of
	// spawning a class whose name contains "ragdoll". Prefer that pawn whenever its skeleton
	// exposes the hand attachment. The client-only candidate path remains for modes/builds that
	// do create a separate corpse entity.
	float pawnPos[3], pawnQ[4];
	if (slot.victimPawn && SafeAttachment(slot.victimPawn, "weapon_hand_R", pawnPos, pawnQ)) {
		slot.ragdoll = slot.victimPawn;
		MvmDebugLog_LinefAlways("ragdoll.weapon", "paired userid=%d corpse=pawn ptr=%p",
			slot.userId, slot.ragdoll);
		return;
	}
	double best = kPairRadius * kPairRadius;
	for (int i = 0; i < slot.candidateCount; ++i) {
		CEntityInstance* candidate = slot.candidates[i];
		float pos[3], q[4];
		if (!SafeAttachment(candidate, "weapon_hand_R", pos, q)) continue;
		const double d = DistanceSq(pos, slot.deathOrigin);
		if (d < best) { best = d; slot.ragdoll = candidate; }
	}
	if (slot.ragdoll)
		MvmDebugLog_LinefAlways("ragdoll.weapon", "paired userid=%d ragdoll=%p", slot.userId, slot.ragdoll);
}

void ResolveWeapon(DeathSlot& slot, const float handPos[3]) {
	if (slot.weapon) return;
	double best = kPairRadius * kPairRadius;
	for (int i = 0; i <= GetHighestEntityIndex(); ++i) {
		CEntityInstance* entity = EntityAt(i);
		const char* cls = SafeClass(entity);
		if (!entity || !cls || 0 != std::strcmp(cls, slot.weaponClass)) continue;
		float pos[3];
		if (!SafeOrigin(entity, pos)) continue;
		const double d = DistanceSq(pos, handPos);
		if (d < best) { best = d; slot.weapon = entity; }
	}
	if (slot.weapon)
		MvmDebugLog_LinefAlways("ragdoll.weapon", "paired userid=%d weapon=%p class=%s",
			slot.userId, slot.weapon, slot.weaponClass);
}

DeathSlot& AllocateSlot() {
	for (DeathSlot& slot : g_deaths) if (!slot.used) return slot;
	DeathSlot& oldest = *std::min_element(g_deaths, g_deaths + kMaxDeaths,
		[](const DeathSlot& a, const DeathSlot& b) { return a.deathTime < b.deathTime; });
	Restore(oldest);
	oldest = DeathSlot{};
	return oldest;
}

void RestoreAll() {
	RagdollWeaponArmPhysics_Clear();
	for (DeathSlot& slot : g_deaths) {
		if (slot.used) Restore(slot);
		slot = DeathSlot{};
	}
}

void CacheLivingWeapons(double now) {
	static ULONGLONG nextTree=0;
	const bool treeProbe=MvmDebugLog_Active() && GetTickCount64()>=nextTree;
	if(treeProbe) nextTree=GetTickCount64()+500;
	// CEntityHandle has a 15-bit entry index; cosmetic entities live above the
	// legacy GetHighestEntityIndex() bound. Read-only, throttled diagnostic only.
	if(treeProbe && g_pEntityList && *g_pEntityList && g_GetEntityFromIndex) {
		__try {
			for(int i=GetHighestEntityIndex()+1;i<(1<<15);++i) {
				auto* entity=(CEntityInstance*)g_GetEntityFromIndex(*g_pEntityList,i);
				const char* cls=SafeClass(entity);
				if(cls && (std::strstr(cls,"keychain") || std::strstr(cls,"Keychain"))) ProbeWeaponTree(entity,-1,"charm");
			}
		} __except(EXCEPTION_EXECUTE_HANDLER) {}
	}
	int pawnCount = 0;
	int armedCount = 0;
	for (int i = 0; i <= GetHighestEntityIndex(); ++i) {
		CEntityInstance* pawn = EntityAt(i);
		const char* probeClass=treeProbe?SafeClass(pawn):nullptr;
		if(probeClass && (std::strstr(probeClass,"keychain") || std::strstr(probeClass,"Keychain"))) ProbeWeaponTree(pawn,-1,"charm");
		bool isPawn = false;
		__try { isPawn = pawn && pawn->IsPlayerPawn(); }
		__except (EXCEPTION_EXECUTE_HANDLER) { isPawn = false; }
		unsigned health = 0;
		if (!isPawn || !SafeHealth(pawn, health) || health == 0) continue;
		++pawnCount;
		int userId = -1;
		__try {
			const auto controller = pawn->GetPlayerControllerHandle();
			if (controller.IsValid()) userId = controller.GetEntryIndex() - 1;
		} __except (EXCEPTION_EXECUTE_HANDLER) { userId = -1; }
		if (userId < 0 || userId >= kMaxUsers) continue;
		CEntityInstance* weapon = SafeActiveWeapon(pawn);
		const char* cls = SafeClass(weapon);
		if (!weapon || !IsGunClass(cls)) continue;
		if(treeProbe) ProbeWeaponTree(weapon,userId,"living");
		++armedCount;
		float origin[3];
		if (!SafeOrigin(pawn, origin)) continue;
		LastHeld& held = g_lastHeld[userId];
		held.sampleTime = now;
		held.pawn = pawn;
		held.weapon = weapon;
		std::memcpy(held.origin, origin, sizeof(origin));
		std::snprintf(held.weaponClass, sizeof(held.weaponClass), "%s", cls);
		CaptureAttachmentLocal(weapon,"muzzle_flash","muzzle",held.haveMuzzleLocal,held.muzzleLocal,held.muzzleLocalQ);
		CaptureAttachmentLocal(weapon,"shell_eject","shell",held.haveShellLocal,held.shellLocal,nullptr);
	}
	if (MvmDebugLog_Active()) {
		static ULONGLONG lastLog = 0;
		const ULONGLONG tick = GetTickCount64();
		if (tick - lastLog >= 1000) {
			lastLog = tick;
			MvmDebugLog_LinefAlways("ragdoll.weapon", "cache livingPawns=%d armed=%d", pawnCount, armedCount);
		}
	}
}

} // namespace

bool RagdollWeapon_KeepEnabled() { return g_keep; }
bool RagdollWeapon_FireEnabled() { return g_fire; }
double RagdollWeapon_FireChance() { return g_fireChance; }
double RagdollWeapon_FireDurationMs() { return g_fireDurationMs; }
double RagdollWeapon_HoldDurationMs() { return g_holdDurationMs; }
void RagdollWeapon_SetFireChance(double v) { if(std::isfinite(v)) g_fireChance=std::clamp(v,0.0,100.0); }
void RagdollWeapon_SetFireDurationMs(double v) { if(std::isfinite(v)) g_fireDurationMs=std::clamp(v,1.0,5000.0); }
void RagdollWeapon_SetHoldDurationMs(double v) { if(std::isfinite(v)) g_holdDurationMs=std::clamp(v,1.0,10000.0); }

void RagdollWeapon_SetKeepEnabled(bool enabled) {
	if (g_keep == enabled) return;
	g_keep = enabled;
	if (!enabled) { g_fire=false; RestoreAll(); }
}

void RagdollWeapon_SetFireEnabled(bool enabled) { if(enabled) RagdollWeapon_SetKeepEnabled(true); g_fire = enabled; }

void RagdollWeapon_OnGameEvent(SOURCESDK::CS2::CGameEvent* event) {
	if (!g_keep || !event) return;
	if (0 == _stricmp(event->GetName(), "round_start")) { RestoreAll(); return; }
	if (0 != _stricmp(event->GetName(), "player_death")) return;
	int userId = -1;
	if (!DeathUserId(event, userId)) {
		MvmDebugLog_LinefAlways("ragdoll.weapon", "death rejected: userid parse failed");
		return;
	}
	const double now = g_MirvTime.curtime_get();
	for (DeathSlot& old : g_deaths)
		if (old.used && old.userId == userId && std::abs(old.deathTime - now) < 0.02) return;

	CEntityInstance* controller = EntityAt(userId + 1);
	CEntityInstance* pawn = nullptr;
	if (controller) {
		__try {
			const auto handle = controller->GetPlayerPawnHandle();
			if (handle.IsValid()) pawn = EntityAt(handle.GetEntryIndex());
		} __except (EXCEPTION_EXECUTE_HANDLER) { pawn = nullptr; }
	}
	CEntityInstance* weapon = SafeActiveWeapon(pawn);
	const char* cls = SafeClass(weapon);
	float deathOrigin[3] = {};
	const bool haveCurrent = weapon && IsGunClass(cls) && SafeOrigin(pawn, deathOrigin);
	LastHeld* held = (userId >= 0 && userId < kMaxUsers) ? &g_lastHeld[userId] : nullptr;
	const bool haveRecent = held && now - held->sampleTime >= -0.1 && now - held->sampleTime < 1.0
		&& IsGunClass(held->weaponClass);
	if (!haveCurrent && !haveRecent) {
		MvmDebugLog_LinefAlways("ragdoll.weapon",
			"death rejected userid=%d controller=%p pawn=%p weapon=%p class=%s cacheAge=%.3f cacheClass=%s",
			userId, controller, pawn, weapon, cls ? cls : "-",
			held ? now - held->sampleTime : -1.0, held && held->weaponClass[0] ? held->weaponClass : "-");
		return;
	}
	if (!haveCurrent) {
		weapon = held->weapon;
		cls = held->weaponClass;
		std::memcpy(deathOrigin, held->origin, sizeof(deathOrigin));
	}
	// A repeated death after a seek, or a later death with the same picked-up gun,
	// must never leave two slots writing competing poses into one scene node.
	for (DeathSlot& old : g_deaths) {
		if (old.used && (old.weapon == weapon || old.userId == userId)) {
			Restore(old); old=DeathSlot{};
		}
	}

	DeathSlot& slot = AllocateSlot();
	slot = DeathSlot{};
	slot.used = true;
	slot.userId = userId;
	slot.deathTime = now;
	int tick = 0;
	g_MirvTime.GetCurrentDemoTick(tick);
	slot.seed = (uint32_t)tick ^ ((uint32_t)userId * 0x9e3779b9u);
	slot.weaponItem=SafeWeaponItem(weapon);
	slot.fireProfile=RagdollWeaponProfile(slot.weaponItem,cls);
	slot.willFire = RagdollWeaponWillFire(slot.seed, g_fireChance) && slot.fireProfile;
	MvmDebugLog_LinefAlways("ragdoll.weapon.profile","userid=%d weapon=%p item=%u class=%s profile=%s cycleMs=%.3f singleShot=%d",
		userId,weapon,slot.weaponItem,cls,slot.fireProfile?slot.fireProfile->name:"unresolved",
		slot.fireProfile?slot.fireProfile->cycle*1000:0,slot.fireProfile?slot.fireProfile->singleShot:0);
	slot.holdSeconds = g_holdDurationMs * 0.001;
	slot.fireSeconds = g_fireDurationMs * 0.001;
	slot.nextShotTime = now;
	slot.weapon = weapon;
	slot.victimPawn = haveCurrent ? pawn : held->pawn;
	std::memcpy(slot.deathOrigin, deathOrigin, sizeof(deathOrigin));
	std::snprintf(slot.weaponClass, sizeof(slot.weaponClass), "%s", cls);
	if(held && held->weapon==weapon){slot.haveMuzzleLocal=held->haveMuzzleLocal; slot.haveShellLocal=held->haveShellLocal; std::memcpy(slot.muzzleLocal,held->muzzleLocal,sizeof(slot.muzzleLocal)); std::memcpy(slot.shellLocal,held->shellLocal,sizeof(slot.shellLocal)); std::memcpy(slot.muzzleLocalQ,held->muzzleLocalQ,sizeof(slot.muzzleLocalQ));}
	else { CaptureAttachmentLocal(weapon,"muzzle_flash","muzzle",slot.haveMuzzleLocal,slot.muzzleLocal,slot.muzzleLocalQ); CaptureAttachmentLocal(weapon,"shell_eject","shell",slot.haveShellLocal,slot.shellLocal,nullptr); }
	MvmDebugLog_LinefAlways("ragdoll.weapon", "death userid=%d class=%s fireRoll=%d seed=%u chance=%.1f holdMs=%.0f fireMs=%.0f",
		userId, slot.weaponClass, slot.willFire ? 1 : 0, slot.seed, g_fireChance, g_holdDurationMs, g_fireDurationMs);
}

void RagdollWeapon_OnEntityAdded(CEntityInstance* entity, unsigned int handle) {
	if (!g_keep || !entity) return;
	const int index = SOURCESDK::CS2::CBaseHandle(handle).GetEntryIndex();
	if (index < 0) return;
	const double now = g_MirvTime.curtime_get();
	for (DeathSlot& slot : g_deaths) {
		if (!slot.used || now - slot.deathTime < -0.1 || now - slot.deathTime > kResolveWindow) continue;
		if (slot.candidateCount < kMaxCandidates) slot.candidates[slot.candidateCount++] = entity;
	}
}

void RagdollWeapon_OnEntityRemoved(CEntityInstance* entity) {
	if (!entity) return;
	RagdollWeaponArmPhysics_Forget(entity);
	for (DeathSlot& slot : g_deaths) {
		if (!slot.used) continue;
		if (slot.weapon == entity) { slot = DeathSlot{}; continue; }
		if (slot.victimPawn == entity || slot.ragdoll == entity) {
			if(slot.released) { slot.victimPawn=nullptr; slot.ragdoll=nullptr; }
			else { Restore(slot); slot=DeathSlot{}; continue; }
		}
		for (int i = 0; i < slot.candidateCount; ++i) if (slot.candidates[i] == entity) slot.candidates[i] = nullptr;
		// Child scene nodes can disappear independently of the weapon. Forget their saved
		// addresses before the engine frees them; never restore a stale child pointer.
		unsigned char* removedNode = SceneNode(entity);
		for (int i = 0; i < slot.savedCount; ++i)
			if (slot.saved[i].node == removedNode) slot.saved[i].node = nullptr;
	}
	for (LastHeld& held : g_lastHeld) {
		if (held.weapon == entity) held.weapon = nullptr;
		if (held.pawn == entity) held.pawn = nullptr;
	}
}

void RagdollWeapon_RunFrame() {
	if (!g_keep) return;
	const double now = g_MirvTime.curtime_get();
	static double previousTime = -1.0;
	if (now < previousTime - 0.1) {
		RestoreAll();
		for (LastHeld& held : g_lastHeld) held=LastHeld{};
	}
	previousTime = now;
	if (g_fire && !g_effectsReady) {
		static ULONGLONG retryMs = 0;
		if (GetTickCount64() >= retryMs) {
			retryMs = GetTickCount64() + 1000;
			g_effectsReady = PrecacheFireEffects();
			MvmDebugLog_Linef("ragdoll.weapon.precache","ready=%d",g_effectsReady);
		}
	}
	CacheLivingWeapons(now);
	for (DeathSlot& slot : g_deaths) {
		if (!slot.used) continue;
		if (ReleaseIfPickedUp(slot)) continue;
		SuspendMovement(slot);
		g_writePhase = "early";
		if(ApplyReleased(slot,now)) continue;
		float handPos[3], handQ[4];
		if (slot.ragdoll && slot.weapon && SafeAttachment(slot.ragdoll, "weapon_hand_R", handPos, handQ))
			ApplyToHand(slot, handPos, handQ, g_fire ? RagdollWeaponKick(now - slot.lastShotTime) : 0.0);
	}
}

static void RagdollWeapon_RunLateFrameInternal() {
	if (!g_keep) return;
	const double now = g_MirvTime.curtime_get();
	for (DeathSlot& slot : g_deaths) {
		if (!slot.used) continue;
		if (now <= slot.armWatchUntil && 0 == std::strcmp(g_writePhase, "late")) {
			float p[3], q[4];
			if (SafeAttachment(slot.ragdoll, "weapon_hand_R", p, q))
				MvmDebugLog_LinefAlways("ragdoll.weapon.armwatch",
					"userid=%d t=%.6f hand=(%.6f %.6f %.6f) q=(%.6f %.6f %.6f %.6f)",
					slot.userId, now - slot.armWatchStart, p[0], p[1], p[2], q[0], q[1], q[2], q[3]);
		}
		if (ReleaseIfPickedUp(slot)) continue;
		SuspendMovement(slot);
		if(ApplyReleased(slot,now)) continue;
		ResolveRagdoll(slot);
		if (!slot.ragdoll) {
			if (now - slot.deathTime > kResolveWindow) slot = DeathSlot{};
			continue;
		}
		float handPos[3], handQ[4];
		if (!SafeAttachment(slot.ragdoll, "weapon_hand_R", handPos, handQ)) continue;
		if (slot.armProbePasses > 0 && 0 == --slot.armProbePasses) {
			const double dx = handPos[0] - slot.armProbeHand[0];
			const double dy = handPos[1] - slot.armProbeHand[1];
			const double dz = handPos[2] - slot.armProbeHand[2];
			MvmDebugLog_LinefAlways("ragdoll.weapon.arm",
				"userid=%d phase=hand-result shot=%u before=(%.3f %.3f %.3f) after=(%.3f %.3f %.3f) delta=(%.3f %.3f %.3f) distance=%.3f",
				slot.userId, slot.armProbeShot,
				slot.armProbeHand[0], slot.armProbeHand[1], slot.armProbeHand[2],
				handPos[0], handPos[1], handPos[2], dx, dy, dz,
				std::sqrt(dx * dx + dy * dy + dz * dz));
		}
		ResolveWeapon(slot, handPos);
		if (!slot.weapon) continue;
		SuspendMovement(slot);

		if(slot.attachedTime<0) slot.attachedTime=now;
		BeginRelease(slot,now);
		if(ApplyReleased(slot,now)) continue;
		if(slot.fireStartTime<0) slot.fireStartTime=now;
		const double fireAge=now-slot.fireStartTime;
		const bool shot=g_fire && slot.willFire && g_effectsReady && now-slot.attachedTime<slot.holdSeconds &&
			RagdollWeaponShotDue(fireAge,slot.fireSeconds,slot.shotNumber,now,slot.nextShotTime,slot.fireProfile && slot.fireProfile->singleShot)
			&& (slot.shotNumber>0 || fireAge<slot.fireSeconds);
		const double kick=shot?RagdollWeaponKick(0):
			(g_fire?RagdollWeaponKick(now-slot.lastShotTime):0.0);
		if (!ApplyToHand(slot, handPos, handQ, kick))
			MvmDebugLog_Linef("ragdoll.weapon", "transform failed userid=%d", slot.userId);
		else if (shot) {
			const bool dispatched=DispatchFireEffects(slot,handPos,handQ,kick);
			ProbeArmPhysics(slot);
			float recoilPos[3], recoilQ[4];
			if (!SafeAttachment(slot.weapon, "muzzle_flash", recoilPos, recoilQ)
				&& !SafeAttachment(slot.weapon, "muzzle", recoilPos, recoilQ)) {
				std::memcpy(recoilPos, handPos, sizeof(recoilPos));
				std::memcpy(recoilQ, handQ, sizeof(recoilQ));
			}
			RagdollWeaponArmPhysics_Queue(slot.ragdoll, slot.userId, slot.weaponClass,
				recoilPos, recoilQ);
			slot.armProbePasses = 2; // skip this frame's post pass; sample after CS2 consumes the impulse
			std::memcpy(slot.armProbeHand, handPos, sizeof(slot.armProbeHand));
			slot.armProbeShot = slot.shotNumber + 1;
			slot.lastShotTime=now;
			++slot.shotNumber;
			slot.nextShotTime=now+slot.fireProfile->cycle;
			MvmDebugLog_LinefAlways("ragdoll.weapon","visual-fire userid=%d shot=%u class=%s ageMs=%.3f durationMs=%.3f dispatched=%d",
				slot.userId,slot.shotNumber,slot.weaponClass,fireAge*1000,slot.fireSeconds*1000,dispatched);
		}

	}
}

void RagdollWeapon_RunLateFrame() {
	g_writePhase = "late";
	RagdollWeapon_RunLateFrameInternal();
}

void RagdollWeapon_RunPostFrame() {
	g_writePhase = "post";
	RagdollWeapon_RunLateFrameInternal();
	std::lock_guard<std::mutex> lock(g_drawProbeMutex);
	g_drawProbeCount = 0;
	if (!g_keep || !MvmDebugLog_Active()) return;
	for (const DeathSlot& slot : g_deaths) {
		if (!slot.used || !slot.wroteTransform || !slot.weapon) continue;
		auto& target = g_drawProbeTargets[g_drawProbeCount];
		target.node = SceneNode(slot.weapon); target.user = slot.userId;
		if (!target.node) continue;
		std::memcpy(target.expected, slot.expected, sizeof(target.expected));
		++g_drawProbeCount;
	}
}

// Read at the actual scene draw without calling attachment/animation functions or writing a pose.
// This deliberately observes a different stage from ApplyToHand's immediate write/readback.
static bool ReadProbeTransform(unsigned char* node, float out[8]) {
	__try { std::memcpy(out, node + g_clientDllOffsets.CGameSceneNode.m_nodeToWorld, 8*sizeof(float)); return true; }
	__except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static bool ReadProbeSources(unsigned char* node,float origin[3],float angles[3]) {
	const auto& o=g_clientDllOffsets.CGameSceneNode;
	if(!node || !o.m_vecOrigin || !o.m_angRotation) return false;
	__try {
		std::memcpy(origin,node+o.m_vecOrigin,3*sizeof(float));
		std::memcpy(angles,node+o.m_angRotation,3*sizeof(float));
		return std::isfinite(origin[0])&&std::isfinite(origin[1])&&std::isfinite(origin[2])
			&&std::isfinite(angles[0])&&std::isfinite(angles[1])&&std::isfinite(angles[2]);
	} __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
void RagdollWeapon_ProbeDraw(void* batch, int count, const char* material) {
	if (!MvmDebugLog_Active() || !material || !std::strstr(material, "weapon")) return;
	DrawProbeTarget targets[kMaxDeaths]; int n;
	{ std::lock_guard<std::mutex> lock(g_drawProbeMutex);
		n = g_drawProbeCount; std::memcpy(targets, g_drawProbeTargets, n * sizeof(targets[0])); }
	static std::atomic<ULONGLONG> next{0};
	const auto now = GetTickCount64();
	auto due = next.load();
	if (now < due || !next.compare_exchange_strong(due, now + 50)) return;
	for (int i = 0; i < n; ++i) {
		float actual[8] = {}; bool ok = ReadProbeTransform(targets[i].node, actual);
		float source[3]={},angles[3]={}; const bool sourceOk=ReadProbeSources(targets[i].node,source,angles);
		const float* e = targets[i].expected;
		MvmDebugLog_LinefAlways("ragdoll.weapon.draw", "user=%d node=%p off=%td ok=%d material=%s actual=(%.3f %.3f %.3f) q=(%.6f %.6f %.6f %.6f) expected=(%.3f %.3f %.3f) eq=(%.6f %.6f %.6f %.6f) sourceOk=%d source=(%.3f %.3f %.3f) angles=(%.3f %.3f %.3f)", targets[i].user, targets[i].node, g_clientDllOffsets.CGameSceneNode.m_nodeToWorld, ok, material, actual[0],actual[1],actual[2],actual[4],actual[5],actual[6],actual[7],e[0],e[1],e[2],e[4],e[5],e[6],e[7],sourceOk?1:0,source[0],source[1],source[2],angles[0],angles[1],angles[2]);
	}
}

void RagdollWeapon_AppendStateJson(std::ostringstream& out) {
	int waiting=0;
	for(const auto& slot:g_deaths) if(slot.used && slot.physicsWarned) ++waiting;
	out << ",\"ragdollPhysicsWaiting\":" << waiting;
	out << ",\"ragdollKeepWeapon\":" << (g_keep ? "true" : "false")
		<< ",\"ragdollDeathFire\":" << (g_fire ? "true" : "false")
		<< ",\"ragdollFireChance\":" << g_fireChance
		<< ",\"ragdollFireDurationMs\":" << g_fireDurationMs
		<< ",\"ragdollHoldDurationMs\":" << g_holdDurationMs;
}

void RagdollWeapon_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd) {
	const char* setting = argc >= 3 ? args->ArgV(2) : "state";
	if (0 == _stricmp(setting, "armprobe")) {
		if (argc < 5 || !MvmDebugLog_Active()) {
			advancedfx::Message("Start mvm_debug, then ragdollweapon armprobe <userid> <force 0..10000>. Uses an existing corpse.\n");
			return;
		}
		char* end = nullptr;
		const double strength = std::strtod(args->ArgV(4), &end);
		if (end == args->ArgV(4) || *end || !std::isfinite(strength) || strength < 0 || strength > 10000) return;
		const int userId = std::atoi(args->ArgV(3));
		for (auto& slot : g_deaths) {
			if (!slot.used || slot.userId != userId || !slot.ragdoll) continue;
			float hand[3], q[4];
			if (!SafeAttachment(slot.ragdoll, "weapon_hand_R", hand, q)) continue;
			RagdollWeaponArmPhysics_Queue(slot.ragdoll, slot.userId, slot.weaponClass, hand, q, strength);
			slot.armWatchStart = g_MirvTime.curtime_get();
			slot.armWatchUntil = slot.armWatchStart + 0.75;
			MvmDebugLog_LinefAlways("ragdoll.weapon.armwatch",
				"userid=%d phase=begin force=%.3f hand=(%.6f %.6f %.6f) q=(%.6f %.6f %.6f %.6f)",
				userId, strength, hand[0], hand[1], hand[2], q[0], q[1], q[2], q[3]);
			advancedfx::Message("armprobe queued for userid=%d force=%.0f; resume demo to simulate.\n", userId, strength);
			return;
		}
		advancedfx::Message("armprobe: no tracked corpse for userid=%d.\n", userId);
		return;
	}
	if (0 == _stricmp(setting, "state") || 0 == _stricmp(setting, "status")) {
		advancedfx::Message("%s ragdollweapon: keep=%s deathfire=%s chance=%.0f%% fireMs=%.0f holdMs=%.0f drop=native physics (cinematic only; no bullets).\n",
			cmd, g_keep ? "ON" : "off", g_fire ? "ON" : "off", g_fireChance,g_fireDurationMs,g_holdDurationMs);
		return;
	}
	if (0 == _stricmp(setting,"reset")) {
		RagdollWeapon_SetKeepEnabled(false);
		g_fireChance=100; g_fireDurationMs=250; g_holdDurationMs=250;
		return;
	}
	void (*setNumber)(double) = nullptr;
	if (0 == _stricmp(setting,"chance")) setNumber=RagdollWeapon_SetFireChance;
	else if (0 == _stricmp(setting,"firems")) setNumber=RagdollWeapon_SetFireDurationMs;
	else if (0 == _stricmp(setting,"holdms")) setNumber=RagdollWeapon_SetHoldDurationMs;
	if (setNumber) {
		char* end=nullptr;
		const char* text=argc>=4?args->ArgV(3):"";
		double value=std::strtod(text,&end);
		if(end==text || *end || !std::isfinite(value)) {
			advancedfx::Warning("ragdollweapon: enter a finite number.\n"); return;
		}
		setNumber(value);
		if(argc<5 || _stricmp(args->ArgV(4),"quiet"))
			advancedfx::Message("ragdollweapon %s updated; applies to new deaths.\n",setting);
		return;
	}
	bool* target = nullptr;
	if (0 == _stricmp(setting, "keep")) target = &g_keep;
	else if (0 == _stricmp(setting, "fire") || 0 == _stricmp(setting, "deathfire")) target = &g_fire;
	if (!target) {
		advancedfx::Message("%s ragdollweapon keep|fire on|off|toggle; chance 0..100; firems 1..5000; holdms 1..10000; reset; state\n", cmd);
		return;
	}
	const char* value = argc >= 4 ? args->ArgV(3) : "toggle";
	const bool enabled = 0 == _stricmp(value, "toggle") ? !*target
		: (0 == _stricmp(value, "on") || 0 == _stricmp(value, "1"));
	if (target == &g_keep) RagdollWeapon_SetKeepEnabled(enabled);
	else RagdollWeapon_SetFireEnabled(enabled);
	advancedfx::Message("%s ragdollweapon %s: %s.\n", cmd, setting, enabled ? "ON" : "off");
}

} // namespace Filmmaker
