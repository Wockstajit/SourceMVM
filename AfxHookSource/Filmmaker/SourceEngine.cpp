#include "stdafx.h"

#include "SourceEngine.h"
#include "SourceGameProfile.h"
#include "SourceNetvars.h"
#include "SourceSdk2013.h"

#include "RenderView.h"
#include "WrpGlobals.h"
#include "WrpVEngineClient.h"

#include <AfxHookSource2/Filmmaker/Debug/MvmDebugLog.h>
#include <Shared/AfxConsole.h>

#include <windows.h>

#include <cstddef>
#include <cstring>
#include <string>
#include <vector>

extern WrpVEngineClient* g_VEngineClient;

namespace SourceFilmmaker {

using namespace Sdk2013;

namespace {

typedef void* (*CreateInterfaceFn)(const char* name, int* returnCode);

void* g_client = nullptr;
void* g_entityList = nullptr;
const ClientClass* g_classes = nullptr;
int g_classCount = 0;
bool g_ready = false;
volatile LONG64 g_faults = 0;

enum FieldId { kTeam, kHealth, kLifeState, kObserverTarget, kObserverMode, kEyePitch, kEyeYaw, kFieldCount };

struct Field {
	const char* table;
	const char* prop;
	int offset;
};

Field g_fields[kFieldCount] = {};

// Guarded engine access. These hold no objects with destructors, which is what lets them use
// __try; callers turn the results into std::string and friends outside.

void* CallPtr(void* object, int slot) {
	if (!object) return nullptr;
	__try {
		typedef void* (__thiscall* Fn)(void*);
		return ((Fn)(*(void***)object)[slot])(object);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		InterlockedIncrement64(&g_faults);
		return nullptr;
	}
}

void* CallPtrInt(void* object, int slot, int arg) {
	if (!object) return nullptr;
	__try {
		typedef void* (__thiscall* Fn)(void*, int);
		return ((Fn)(*(void***)object)[slot])(object, arg);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		InterlockedIncrement64(&g_faults);
		return nullptr;
	}
}

bool CallBool(void* object, int slot, bool fallback) {
	if (!object) return fallback;
	__try {
		typedef bool (__thiscall* Fn)(void*);
		return ((Fn)(*(void***)object)[slot])(object);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		InterlockedIncrement64(&g_faults);
		return fallback;
	}
}

bool CallBoolIntPtr(void* object, int slot, int arg, void* out) {
	if (!object) return false;
	__try {
		typedef bool (__thiscall* Fn)(void*, int, void*);
		return ((Fn)(*(void***)object)[slot])(object, arg, out);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		InterlockedIncrement64(&g_faults);
		return false;
	}
}

bool ReadBytes(const void* base, int offset, void* out, size_t size) {
	if (!base || offset < 0) return false;
	__try {
		std::memcpy(out, (const unsigned char*)base + offset, size);
		return true;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		InterlockedIncrement64(&g_faults);
		return false;
	}
}

// Copies a game-owned C string into buffer, bounded and guarded.
bool CopyString(const char* source, char* buffer, size_t size) {
	if (!source || size == 0) return false;
	__try {
		size_t i = 0;
		for (; i + 1 < size && source[i]; ++i) buffer[i] = source[i];
		buffer[i] = '\0';
		return true;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		InterlockedIncrement64(&g_faults);
		buffer[0] = '\0';
		return false;
	}
}

int GuardedFindNetvar(const ClientClass* classes, const char* table, const char* prop) {
	__try {
		return FindNetvarOffset(classes, table, prop);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		InterlockedIncrement64(&g_faults);
		return -1;
	}
}

int GuardedCountClasses(const ClientClass* classes) {
	__try {
		return CountClientClasses(classes);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		InterlockedIncrement64(&g_faults);
		return 0;
	}
}

template <typename T>
bool ReadField(const void* entity, FieldId id, T& out) {
	return g_fields[id].offset >= 0 && ReadBytes(entity, g_fields[id].offset, &out, sizeof(T));
}

bool ReadVector(const void* vectorPtr, double out[3]) {
	float v[3];
	if (!ReadBytes(vectorPtr, 0, v, sizeof(v))) return false;
	for (int i = 0; i < 3; ++i) out[i] = v[i];
	return true;
}

void* EntityAt(int index) {
	return CallPtrInt(g_entityList, Slot::EntityList_GetClientEntity, index);
}

int HandleToIndex(unsigned int handle) {
	if (handle == kInvalidEntityHandle) return -1;
	const int index = (int)(handle & kEntityHandleIndexMask);
	return index > 0 ? index : -1;
}

} // namespace

bool Engine_Init() {
	if (g_ready) return true;

	HMODULE clientDll = GetModuleHandleW(L"client.dll");
	if (!clientDll) return false;
	auto createInterface = (CreateInterfaceFn)GetProcAddress(clientDll, "CreateInterface");
	if (!createInterface) return false;
	if (!g_client) g_client = createInterface("VClient017", nullptr);
	if (!g_entityList) g_entityList = createInterface("VClientEntityList003", nullptr);
	if (!g_client || !g_entityList) return false;

	g_classes = (const ClientClass*)CallPtr(g_client, Slot::Client_GetAllClasses);
	if (!g_classes) return false;
	g_classCount = GuardedCountClasses(g_classes);

	const GameProfile& profile = GameProfile_Current();
	g_fields[kTeam] = { "DT_BaseEntity", "m_iTeamNum", -1 };
	g_fields[kHealth] = { "DT_BasePlayer", "m_iHealth", -1 };
	g_fields[kLifeState] = { "DT_BasePlayer", "m_lifeState", -1 };
	g_fields[kObserverTarget] = { "DT_BasePlayer", "m_hObserverTarget", -1 };
	g_fields[kObserverMode] = { "DT_BasePlayer", "m_iObserverMode", -1 };
	g_fields[kEyePitch] = { profile.eyeAnglesTable, profile.eyePitchProp, -1 };
	g_fields[kEyeYaw] = { profile.eyeAnglesTable, profile.eyeYawProp, -1 };
	for (Field& field : g_fields) {
		if (field.table && field.prop) field.offset = GuardedFindNetvar(g_classes, field.table, field.prop);
	}

	g_ready = true;
	return true;
}

bool Engine_Ready() { return g_ready; }

ViewPose View_Game() {
	ViewPose pose;
	for (int i = 0; i < 3; ++i) {
		pose.origin[i] = g_Hook_VClient_RenderView.GameCameraOrigin[i];
		pose.angles[i] = g_Hook_VClient_RenderView.GameCameraAngles[i];
	}
	pose.fov = g_Hook_VClient_RenderView.GameCameraFov;
	return pose;
}

ViewPose View_Rendered() {
	ViewPose pose;
	for (int i = 0; i < 3; ++i) {
		pose.origin[i] = g_Hook_VClient_RenderView.LastCameraOrigin[i];
		pose.angles[i] = g_Hook_VClient_RenderView.LastCameraAngles[i];
	}
	pose.fov = g_Hook_VClient_RenderView.LastCameraFov;
	return pose;
}

DemoState Demo_State() {
	DemoState state;
	if (WrpGlobals* globals = g_Hook_VClient_RenderView.GetGlobals())
		state.intervalPerTick = globals->interval_per_tick_get();
	if (!g_VEngineClient || !g_VEngineClient->IsPlayingDemo()) return state;
	state.playing = true;
	state.paused = g_VEngineClient->IsPaused();
	if (WrpVEngineClientDemoInfoEx* demo = g_VEngineClient->GetDemoInfoEx()) {
		state.tick = demo->GetDemoPlaybackTick();
		state.startTick = demo->GetDemoPlaybackStartTick();
		state.totalTicks = demo->GetDemoPlaybackTotalTicks();
		state.timescale = demo->GetDemoPlaybackTimeScale();
	}
	return state;
}

std::string Engine_MapName() {
	if (!g_VEngineClient || !g_VEngineClient->IsInGame()) return std::string();
	std::string name = g_VEngineClient->GetLevelName() ? g_VEngineClient->GetLevelName() : "";
	const size_t slash = name.find_last_of("/\\");
	if (slash != std::string::npos) name.erase(0, slash + 1);
	const size_t dot = name.rfind('.');
	if (dot != std::string::npos) name.erase(dot);
	return name;
}

int Players_Snapshot(std::vector<PlayerState>& out) {
	out.clear();
	if (!Engine_Init() || !g_VEngineClient || !g_VEngineClient->IsInGame()) return 0;

	int maxClients = 0;
	if (WrpGlobals* globals = g_Hook_VClient_RenderView.GetGlobals()) maxClients = globals->maxclients_get();
	if (maxClients <= 0 || maxClients > 255) maxClients = 64;

	void* engine = g_VEngineClient->GetVEngineClient_css();
	const GameProfile& profile = GameProfile_Current();

	for (int index = 1; index <= maxClients; ++index) {
		void* entity = EntityAt(index);
		if (!entity) continue;

		PlayerState player;
		player.index = index;

		char info[kPlayerInfoBufferSize] = {};
		if (engine && CallBoolIntPtr(engine, Slot::EngineClient_GetPlayerInfo, index, info)) {
			const PlayerInfoPrefix* prefix = (const PlayerInfoPrefix*)info;
			char name[sizeof(prefix->name) + 1] = {};
			std::memcpy(name, prefix->name, sizeof(prefix->name));
			player.name = name;
			player.userId = prefix->userID;
			player.bot = prefix->fakeplayer;
			player.hltv = prefix->ishltv;
		}

		if (void* networkable = CallPtr(entity, Slot::Unknown_GetClientNetworkable)) {
			player.dormant = CallBool(networkable, Slot::Networkable_IsDormant, true);
			if (const ClientClass* cls = (const ClientClass*)CallPtr(networkable, Slot::Networkable_GetClientClass)) {
				const char* networkName = nullptr;
				char nameBuffer[64];
				if (ReadBytes(cls, (int)offsetof(ClientClass, m_pNetworkName), &networkName, sizeof(networkName))
					&& CopyString(networkName, nameBuffer, sizeof(nameBuffer)))
					player.className = nameBuffer;
			}
		}

		ReadField(entity, kTeam, player.team);
		player.teamName = profile.teamName(player.team);
		ReadField(entity, kHealth, player.health);
		unsigned char lifeState = 1;
		ReadField(entity, kLifeState, lifeState);
		player.alive = lifeState == kLifeAlive && player.health > 0;

		ReadVector(CallPtr(entity, Slot::Entity_GetAbsOrigin), player.origin);
		float pitch = 0, yaw = 0;
		if (ReadField(entity, kEyePitch, pitch) && ReadField(entity, kEyeYaw, yaw)) {
			player.eyeAngles[0] = pitch;
			player.eyeAngles[1] = yaw;
		} else {
			ReadVector(CallPtr(entity, Slot::Entity_GetAbsAngles), player.eyeAngles);
		}

		out.push_back(player);
	}
	return (int)out.size();
}

int Players_LocalIndex() {
	if (!g_VEngineClient || !g_VEngineClient->IsInGame()) return -1;
	const int index = g_VEngineClient->GetLocalPlayer();
	return index > 0 ? index : -1;
}

int Players_ObservedIndex() {
	const int local = Players_LocalIndex();
	if (local < 0 || !Engine_Init()) return -1;
	unsigned int handle = kInvalidEntityHandle;
	if (!ReadField(EntityAt(local), kObserverTarget, handle)) return -1;
	return HandleToIndex(handle);
}

int Players_ObserverMode() {
	const int local = Players_LocalIndex();
	if (local < 0 || !Engine_Init()) return 0;
	int mode = 0;
	ReadField(EntityAt(local), kObserverMode, mode);
	return mode;
}

EngineDiagnostics Engine_Diagnostics() {
	EngineDiagnostics d;
	d.ready = g_ready;
	d.classCount = g_classCount;
	for (const Field& field : g_fields) {
		if (!field.table || !field.prop) continue;
		++d.fieldsWanted;
		if (field.offset >= 0) ++d.fieldsResolved;
	}
	d.faults = (unsigned long long)g_faults;
	return d;
}

void Engine_LogFields() {
	if (!Filmmaker::MvmDebugLog_Active()) return;
	const bool ready = Engine_Init();
	Filmmaker::MvmDebugLog_Linef("engine", "ready=%d classes=%d profile=\"%s\" faults=%llu", ready ? 1 : 0,
		g_classCount, GameProfile_Current().name, (unsigned long long)g_faults);
	for (const Field& field : g_fields) {
		if (field.table && field.prop)
			Filmmaker::MvmDebugLog_Linef("engine.field", "%s.%s offset=%d", field.table, field.prop, field.offset);
	}
}

void Engine_PrintFields() {
	if (!Engine_Init()) {
		advancedfx::Message("engine: not ready (client.dll interfaces or class list missing)\n");
		return;
	}
	advancedfx::Message("engine: %d client classes, profile \"%s\"\n", g_classCount, GameProfile_Current().name);
	for (const Field& field : g_fields) {
		if (!field.table || !field.prop) continue;
		if (field.offset >= 0)
			advancedfx::Message("  %s.%s = 0x%x\n", field.table, field.prop, field.offset);
		else
			advancedfx::Message("  %s.%s NOT FOUND\n", field.table, field.prop);
	}
}

} // namespace SourceFilmmaker
