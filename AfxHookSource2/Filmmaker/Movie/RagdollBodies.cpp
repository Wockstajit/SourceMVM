#include "RagdollBodies.h"

#include "../Debug/MvmDebugLog.h"
#include "../Platform/JsonParser.h"
#include "../../ClientEntitySystem.h"
#include "../../SchemaSystem.h"
#include "../../../Shared/binutils.h"
#include "../../../Deps/release/prop/AfxHookSource/SourceSdkShared.h"
#include "../../../Deps/release/prop/cs2/sdk_src/public/entityhandle.h"
#include "../../../Deps/release/prop/cs2/sdk_src/public/igameevents.h"
#include "../../../Deps/release/prop/cs2/sdk_src/public/tier1/utlstring.h"

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstring>

namespace Filmmaker {
namespace RagdollBodies {

using DeathAnimationsMath::QuatNormalize;

namespace {

using MotionGet = int(__fastcall*)(void*);
using MotionSet = void(__fastcall*)(void*, int);
using PoseGet = float*(__fastcall*)(void*, float*);
using PoseSet = void(__fastcall*)(void*, const float*);
using BodyGet = void*(__fastcall*)(void*, int);
template<class T> T Method(void* object, int index) { return reinterpret_cast<T>((*reinterpret_cast<void***>(object))[index]); }

bool Prefix(void* object, int index, const unsigned char* bytes, size_t length) {
	__try {
		auto* fn = (*reinterpret_cast<unsigned char***>(object))[index];
		HMODULE module = nullptr;
		if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			(const char*)fn, &module) || module != GetModuleHandleA("vphysics2.dll")) return false;
		return std::memcmp(fn, bytes, length) == 0;
	} __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool VerifiedAggregate(void* aggregate) {
	static const unsigned char getBody[] = {0x85,0xD2,0x78,0x11,0x3B,0x51,0x38,0x7D,0x0C,0x48,0x8B,0x41,0x40};
	return aggregate && Prefix(aggregate, 24, getBody, sizeof(getBody));
}

bool VerifiedBody(void* body) {
	static const unsigned char getMotion[] = {0x48,0x8B,0x41,0x08,0x8B,0x40,0x54};
	static const unsigned char setMotion[] = {0x48,0x89,0x5C,0x24,0x08,0x57,0x48,0x83,0xEC,0x20,0x48,0x8B,0xD9};
	static const unsigned char setPose[] = {0x48,0x8B,0x49,0x08,0xE9};
	static const unsigned char getPose[] = {0x48,0x83,0xEC,0x68,0x48,0x8B,0x41,0x08};
	static const unsigned char setVelocity[] = {0x48,0x89,0x5C,0x24,0x08,0x57,0x48,0x83,0xEC,0x20,0x8B,0x02};
	static const unsigned char setAngular[] = {0x40,0x53,0x48,0x83,0xEC,0x40,0xF3,0x0F,0x10,0x05};
	static const unsigned char getVelocity[] = {0x48,0x8B,0x41,0x08,0xF2,0x0F,0x10,0x80,0x08,0x01,0x00,0x00};
	return body && Prefix(body, 66, getVelocity, sizeof(getVelocity)) && Prefix(body, 4, getMotion, sizeof(getMotion)) && Prefix(body, 3, setMotion, sizeof(setMotion))
		&& Prefix(body, 53, setPose, sizeof(setPose)) && Prefix(body, 56, getPose, sizeof(getPose))
		&& Prefix(body, 64, setVelocity, sizeof(setVelocity)) && Prefix(body, 68, setAngular, sizeof(setAngular));
}


void* AggregateOf(CEntityInstance* entity) {
	const auto& o = g_clientDllOffsets;
	if (!entity || !o.ModelChain.m_CBodyComponent || !o.ModelChain.m_skeletonInstance
		|| !o.ModelChain.m_modelState || !o.DeathWeaponPhysics.m_pVPhysicsAggregate) return nullptr;
	__try {
		auto* component = *(unsigned char**)((unsigned char*)entity + o.ModelChain.m_CBodyComponent);
		if ((uintptr_t)component <= 0x10000) return nullptr;
		void* aggregate = *(void**)(component + o.ModelChain.m_skeletonInstance + o.ModelChain.m_modelState
			+ o.DeathWeaponPhysics.m_pVPhysicsAggregate);
		return (uintptr_t)aggregate > 0x10000 ? aggregate : nullptr;
	} __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}


// Where a dying pawn keeps its ragdoll object. In demo playback the pawn itself becomes the
// corpse; its model-state aggregate keeps only the one hit body, and the fifteen ragdoll bodies
// hang off this object. client.dll's apply-bone-force function reads it first thing
// (`mov rcx, [rcx + disp32]`), which gives the offset without a hardcoded value; the schema's
// CBaseAnimGraph::m_pRagdollControl is the fallback.
ptrdiff_t RagdollObjectOffset() {
	static bool tried = false;
	static ptrdiff_t offset = 0;
	if (tried) return offset;
	tried = true;
	if (HMODULE client = GetModuleHandleA("client.dll")) {
		Afx::BinUtils::ImageSectionsReader sections(client);
		if (!sections.Eof()) {
			const auto match = Afx::BinUtils::FindPatternString(sections.GetMemRange(),
				"40 53 55 41 57 48 83 EC 50 48 8B E9 48 63 DA 48 8B 89 ?? ?? ?? ?? 4D 8B F8 48 85 C9 0F 84 ?? ?? ?? ?? 48 8B 01 FF 50 40 83 38 00");
			if (!match.IsEmpty()) {
				int32_t displacement = 0;
				std::memcpy(&displacement, reinterpret_cast<void*>(match.Start + 18), sizeof(displacement));
				if (displacement > 0 && displacement < 0x10000) offset = displacement;
			}
		}
	}
	const ptrdiff_t schema = g_clientDllOffsets.DeathWeaponRagdoll.m_pRagdollControl;
	MvmDebugLog_LinefAlways("deathanim.resolve", "ragdollObjectOffset pattern=0x%tx schema=0x%tx", offset, schema);
	if (!offset) offset = schema;
	return offset;
}

// The pawn's ragdoll object: slot 8 returns a descriptor whose first int is the body count,
// slot 4 returns body i. Both must live in client.dll or vphysics2.dll.
void* RagdollControlOf(CEntityInstance* entity) {
	const ptrdiff_t off = RagdollObjectOffset();
	if (!entity || !off) return nullptr;
	__try {
		void* control = *(void**)((unsigned char*)entity + off);
		return (uintptr_t)control > 0x10000 ? control : nullptr;
	} __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

bool SlotInEngine(void* object, int index) {
	__try {
		void* fn = (*reinterpret_cast<void***>(object))[index];
		HMODULE module = nullptr;
		if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			(const char*)fn, &module)) return false;
		return module == GetModuleHandleA("client.dll") || module == GetModuleHandleA("vphysics2.dll")
			|| module == GetModuleHandleA("animationsystem.dll");
	} __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

int ControlCount(void* control) {
	if (!control || !SlotInEngine(control, 4) || !SlotInEngine(control, 8)) return -1;
	__try {
		void* desc = Method<BodyGet>(control, 8)(control, 0);
		if ((uintptr_t)desc <= 0x10000) return -2;
		return *(int*)desc;
	} __except (EXCEPTION_EXECUTE_HANDLER) { return -3; }
}

void* ControlBody(void* control, int i) {
	__try { return Method<BodyGet>(control, 4)(control, i); }
	__except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

int BodyCount(void* aggregate) {
	__try {
		const int n = *(int*)((unsigned char*)aggregate + 0x38);
		return n >= 0 && n <= 64 ? n : 0;
	} __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

void* BodyAt(void* aggregate, int i) {
	__try { return Method<BodyGet>(aggregate, 24)(aggregate, i); }
	__except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}


void* RouteObject(int route, CEntityInstance* carrier) {
	return route == 0 ? AggregateOf(carrier) : RagdollControlOf(carrier);
}

void* RouteBody(int route, void* object, int i) {
	return route == 0 ? BodyAt(object, i) : ControlBody(object, i);
}

} // namespace

CEntityInstance* EntityAt(int index) {
	if (index < 0 || index > GetHighestEntityIndex() || !g_pEntityList || !*g_pEntityList || !g_GetEntityFromIndex)
		return nullptr;
	return (CEntityInstance*)g_GetEntityFromIndex(*g_pEntityList, index);
}

CEntityInstance* EntityInSlot(int index) {
	if (index < 0 || index >= 0x8000 || !g_pEntityList || !*g_pEntityList || !g_GetEntityFromIndex)
		return nullptr;
	__try { return (CEntityInstance*)g_GetEntityFromIndex(*g_pEntityList, index); }
	__except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

CEntityInstance* ClientRagdollOf(CEntityInstance* entity) {
	const ptrdiff_t off = g_clientDllOffsets.DeathWeaponRagdoll.m_pClientsideRagdoll;
	if (!entity || !off) return nullptr;
	__try {
		auto* child = *(CEntityInstance**)((unsigned char*)entity + off);
		return (uintptr_t)child > 0x10000 ? child : nullptr;
	} __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

bool ReadBody(void* body, Pose& out, int& motion) {
	__try {
		float t[8];
		Method<PoseGet>(body, 56)(body, t);
		motion = Method<MotionGet>(body, 4)(body);
		for (int k = 0; k < 3; ++k) out.p[k] = t[k];
		for (int k = 0; k < 4; ++k) out.q[k] = t[4 + k];
		for (float v : t) if (!std::isfinite(v)) return false;
		return QuatNormalize(out.q);
	} __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Place a body exactly on the clip pose, moving with the clip's own velocity so the solver and the
// renderer see the motion the clip has. Every frame overwrites whatever physics did in between.
// Returns whether the motion type had to be re-asserted.
bool HoldBody(void* body, const Pose& pose, const float linear[3], const float angularDeg[3], bool& reasserted) {
	__try {
		if (Method<MotionGet>(body, 4)(body) != kDynamic) { Method<MotionSet>(body, 3)(body, kDynamic); reasserted = true; }
		const float t[8] = {pose.p[0], pose.p[1], pose.p[2], 1.f, pose.q[0], pose.q[1], pose.q[2], pose.q[3]};
		Method<PoseSet>(body, 53)(body, t);
		Method<PoseSet>(body, 64)(body, linear);
		Method<PoseSet>(body, 68)(body, angularDeg);
		return true;
	} __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Solid playback: steer a body by velocity only (DeathAnimationsMath::DriveVelocity), so the physics
// step still resolves floors, walls and the other bodies. Angular velocity is about WORLD axes in
// degrees/s: the setter scales by pi/180 itself (vphysics2 slot 68 loads that constant first), and a
// probe of commanded against produced pelvis rotation agreed with world axes, not local ones.
bool DriveBody(void* body, const float linear[3], const float angularDeg[3], bool& reasserted) {
	__try {
		if (Method<MotionGet>(body, 4)(body) != kDynamic) { Method<MotionSet>(body, 3)(body, kDynamic); reasserted = true; }
		Method<PoseSet>(body, 64)(body, linear);
		Method<PoseSet>(body, 68)(body, angularDeg);
		return true;
	} __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// The motion physics gave a body: linear from the verified getter (slot 66, which reads +0x108 of the
// body's simulation object) and angular from the three floats after it (+0x114, radians/s -- the
// layout vphysics2's velocity-at-point function reads), returned in degrees/s.
bool ReadVelocity(void* body, float linear[3], float angularDeg[3]) {
	__try {
		float v[3];
		Method<PoseGet>(body, 66)(body, v);
		const unsigned char* sim = *reinterpret_cast<unsigned char**>(reinterpret_cast<unsigned char*>(body) + 8);
		const float* w = reinterpret_cast<const float*>(sim + 0x114);
		for (int k = 0; k < 3; ++k) {
			linear[k] = v[k];
			angularDeg[k] = float(w[k] * 57.29577951308232);
			if (!std::isfinite(linear[k]) || !std::isfinite(angularDeg[k])) return false;
		}
		return true;
	} __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool ForceDynamic(void* body) {
	__try { Method<MotionSet>(body, 3)(body, kDynamic); return true; }
	__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool DampHorizontal(void* body, float factor) {
	__try {
		float v[3];
		Method<PoseGet>(body, 66)(body, v);
		v[0] *= factor; v[1] *= factor;
		Method<PoseSet>(body, 64)(body, v);
		return true;
	} __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool ReleaseBody(void* body, const Pose& pose, const float linear[3], const float angularDeg[3]) {
	__try {
		const float t[8] = {pose.p[0], pose.p[1], pose.p[2], 1.f, pose.q[0], pose.q[1], pose.q[2], pose.q[3]};
		Method<MotionSet>(body, 3)(body, kDynamic);
		Method<PoseSet>(body, 53)(body, t);
		Method<PoseSet>(body, 64)(body, linear);
		Method<PoseSet>(body, 68)(body, angularDeg);
		return true;
	} __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool SafeOrigin(CEntityInstance* e, float out[3]) {
	if (!e) return false;
	__try {
		e->GetOrigin(out[0], out[1], out[2]);
		return std::isfinite(out[0]) && std::isfinite(out[1]) && std::isfinite(out[2]);
	} __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

const char* SafeClassName(CEntityInstance* e) {
	__try { const char* n = e ? e->GetClassName() : nullptr; return n ? n : "-"; }
	__except (EXCEPTION_EXECUTE_HANDLER) { return "?"; }
}


CEntityInstance* PawnOfUser(int userid, int* pawnIndex) {
	CEntityInstance* controller = EntityAt(userid + 1);
	if (!controller) return nullptr;
	__try {
		const auto handle = controller->GetPlayerPawnHandle();
		if (!handle.IsValid()) return nullptr;
		if (pawnIndex) *pawnIndex = handle.GetEntryIndex();
		return EntityAt(handle.GetEntryIndex());
	} __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}


typedef bool (*SaveKV3AsJSON_t)(const SOURCESDK::CS2::KeyValues3*, SOURCESDK::CS2::CUtlString*, SOURCESDK::CS2::CUtlString*);

bool EventJson(SOURCESDK::CS2::CGameEvent* event, JsonValue& out) {
	static SaveKV3AsJSON_t fn = nullptr;
	static bool tried = false;
	if (!tried) {
		tried = true;
		if (HMODULE tier0 = GetModuleHandleA("tier0.dll"))
			fn = (SaveKV3AsJSON_t)GetProcAddress(tier0, "?SaveKV3AsJSON@@YA_NPEBVKeyValues3@@PEAVCUtlString@@1@Z");
	}
	if (!event || !fn) return false;
	SOURCESDK::CS2::CUtlString error, text;
	if (!fn(event->GetDataKeys(), &error, &text) || !text.Get()) return false;
	return JsonParse(text.Get(), out) && out.type == JsonValue::Type::Object;
}

int JInt(const JsonValue& r, const char* k, int def) { const JsonValue* v = r.Find(k); return v ? v->AsInt(def) : def; }
bool JBool(const JsonValue& r, const char* k) { const JsonValue* v = r.Find(k); return v && (v->AsBool() || v->AsInt(0) != 0); }
std::string JStr(const JsonValue& r, const char* k) { const JsonValue* v = r.Find(k); return v ? v->AsString() : std::string(); }

static int EntityIndexOf(CEntityInstance* entity) {
	__try {
		const auto handle = entity->GetHandle();
		return handle.IsValid() ? handle.GetEntryIndex() : -1;
	} __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}

bool Acquire(CEntityInstance* pawn, BodySet& out, char* why, size_t whySize) {
	CEntityInstance* carriers[2] = { pawn, ClientRagdollOf(pawn) };
	size_t used = 0;
	auto note = [&](const char* fmt, auto... v) {
		if (used < whySize) used += (size_t)_snprintf_s(why + used, whySize - used, _TRUNCATE, fmt, v...);
	};
	for (int route = 0; route < 2; ++route) {
		for (int k = 0; k < 2; ++k) {
			CEntityInstance* carrier = carriers[k];
			if (!carrier || (k == 1 && carrier == pawn)) continue;
			void* object = RouteObject(route, carrier);
			int count = -9;
			if (route == 0) count = object && VerifiedAggregate(object) ? BodyCount(object) : -1;
			else count = ControlCount(object);
			note(" [%s/%s obj=%p count=%d", route ? "control" : "aggregate", k ? "child" : "pawn", object, count);
			if (!object || count != kBodies) { note("]"); continue; }
			void* bodies[kBodies];
			int bad = -1;
			for (int i = 0; i < kBodies && bad < 0; ++i) {
				bodies[i] = RouteBody(route, object, i);
				if (!VerifiedBody(bodies[i])) bad = i;
			}
			note(" badBody=%d]", bad);
			if (bad >= 0) continue;
			out.carrier = carrier;
			out.carrierIndex = EntityIndexOf(carrier);
			out.route = route;
			out.object = object;
			std::memcpy(out.bodies, bodies, sizeof(bodies));
			return true;
		}
	}
	return false;
}

bool Current(const BodySet& set) {
	if (!set.object || RouteObject(set.route, set.carrier) != set.object) return false;
	for (int i = 0; i < kBodies; ++i) if (RouteBody(set.route, set.object, i) != set.bodies[i]) return false;
	return true;
}

} // namespace RagdollBodies
} // namespace Filmmaker
