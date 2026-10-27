#include "stdafx.h"

#include "SourceEngine.h"
#include "SourceConCommand.h"
#include "SourceGameProfile.h"
#include "SourceNetvars.h"
#include "SourceSdk2013.h"

#include "RenderView.h"
#include "WrpGlobals.h"
#include "WrpVEngineClient.h"

#include <AfxHookSource2/Filmmaker/Debug/MvmDebugLog.h>
#include <Shared/AfxConsole.h>
#include <Shared/FovScaling.h>

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
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
void* g_engineVgui = nullptr;
const unsigned char* g_hltvCamera = nullptr; // C_HLTVCamera, found from spec_mode (FindHltvCamera)
int g_hltvTries = 0;
const ClientClass* g_classes = nullptr;
int g_classCount = 0;
bool g_ready = false;
volatile LONG64 g_faults = 0;

enum FieldId {
	kTeam, kHealth, kLifeState, kObserverTarget, kObserverMode, kEyePitch, kEyeYaw,
	// The follow camera's targets (step 11): links between entities, and a player's stance.
	kOwnerEntity, kWeaponOwner, kThrower, kActiveWeapon, kFlags, kPlayerRagdoll, kRagdollPlayer,
	kFieldCount
};

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

// The first call in code[0, scan) to a function that is only `lea rax, [rip + x]; ret` (an accessor
// returning a global), and that global, when both lie in [lo, hi).
const unsigned char* GuardedFollowAccessor(const unsigned char* code, size_t scan, uintptr_t lo, uintptr_t hi) {
	__try {
		for (size_t i = 0; i + 5 <= scan; ++i) {
			if (code[i] != 0xE8) continue;
			int32_t rel;
			std::memcpy(&rel, code + i + 1, 4);
			const unsigned char* fn = code + i + 5 + rel;
			if ((uintptr_t)fn < lo || (uintptr_t)fn + 8 > hi) continue;
			if (fn[0] != 0x48 || fn[1] != 0x8D || fn[2] != 0x05 || fn[7] != 0xC3) continue;
			int32_t disp;
			std::memcpy(&disp, fn + 3, 4);
			const unsigned char* global = fn + 7 + disp;
			if ((uintptr_t)global >= lo && (uintptr_t)global + 0x40 <= hi) return global;
		}
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		InterlockedIncrement64(&g_faults);
	}
	return nullptr;
}

// SourceTV's camera: spec_mode's callback starts with `HLTVCamera()->...`, and HLTVCamera() returns
// the address of the client's one C_HLTVCamera.
const unsigned char* FindHltvCamera() {
	HMODULE client = GetModuleHandleW(L"client.dll");
	const CommandCallback specMode = ConCommand_Callback("spec_mode");
	if (!client || !specMode) return nullptr;
	const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)client;
	const IMAGE_NT_HEADERS* nt = (const IMAGE_NT_HEADERS*)((const unsigned char*)client + dos->e_lfanew);
	const uintptr_t lo = (uintptr_t)client, hi = lo + nt->OptionalHeader.SizeOfImage;
	if ((uintptr_t)specMode < lo || (uintptr_t)specMode + 0x100 > hi) return nullptr;
	const unsigned char* camera = GuardedFollowAccessor((const unsigned char*)specMode, 0x100, lo, hi);
	if (Filmmaker::MvmDebugLog_Active())
		Filmmaker::MvmDebugLog_Linef("engine", "SourceTV camera: spec_mode at client.dll+0x%llx, camera %s client.dll+0x%llx",
			(unsigned long long)((uintptr_t)specMode - lo), camera ? "at" : "NOT FOUND near",
			(unsigned long long)(camera ? (uintptr_t)camera - lo : 0));
	return camera;
}

struct HltvState {
	bool valid = false;
	int mode = 0;
	int target = -1;
	int cameraMan = 0;
};

HltvState ReadHltv() {
	HltvState s;
	if (!g_hltvCamera && g_hltvTries < 600) {
		++g_hltvTries;
		g_hltvCamera = FindHltvCamera();
	}
	int mode = 0, target = 0, cameraMan = 0;
	if (!g_hltvCamera || !ReadBytes(g_hltvCamera, kHltvCameraMode, &mode, 4) || !ReadBytes(g_hltvCamera, kHltvCameraTarget, &target, 4)
		|| !ReadBytes(g_hltvCamera, kHltvCameraMan, &cameraMan, 4))
		return s;
	if (mode < 0 || mode > kObserverModeLast || target < 0 || target > 255) return s; // not what it should be: unused
	s.valid = true;
	s.mode = mode;
	s.target = target > 0 ? target : -1;
	s.cameraMan = cameraMan;
	return s;
}

bool SourceTvPlayback() { return g_VEngineClient && g_VEngineClient->IsInGame() && g_VEngineClient->IsHLTV(); }

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
	g_fields[kOwnerEntity] = { "DT_BaseEntity", "m_hOwnerEntity", -1 };
	g_fields[kWeaponOwner] = { "DT_BaseCombatWeapon", "m_hOwner", -1 };
	g_fields[kThrower] = { "DT_BaseGrenade", "m_hThrower", -1 };
	g_fields[kActiveWeapon] = { "DT_BaseCombatCharacter", "m_hActiveWeapon", -1 };
	g_fields[kFlags] = { "DT_BasePlayer", "m_fFlags", -1 };
	g_fields[kPlayerRagdoll] = { profile.ragdollTable, profile.ragdollProp, -1 };
	g_fields[kRagdollPlayer] = { profile.ragdollPlayerTable, profile.ragdollPlayerProp, -1 };
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

namespace {

int HighestEntityIndex() {
	const int highest = (int)(intptr_t)CallPtr(g_entityList, Slot::EntityList_GetHighestEntityIndex);
	return highest > 0 && highest < 8192 ? highest : 0;
}

// A field holding another entity's handle, as that entity's index.
int LinkedIndex(const void* entity, FieldId id) {
	unsigned int handle = kInvalidEntityHandle;
	return ReadField(entity, id, handle) ? HandleToIndex(handle) : -1;
}

// Network class names are only read for entities whose class carries the field: a read of a field
// another class networks would be garbage, so links are read where the class table has them.
bool FillEntity(int index, void* entity, EntityState& e) {
	e.index = index;
	if (const unsigned int* handle = (const unsigned int*)CallPtr(entity, Slot::Handle_GetRefEHandle)) ReadBytes(handle, 0, &e.handle, 4);
	if (void* networkable = CallPtr(entity, Slot::Unknown_GetClientNetworkable)) {
		e.dormant = CallBool(networkable, Slot::Networkable_IsDormant, true);
		if (const ClientClass* cls = (const ClientClass*)CallPtr(networkable, Slot::Networkable_GetClientClass)) {
			const char* networkName = nullptr;
			char nameBuffer[64];
			if (ReadBytes(cls, (int)offsetof(ClientClass, m_pNetworkName), &networkName, sizeof(networkName))
				&& CopyString(networkName, nameBuffer, sizeof(nameBuffer)))
				e.className = nameBuffer;
		}
	}
	if (e.className.empty()) return false;
	ReadVector(CallPtr(entity, Slot::Entity_GetAbsOrigin), e.origin);
	ReadVector(CallPtr(entity, Slot::Entity_GetAbsAngles), e.angles);
	ReadField(entity, kTeam, e.team);
	e.ownerIndex = LinkedIndex(entity, kOwnerEntity);
	const GameProfile& profile = GameProfile_Current();
	const std::string& c = e.className;
	if (profile.isPlayerClass && profile.isPlayerClass(c.c_str())) {
		ReadField(entity, kHealth, e.health);
		unsigned char lifeState = 1;
		ReadField(entity, kLifeState, lifeState);
		e.alive = lifeState == kLifeAlive && e.health > 0;
		ReadField(entity, kFlags, e.flags);
		for (int i = 0; i < 3; ++i) e.eye[i] = e.origin[i];
		e.eye[2] += (e.flags & kFlagDucking) ? profile.duckEyeHeight : profile.eyeHeight;
		e.activeWeaponIndex = LinkedIndex(entity, kActiveWeapon);
		e.ragdollIndex = LinkedIndex(entity, kPlayerRagdoll);
	} else if (profile.isRagdollClass && profile.isRagdollClass(c.c_str())) {
		e.ragdollPlayerIndex = LinkedIndex(entity, kRagdollPlayer);
	} else if (profile.isGrenadeClass && profile.isGrenadeClass(c.c_str())) {
		e.throwerIndex = LinkedIndex(entity, kThrower);
	} else if (profile.isWeaponClass && profile.isWeaponClass(c.c_str())) {
		e.weaponOwnerIndex = LinkedIndex(entity, kWeaponOwner);
	}
	return true;
}

} // namespace

int Entities_Snapshot(std::vector<EntityState>& out, bool includeDormant) {
	out.clear();
	if (!Engine_Init() || !g_VEngineClient || !g_VEngineClient->IsInGame()) return 0;
	const int highest = HighestEntityIndex();
	for (int index = 1; index <= highest; ++index) {
		void* entity = EntityAt(index);
		if (!entity) continue;
		EntityState e;
		if (!FillEntity(index, entity, e) || (e.dormant && !includeDormant)) continue;
		out.push_back(e);
	}
	return (int)out.size();
}

bool Entity_Get(int index, EntityState& out) {
	out = EntityState();
	if (index <= 0 || !Engine_Init() || !g_VEngineClient || !g_VEngineClient->IsInGame()) return false;
	void* entity = EntityAt(index);
	return entity && FillEntity(index, entity, out);
}

namespace {

struct Watch {
	std::string filter; // lower case; empty: off
	std::vector<std::pair<int, unsigned int>> seen; // index, handle
	unsigned frames = 0;
} g_watch;

bool ClassMatches(const std::string& className, const std::string& lowerFilter) {
	std::string lower = className;
	for (char& ch : lower) ch = (char)tolower((unsigned char)ch);
	return lower.find(lowerFilter) != std::string::npos;
}

} // namespace

void Entities_Watch(const char* classFilter) {
	g_watch = Watch();
	std::string f = classFilter ? classFilter : "";
	if (0 == _stricmp(f.c_str(), "off")) f.clear();
	for (char& ch : f) ch = (char)tolower((unsigned char)ch);
	g_watch.filter = f;
	advancedfx::Message("entities watch: %s%s\n", f.empty() ? "off" : f.c_str(),
		f.empty() || Filmmaker::MvmDebugLog_Active() ? "" : " (mvm_debug is off: start it to see the log)");
}

void Entities_WatchFrame() {
	if (g_watch.filter.empty() || !Filmmaker::MvmDebugLog_Active()) return;
	if (g_watch.filter == "eye") {
		// The eye height: in first person the game's view sits at the spectated player's eyes.
		static int lastIndex = -1, lastDuck = -1;
		static double lastHeight = -1e9;
		const int observed = Players_ObservedIndex();
		EntityState e;
		if (observed <= 0 || Players_ObserverMode() != 4 || !Entity_Get(observed, e)) return;
		const double height = View_Game().origin[2] - e.origin[2];
		const int duck = (e.flags & kFlagDucking) ? 1 : 0;
		if (observed == lastIndex && duck == lastDuck && std::fabs(height - lastHeight) < 0.5) return;
		Filmmaker::MvmDebugLog_Linef("entities.eye", "tick=%d index=%d ducking=%d onground=%d flags=0x%x height=%.2f", Demo_State().tick,
			observed, duck, (e.flags & kFlagOnGround) ? 1 : 0, e.flags, height);
		lastIndex = observed;
		lastDuck = duck;
		lastHeight = height;
		return;
	}
	std::vector<EntityState> now;
	Entities_Snapshot(now, false);
	const int tick = Demo_State().tick;
	std::vector<std::pair<int, unsigned int>> current;
	for (const EntityState& e : now) {
		if (!ClassMatches(e.className, g_watch.filter)) continue;
		current.emplace_back(e.index, e.handle);
		if (std::find(g_watch.seen.begin(), g_watch.seen.end(), std::make_pair(e.index, e.handle)) != g_watch.seen.end()) continue;
		Filmmaker::MvmDebugLog_Linef("entities.watch", "appear tick=%d index=%d handle=%08x class=%s origin=(%.1f %.1f %.1f) owner=%d "
			"thrower=%d weaponowner=%d player=%d team=%d", tick, e.index, e.handle, e.className.c_str(), e.origin[0], e.origin[1],
			e.origin[2], e.ownerIndex, e.throwerIndex, e.weaponOwnerIndex, e.ragdollPlayerIndex, e.team);
	}
	for (const auto& s : g_watch.seen)
		if (std::find(current.begin(), current.end(), s) == current.end())
			Filmmaker::MvmDebugLog_Linef("entities.watch", "gone tick=%d index=%d handle=%08x", tick, s.first, s.second);
	g_watch.seen.swap(current);
}

void Engine_PrintClasses(const char* filter) {
	if (!Engine_Init()) {
		advancedfx::Message("engine: not ready (client.dll interfaces or class list missing)\n");
		return;
	}
	std::string want = filter ? filter : "";
	for (char& ch : want) ch = (char)tolower((unsigned char)ch);
	std::string line;
	int shown = 0, guard = 0;
	for (const ClientClass* cls = g_classes; cls && guard < 4096; ++guard) {
		const char* networkName = nullptr;
		char name[64];
		if (!ReadBytes(cls, (int)offsetof(ClientClass, m_pNetworkName), &networkName, sizeof(networkName))
			|| !CopyString(networkName, name, sizeof(name)))
			break;
		std::string lower = name;
		for (char& ch : lower) ch = (char)tolower((unsigned char)ch);
		if (want.empty() || lower.find(want) != std::string::npos) {
			line += line.empty() ? "  " : " ";
			line += name;
			if (line.size() > 100) {
				advancedfx::Message("%s\n", line.c_str());
				line.clear();
			}
			++shown;
		}
		const ClientClass* next = nullptr;
		if (!ReadBytes(cls, (int)offsetof(ClientClass, m_pNext), &next, sizeof(next))) break;
		cls = next;
	}
	if (!line.empty()) advancedfx::Message("%s\n", line.c_str());
	advancedfx::Message("engine: %d of %d client classes\n", shown, g_classCount);
}

void Entities_Print(bool includeDormant, const char* classFilter) {
	std::vector<EntityState> entities;
	if (!Entities_Snapshot(entities, includeDormant)) {
		advancedfx::Message("entities: none (not in game, or the engine adapter is not ready)\n");
		return;
	}
	std::string filter = classFilter ? classFilter : "";
	for (char& ch : filter) ch = (char)tolower((unsigned char)ch);
	int shown = 0;
	advancedfx::Message("    #  handle    class                     origin (x y z)              angles (p y r)        team  links\n");
	for (const EntityState& e : entities) {
		std::string lower = e.className;
		for (char& ch : lower) ch = (char)tolower((unsigned char)ch);
		if (!filter.empty() && lower.find(filter) == std::string::npos) continue;
		char links[160] = "";
		size_t at = 0;
		auto link = [&](const char* name, int value) {
			if (value >= 0 && at < sizeof(links)) at += (size_t)std::snprintf(links + at, sizeof(links) - at, " %s %d", name, value);
		};
		link("owner", e.ownerIndex);
		link("weaponowner", e.weaponOwnerIndex);
		link("thrower", e.throwerIndex);
		link("weapon", e.activeWeaponIndex);
		link("ragdoll", e.ragdollIndex);
		link("player", e.ragdollPlayerIndex);
		if (e.health || e.flags) {
			if (at < sizeof(links))
				at += (size_t)std::snprintf(links + at, sizeof(links) - at, " hp %d%s%s", e.health, e.alive ? "" : " dead",
					(e.flags & kFlagDucking) ? " ducking" : "");
		}
		advancedfx::Message("%5d  %08x  %-24.24s (%8.1f %8.1f %8.1f)  (%6.1f %6.1f %6.1f)  %4d %s%s\n", e.index, e.handle,
			e.className.c_str(), e.origin[0], e.origin[1], e.origin[2], e.angles[0], e.angles[1], e.angles[2], e.team, links,
			e.dormant ? " dormant" : "");
		++shown;
	}
	advancedfx::Message("entities: %d shown of %d%s, highest index %d\n", shown, (int)entities.size(),
		includeDormant ? " (dormant included)" : "", HighestEntityIndex());
}

int Players_LocalIndex() {
	if (!g_VEngineClient || !g_VEngineClient->IsInGame()) return -1;
	const int index = g_VEngineClient->GetLocalPlayer();
	return index > 0 ? index : -1;
}

int Players_ObservedIndex() {
	if (SourceTvPlayback()) return ReadHltv().target;
	const int local = Players_LocalIndex();
	if (local < 0 || !Engine_Init()) return -1;
	unsigned int handle = kInvalidEntityHandle;
	if (!ReadField(EntityAt(local), kObserverTarget, handle)) return -1;
	return HandleToIndex(handle);
}

int Players_ObserverMode() {
	if (SourceTvPlayback()) return ReadHltv().mode;
	const int local = Players_LocalIndex();
	if (local < 0 || !Engine_Init()) return 0;
	int mode = 0;
	ReadField(EntityAt(local), kObserverMode, mode);
	return mode;
}

bool Engine_SourceTvPlayback() { return SourceTvPlayback(); }

std::string Engine_SpectatorCameraText() {
	if (!SourceTvPlayback()) return "the local player's observer fields";
	const HltvState s = ReadHltv();
	HMODULE client = GetModuleHandleW(L"client.dll");
	char text[160];
	if (!s.valid) {
		std::snprintf(text, sizeof(text), "SourceTV camera NOT FOUND (%d tries)", g_hltvTries);
	} else {
		std::snprintf(text, sizeof(text), "SourceTV camera at client.dll+0x%llx: mode %d, target %d%s",
			(unsigned long long)((uintptr_t)g_hltvCamera - (uintptr_t)client), s.mode, s.target,
			s.cameraMan > 0 ? ", following a camera man" : "");
	}
	return text;
}

bool Engine_GameUiVisible() {
	if (g_VEngineClient && g_VEngineClient->Con_IsVisible()) return true;
	if (!g_engineVgui) {
		HMODULE engineDll = GetModuleHandleW(L"engine.dll");
		auto createInterface = engineDll ? (CreateInterfaceFn)GetProcAddress(engineDll, "CreateInterface") : nullptr;
		if (createInterface) g_engineVgui = createInterface("VEngineVGui001", nullptr);
	}
	return CallBool(g_engineVgui, Slot::EngineVGui_IsGameUIVisible, false);
}

bool Engine_ViewMatrixFov(double& horizontal, double& vertical) {
	if (!g_VEngineClient || !g_VEngineClient->IsInGame()) return false;
	const SOURCESDK::VMatrix& m = g_VEngineClient->WorldToScreenMatrix();
	const auto length = [&m](int row) {
		return std::sqrt((double)m.m[row][0] * m.m[row][0] + (double)m.m[row][1] * m.m[row][1] + (double)m.m[row][2] * m.m[row][2]);
	};
	// Row 0 is the view's right over tan(horizontal / 2), row 1 its up over tan(vertical / 2), row 3
	// its forward: the depth the others are divided by.
	const double x = length(0), y = length(1), w = length(3);
	if (!(x > 0) || !(y > 0) || !(w > 0)) return false;
	constexpr double kDeg = 180.0 / 3.14159265358979323846;
	horizontal = 2.0 * std::atan(w / x) * kDeg;
	vertical = 2.0 * std::atan(w / y) * kDeg;
	return std::isfinite(horizontal) && std::isfinite(vertical);
}

double View_ScreenFov(double fov, double width, double height) { return AlienSwarm_FovScaling(width, height, fov); }

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
