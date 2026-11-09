#define NOMINMAX
#include "TakesServer.h"

#include "../Debug/MvmDebugLog.h"
#include "../../Globals.h"
#include "../../SchemaSystem.h"
#include "../../ClientEntitySystem.h"

#include <Windows.h>
#include <cstdio>
#include <cstring>
#include <map>
#include "../../../Deps/release/Detours/src/detours.h"

namespace Filmmaker::Takes::Server {
namespace {

unsigned char *At(void *p, ptrdiff_t off) { return static_cast<unsigned char *>(p) + off; }
template <class T> T Read(void *p, ptrdiff_t off) { return *reinterpret_cast<T *>(At(p, off)); }
template <class T> void Write(void *p, ptrdiff_t off, const T &v) { *reinterpret_cast<T *>(At(p, off)) = v; }

// Offsets of the CCSGOUserCmd the server passes around: the wrapper's command number and the
// CSGOUserCmdPB it holds. Traced from the bot_mimic copy at server.dll+0xb48720 (2026-11-05).
constexpr ptrdiff_t kCmdNumber = 0x8;
constexpr ptrdiff_t kCmdMessage = 0x10;
// protobuf MessageLite vtable: ByteSizeLong and _InternalSerialize. Their order follows the library,
// not the game, and it is confirmed by the ToArray helper at server.dll+0x1128f80.
constexpr int kVtByteSizeLong = 7;
constexpr int kVtInternalSerialize = 11;
// CCSGOUserCmd keeps its buttons twice: in the message and in three plain fields next to it. On the
// way to the movement code the plain fields are written over the message (+0x30, server.dll+0x2e3290),
// so a command filled through the message alone arrives with no buttons at all: no jump, no attack.
// This slot is the copy the other way, the same call the game's own bot_mimic copy ends with.
constexpr int kVtCmdButtonsFromMessage = 7;
// CPlayerPawnComponent -> its pawn (both CPlayer_WeaponServices and the movement services).
constexpr ptrdiff_t kComponentPawn = 0x38;
// CEntityInstance -> CEntityIdentity -> entity handle.
constexpr ptrdiff_t kIdentityHandle = 0x10;

struct Api {
	void *mimic = nullptr;
	void *botUpdate = nullptr;
	bool (*parseFromString)(void *message, const void *msvcString) = nullptr;
	void *(*entityByIndex)(void *system, int index) = nullptr;
	void *(*playerBySlot)(int slot) = nullptr;
	unsigned char **globals = nullptr; // CGlobalVars**
	void **entitySystem = nullptr;     // CGameEntitySystem**
	void (*destroyWeapons)(void *weaponServices, bool all) = nullptr;
	void (*setModel)(void *entity, const char *model) = nullptr;
	void (*setAbsOrigin)(void *entity, const float *pos) = nullptr;
	void (*removeEntity)(void *entity) = nullptr;
	void (*switchToPlayerPawn)(void *controller) = nullptr;
	void (*tryRespawn)(void *pawn) = nullptr;
	bool (*canRespawn)(void *rules, void *pawn) = nullptr;
	void (*joinTeam)(void *controller, int team, bool arg) = nullptr;
	ptrdiff_t slotTeleport = 0, slotGive = 0, slotEye = 0, slotAfterCmd = 0, slotStateChanged = 0;
	ptrdiff_t weaponServicesAt = 0, itemServicesAt = 0, movementServicesAt = 0, preventDeployAt = 0;
} api;

struct Offsets {
	ptrdiff_t entityIdentity = 0, designerName = 0, targetName = 0;
	ptrdiff_t health = 0, team = 0, lifeState = 0, absVelocity = 0, bodyComponent = 0;
	ptrdiff_t sceneNode = 0, absOrigin = 0;
	ptrdiff_t controllerPawn = 0, steamId = 0, playerName = 0, playerPawn = 0, pawnController = 0;
	ptrdiff_t myWeapons = 0, activeWeapon = 0;
	ptrdiff_t armor = 0, helmet = 0, eyeAngles = 0;
	ptrdiff_t moneyServices = 0, account = 0, pendingTeam = 0;
	ptrdiff_t ownerEntity = 0, clip1 = 0, numSpawns = 0, modelState = 0, modelName = 0, flags = 0, xuidLow = 0, xuidHigh = 0;
	ptrdiff_t gameRulesProxy = 0, warmupPeriod = 0, freezePeriod = 0, gameRestart = 0, restartRoundTime = 0, roundStartTime = 0;
} off;

bool g_resolved = false, g_resolvedOk = false;
std::string g_resolveError;
Hooks g_hooks;
bool g_installed = false;
void *g_gameRules = nullptr;
int g_gameRulesCheckedAtTick = -1000000;

using MimicFn = bool (*)(void *, void *);
using AfterCmdFn = void (*)(void *, void *);
using BotUpdateFn = void (*)(void *);
MimicFn g_origMimic = nullptr;
AfterCmdFn g_origAfterCmd = nullptr;
BotUpdateFn g_origBotUpdate = nullptr;
// CCSBot -> its pawn, as the command builder at server.dll+0x2be8a0 reads it.
constexpr ptrdiff_t kBotPawn = 0x18;

uintptr_t Rel32(size_t at, int immOffset, int instrSize) {
	return at + instrSize + *reinterpret_cast<int32_t *>(at + immOffset);
}

bool Live(const char *cls, const char *field, ptrdiff_t &out) {
	if (schemaLiveOffset("server.dll", cls, field, &out)) return true;
	if (g_resolveError.empty()) g_resolveError = std::string("schema ") + cls + "::" + field;
	return false;
}

bool ResolveOffsets() {
	bool ok = true;
	ok = Live("CEntityInstance", "m_pEntity", off.entityIdentity) && ok;
	ok = Live("CEntityIdentity", "m_designerName", off.designerName) && ok;
	ok = Live("CEntityIdentity", "m_name", off.targetName) && ok;
	ok = Live("CBaseEntity", "m_iHealth", off.health) && ok;
	ok = Live("CBaseEntity", "m_iTeamNum", off.team) && ok;
	ok = Live("CBaseEntity", "m_lifeState", off.lifeState) && ok;
	ok = Live("CBaseEntity", "m_vecAbsVelocity", off.absVelocity) && ok;
	ok = Live("CBaseEntity", "m_CBodyComponent", off.bodyComponent) && ok;
	ok = Live("CBodyComponent", "m_pSceneNode", off.sceneNode) && ok;
	ok = Live("CGameSceneNode", "m_vecAbsOrigin", off.absOrigin) && ok;
	ok = Live("CBasePlayerController", "m_hPawn", off.controllerPawn) && ok;
	ok = Live("CBasePlayerController", "m_steamID", off.steamId) && ok;
	ok = Live("CBasePlayerController", "m_iszPlayerName", off.playerName) && ok;
	ok = Live("CCSPlayerController", "m_hPlayerPawn", off.playerPawn) && ok;
	ok = Live("CBasePlayerPawn", "m_hController", off.pawnController) && ok;
	ok = Live("CPlayer_WeaponServices", "m_hMyWeapons", off.myWeapons) && ok;
	ok = Live("CPlayer_WeaponServices", "m_hActiveWeapon", off.activeWeapon) && ok;
	ok = Live("CCSGameRulesProxy", "m_pGameRules", off.gameRulesProxy) && ok;
	ok = Live("CCSGameRules", "m_bWarmupPeriod", off.warmupPeriod) && ok;
	ok = Live("CBaseEntity", "m_hOwnerEntity", off.ownerEntity) && ok;
	ok = Live("CBaseEntity", "m_fFlags", off.flags) && ok;
	ok = Live("CCSPlayerPawnBase", "m_iNumSpawns", off.numSpawns) && ok;
	// Optional: a missing one costs one detail, not the take.
	schemaLiveOffset("server.dll", "CCSGameRules", "m_bFreezePeriod", &off.freezePeriod);
	schemaLiveOffset("server.dll", "CCSGameRules", "m_bGameRestart", &off.gameRestart);
	schemaLiveOffset("server.dll", "CCSGameRules", "m_flRestartRoundTime", &off.restartRoundTime);
	schemaLiveOffset("server.dll", "CCSGameRules", "m_fRoundStartTime", &off.roundStartTime);
	schemaLiveOffset("server.dll", "CBasePlayerWeapon", "m_iClip1", &off.clip1);
	schemaLiveOffset("server.dll", "CCSPlayerController", "m_pInGameMoneyServices", &off.moneyServices);
	schemaLiveOffset("server.dll", "CCSPlayerController", "m_iPendingTeamNum", &off.pendingTeam);
	schemaLiveOffset("server.dll", "CCSPlayerController_InGameMoneyServices", "m_iAccount", &off.account);
	schemaLiveOffset("server.dll", "CEconEntity", "m_OriginalOwnerXuidLow", &off.xuidLow);
	schemaLiveOffset("server.dll", "CEconEntity", "m_OriginalOwnerXuidHigh", &off.xuidHigh);
	schemaLiveOffset("server.dll", "CSkeletonInstance", "m_modelState", &off.modelState);
	schemaLiveOffset("server.dll", "CModelState", "m_ModelName", &off.modelName);
	schemaLiveOffset("server.dll", "CCSPlayerPawn", "m_ArmorValue", &off.armor);
	schemaLiveOffset("server.dll", "CCSPlayerPawn", "m_angEyeAngles", &off.eyeAngles);
	schemaLiveOffset("server.dll", "CCSPlayer_ItemServices", "m_bHasHelmet", &off.helmet);
	return ok;
}

bool ResolveCode(HMODULE m) {
	auto need = [&](const char *what, size_t found) {
		if (!found && g_resolveError.empty()) g_resolveError = what;
		return found;
	};
	// The bot_mimic check at the top of the bot's command builder: (pawn, cmd) -> "cmd is done".
	api.mimic = reinterpret_cast<void *>(need("bot mimic", getAddress(m,
		"40 55 56 41 57 48 81 EC 80 00 00 00 48 8B 01 48 8B F2 4C 8B F9 33 ED FF 90 ?? ?? ?? ?? 84 C0")));
	// CCSBot::Update, the AI's whole think: state machine, buying, aiming.
	api.botUpdate = reinterpret_cast<void *>(need("bot update", getAddress(m,
		"40 55 56 57 41 54 48 8D AC 24 08 FF FF FF 48 81 EC F8 01 00 00 48 8D 05 ?? ?? ?? ?? C6 81 ?? ?? ?? ?? 00")));
	// MessageLite::ParseFromString(const std::string&): clears, then parses.
	api.parseFromString = reinterpret_cast<decltype(api.parseFromString)>(need("protobuf parse", getAddress(m,
		"48 89 5C 24 10 48 89 74 24 18 55 57 41 56 48 8D 6C 24 B9 48 81 EC B0 00 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C4 "
		"48 89 45 37 48 8B 01 48 8B F2")));
	// UTIL_PlayerByIndex: gives the globals, the entity system and the index lookup.
	if (size_t at = need("player lookup", getAddress(m,
			"48 83 EC 28 48 8B 05 ?? ?? ?? ?? 48 85 C0 74 ?? 85 C9 78 ?? 3B 48 10 7D ?? 8D 51 01 48 8B 0D ?? ?? ?? ?? "
			"E8 ?? ?? ?? ?? 48 85 C0 75"))) {
		api.playerBySlot = reinterpret_cast<decltype(api.playerBySlot)>(at);
		api.globals = reinterpret_cast<unsigned char **>(Rel32(at + 4, 3, 7));
		api.entitySystem = reinterpret_cast<void **>(Rel32(at + 28, 3, 7));
		api.entityByIndex = reinterpret_cast<decltype(api.entityByIndex)>(Rel32(at + 35, 1, 5));
	}
	// CBaseEntity::SetAbsOrigin, what the script API's Entity.Move calls.
	api.setAbsOrigin = reinterpret_cast<decltype(api.setAbsOrigin)>(need("set origin", getAddress(m,
		"48 89 5C 24 08 57 48 83 EC 60 48 8B 01 48 8B FA 48 8B D9 FF 90 C0")));
	// UTIL_Remove: a null check, then a tail call into the entity system.
	api.removeEntity = reinterpret_cast<decltype(api.removeEntity)>(need("remove entity", getAddress(m,
		"48 85 C9 74 ?? 48 8B D1 48 8B 0D ?? ?? ?? ?? E9 ?? ?? ?? ?? C3")));
	// What a controller's join path calls for a player who is back: the observer pawn gives way to the
	// player pawn, and the pawn's own spawn routine runs. server.dll+0xa924f0 is that join path.
	api.switchToPlayerPawn = reinterpret_cast<decltype(api.switchToPlayerPawn)>(need("switch to player pawn", getAddress(m,
		"40 53 48 83 EC 30 C7 81 ?? ?? ?? ?? 00 00 00 00 48 8B D9 C7 81 ?? ?? ?? ?? FF FF FF FF 80 B9 ?? ?? ?? ?? 00")));
	// What the script API's CSPlayerController.JoinTeam calls, with a false last argument.
	api.joinTeam = reinterpret_cast<decltype(api.joinTeam)>(need("join team", getAddress(m,
		"48 89 5C 24 08 44 88 44 24 18 55 56 57 41 54 41 55 41 56 41 57 48 8D 6C 24 90")));
	// FPlayerCanRespawn, the check the respawn routine makes first. It names itself in its fatal error.
	api.canRespawn = reinterpret_cast<decltype(api.canRespawn)>(need("respawn check", getAddress(m,
		"40 56 57 48 83 EC 48 48 8B F2 48 8B F9 48 85 D2 0F 84 ?? ?? ?? ?? 48 8B 02 48 8B CA FF 90 ?? ?? ?? ?? 84 C0")));
	api.tryRespawn = reinterpret_cast<decltype(api.tryRespawn)>(need("respawn", getAddress(m,
		"40 53 56 57 48 83 EC 50 48 8B F9 0F 29 74 24 40 48 8B D1 33 DB 48 8B 89 ?? ?? ?? ?? 0F 57 DB 45 33 C0 89 5C 24 20 0F 57 F6 E8")));
	api.destroyWeapons = reinterpret_cast<decltype(api.destroyWeapons)>(need("destroy weapons", getAddress(m,
		"48 89 5C 24 18 55 56 57 41 56 41 57 48 83 EC 70 48 83")));
	api.setModel = reinterpret_cast<decltype(api.setModel)>(need("set model", getAddress(m,
		"40 53 48 83 EC 20 48 8B D9 4C 8B C2 48 8B 0D ?? ?? ?? ?? 48 8D 54 24 40 48 8B 01 FF 50 60 48 8B 54 24 40 48 8B "
		"CB E8 ?? ?? ?? ?? 48 83 C4 20 5B C3")));
	// Slots and member offsets, read out of the code that uses them so an update that moves them
	// moves these too. Each pattern sits in a script API callback, which Valve keeps for mapmakers.
	if (size_t at = need("teleport slot", getAddress(m, "49 8B 04 24 4D 8B CF 4C 8B C6 49 8B D5 49 8B CC FF 90 ?? ?? ?? ??")))
		api.slotTeleport = *reinterpret_cast<int32_t *>(at + 18);
	if (size_t at = need("give slot", getAddress(m,
			"48 8B BD ?? ?? ?? ?? 0F 94 C0 0F B6 9F ?? ?? ?? ?? 88 87 ?? ?? ?? ?? 48 8B 8D ?? ?? ?? ?? 48 8B 01 4C 8B 90 ?? ?? ?? ??"))) {
		api.weaponServicesAt = *reinterpret_cast<int32_t *>(at + 3);
		api.preventDeployAt = *reinterpret_cast<int32_t *>(at + 13);
		api.itemServicesAt = *reinterpret_cast<int32_t *>(at + 26);
		api.slotGive = *reinterpret_cast<int32_t *>(at + 36);
	}
	if (size_t at = need("after-command slot", getAddress(m,
			"49 8B 8D ?? ?? ?? ?? 48 8D 54 24 60 48 8B 01 FF 90 ?? ?? ?? ??"))) {
		api.movementServicesAt = *reinterpret_cast<int32_t *>(at + 3);
		api.slotAfterCmd = *reinterpret_cast<int32_t *>(at + 17);
	}
	if (size_t at = need("state changed slot", getAddress(m,
			"C7 00 ?? ?? ?? ?? 48 8B 07 FF 90 ?? ?? ?? ?? 8B 45 DC 48 8B 55 D0")))
		api.slotStateChanged = *reinterpret_cast<int32_t *>(at + 11);
	// Inside the mimic function: pawn->EyePosition(&out).
	if (api.mimic) {
		auto *site = static_cast<unsigned char *>(api.mimic) + 0x98;
		if (site[0] == 0xFF && site[1] == 0x90) api.slotEye = *reinterpret_cast<int32_t *>(site + 2);
	}
	return api.mimic && api.botUpdate && api.setAbsOrigin && api.removeEntity && api.switchToPlayerPawn && api.canRespawn && api.tryRespawn && api.joinTeam && api.parseFromString && api.playerBySlot &&
	       api.destroyWeapons && api.setModel && api.slotTeleport &&
	       api.slotGive && api.slotAfterCmd && api.slotStateChanged;
}

bool ResolveGuarded(HMODULE m) {
	__try {
		const bool code = ResolveCode(m);
		const bool offsets = ResolveOffsets();
		return code && offsets;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		g_resolveError = "fault while resolving";
		return false;
	}
}

bool New_BotMimic(void *pawn, void *cmd) {
	if (g_hooks.botBuildCmd) {
		__try {
			if (g_hooks.botBuildCmd(pawn, cmd)) return true;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			MvmDebugLog_LinefAlways("takes.fault", "botBuildCmd faulted pawn=%p", pawn);
		}
	}
	return g_origMimic(pawn, cmd);
}

void New_BotUpdate(void *bot) {
	if (g_hooks.skipBotThink) {
		__try {
			if (g_hooks.skipBotThink(Read<void *>(bot, kBotPawn))) return;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			MvmDebugLog_LinefAlways("takes.fault", "skipBotThink faulted bot=%p", bot);
		}
	}
	g_origBotUpdate(bot);
}

void New_AfterCmd(void *movementServices, void *cmd) {
	g_origAfterCmd(movementServices, cmd);
	if (!g_hooks.commandRan) return;
	__try {
		g_hooks.commandRan(Read<void *>(movementServices, kComponentPawn), cmd);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		MvmDebugLog_LinefAlways("takes.fault", "commandRan faulted services=%p", movementServices);
	}
}

// The argument CEntityInstance's NetworkStateChanged takes, as the generated setters build it.
struct StateChangedData {
	int32_t flags = 1;
	int32_t pad0 = 0;
	int32_t count = 1;
	int32_t pad1 = 0;
	uint32_t *offsets = nullptr;
	int32_t allocated = 1;
	uint32_t vectorFlags = 0x40000000; // the elements are not the vector's to free
	uint64_t zero0 = 0, zero1 = 0;
	int32_t minusOne0 = -1, minusOne1 = -1, minusOne2 = -1;
	int16_t zero2 = 0;
	int16_t pad2 = 0;
	uint64_t pad3 = 0, pad4 = 0;
};

void MarkChanged(void *entity, ptrdiff_t offset) {
	if (!api.slotStateChanged) return;
	__try {
		uint32_t offsets[1] = {uint32_t(offset)};
		StateChangedData data;
		data.offsets = offsets;
		auto fn = reinterpret_cast<void (*)(void *, void *)>(Read<void **>(entity, 0)[api.slotStateChanged / 8]);
		fn(entity, &data);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		MvmDebugLog_LinefAlways("takes.fault", "state changed faulted entity=%p offset=0x%llx", entity, (long long)offset);
	}
}

} // namespace

bool Resolve() {
	if (g_resolved) return g_resolvedOk;
	HMODULE m = GetModuleHandleW(L"server.dll");
	if (!m) {
		g_resolveError = "server.dll is not loaded";
		return false;
	}
	g_resolved = true;
	g_resolvedOk = ResolveGuarded(m);
	const std::string described = DescribeResolve();
	MvmDebugLog_LinefAlways("takes.resolve", "ok=%d error=%s %s", int(g_resolvedOk), g_resolveError.c_str(), described.c_str());
	return g_resolvedOk;
}

const char *ResolveError() { return g_resolveError.c_str(); }

bool InstallHooks(const Hooks &hooks) {
	g_hooks = hooks;
	if (g_installed) return true;
	if (!Resolve()) return false;
	void *host = HostController();
	void *pawn = host ? PawnOf(host) : nullptr;
	if (!pawn) return false;
	void *services = Read<void *>(pawn, api.movementServicesAt);
	if (!services || Read<void *>(services, kComponentPawn) != pawn) {
		g_resolveError = "movement services do not point back at their pawn";
		return false;
	}
	void *afterCmd = Read<void **>(services, 0)[api.slotAfterCmd / 8];
	g_origMimic = reinterpret_cast<MimicFn>(api.mimic);
	g_origAfterCmd = reinterpret_cast<AfterCmdFn>(afterCmd);
	g_origBotUpdate = reinterpret_cast<BotUpdateFn>(api.botUpdate);
	DetourTransactionBegin();
	DetourUpdateThread(GetCurrentThread());
	DetourAttach(&reinterpret_cast<PVOID &>(g_origMimic), New_BotMimic);
	DetourAttach(&reinterpret_cast<PVOID &>(g_origAfterCmd), New_AfterCmd);
	DetourAttach(&reinterpret_cast<PVOID &>(g_origBotUpdate), New_BotUpdate);
	const LONG result = DetourTransactionCommit();
	g_installed = result == NO_ERROR;
	if (!g_installed) g_resolveError = "detour commit failed";
	MvmDebugLog_LinefAlways("takes.install", "installed=%d result=%ld afterCmd=%p mimic=%p", int(g_installed), result, afterCmd, api.mimic);
	return g_installed;
}

bool HooksInstalled() { return g_installed; }

bool Active() {
	if (!g_resolved && !Resolve()) return false;
	if (!g_resolvedOk || !api.entitySystem || !*api.entitySystem || !api.globals || !*api.globals) return false;
	return HostController() != nullptr;
}

int TickCount() {
	__try {
		return (api.globals && *api.globals) ? Read<int>(*api.globals, 0x44) : 0;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return 0;
	}
}

int MaxClients() {
	__try {
		return (api.globals && *api.globals) ? Read<int>(*api.globals, 0x10) : 0;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return 0;
	}
}

void *ControllerBySlot(int slot) {
	if (!api.playerBySlot) return nullptr;
	__try {
		return api.playerBySlot(slot);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return nullptr;
	}
}

void *HostController() {
	const int count = MaxClients();
	for (int i = 0; i < count; ++i) {
		void *c = ControllerBySlot(i);
		if (c && !ControllerIsBot(c)) return c;
	}
	return nullptr;
}

bool ControllerIsBot(void *controller) {
	__try {
		// FL_FAKECLIENT, what the controller's own connect code tests at server.dll+0xa924f0. A performer
		// has a SteamID, so that can not say who is a bot.
		return (Read<uint32_t>(controller, off.flags) & 0x100u) != 0;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return true;
	}
}

int PendingTeam(void *controller) {
	if (!off.pendingTeam) return 0;
	__try {
		return Read<uint8_t>(controller, off.pendingTeam);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return 0;
	}
}

bool JoinTeam(void *controller, int team) {
	if (!controller || !api.joinTeam) return false;
	__try {
		api.joinTeam(controller, team, false);
		MvmDebugLog_Linef("takes.team", "controller=%p asked=%d now=%d pending=%d", controller, team, Team(controller), PendingTeam(controller));
		return Team(controller) == team;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		MvmDebugLog_LinefAlways("takes.fault", "join team faulted controller=%p", controller);
		return false;
	}
}

void SetMoney(void *controller, int amount) {
	if (!off.moneyServices || !off.account) return;
	__try {
		void *services = Read<void *>(controller, off.moneyServices);
		if (!services || Read<int>(services, off.account) == amount) return;
		Write<int>(services, off.account, amount);
		// The services object is networked through its controller, so that is what gets told.
		MarkChanged(controller, off.moneyServices);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
	}
}

void SetSteamId(void *controller, uint64_t steamId) {
	__try {
		if (Read<uint64_t>(controller, off.steamId) == steamId) return;
		Write<uint64_t>(controller, off.steamId, steamId);
		MarkChanged(controller, off.steamId);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
	}
}

void SetOriginalOwner(void *weapon, uint64_t steamId) {
	if (!weapon || !off.xuidLow || !off.xuidHigh) return;
	__try {
		Write<uint32_t>(weapon, off.xuidLow, uint32_t(steamId));
		MarkChanged(weapon, off.xuidLow);
		Write<uint32_t>(weapon, off.xuidHigh, uint32_t(steamId >> 32));
		MarkChanged(weapon, off.xuidHigh);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
	}
}

void *EntityFromHandle(uint32_t handle) {
	if (handle == 0xffffffffu || !api.entityByIndex || !api.entitySystem || !*api.entitySystem) return nullptr;
	__try {
		void *e = api.entityByIndex(*api.entitySystem, int(handle & 0x7fff));
		return (e && HandleOf(e) == handle) ? e : nullptr;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return nullptr;
	}
}

void *EntityFromIndex(int index) {
	if (!api.entityByIndex || !api.entitySystem || !*api.entitySystem) return nullptr;
	__try {
		return api.entityByIndex(*api.entitySystem, index);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return nullptr;
	}
}

uint32_t HandleOf(void *entity) {
	__try {
		void *identity = Read<void *>(entity, off.entityIdentity);
		return identity ? Read<uint32_t>(identity, kIdentityHandle) : 0xffffffffu;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return 0xffffffffu;
	}
}

const char *ClassNameOf(void *entity) {
	__try {
		void *identity = entity ? Read<void *>(entity, off.entityIdentity) : nullptr;
		const char *name = identity ? Read<const char *>(identity, off.designerName) : nullptr;
		return name ? name : "";
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return "";
	}
}

void *FindByTargetName(const char *name) {
	if (!api.entityByIndex || !api.entitySystem || !*api.entitySystem) return nullptr;
	__try {
		for (int i = 0; i < 8192; ++i) {
			void *e = api.entityByIndex(*api.entitySystem, i);
			void *identity = e ? Read<void *>(e, off.entityIdentity) : nullptr;
			const char *text = identity ? Read<const char *>(identity, off.targetName) : nullptr;
			if (text && 0 == strcmp(text, name)) return e;
		}
	} __except (EXCEPTION_EXECUTE_HANDLER) {
	}
	return nullptr;
}

void *PawnOf(void *controller) {
	__try {
		return EntityFromHandle(Read<uint32_t>(controller, off.playerPawn));
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return nullptr;
	}
}

void *ControllerOfPawn(void *pawn) {
	__try {
		return EntityFromHandle(Read<uint32_t>(pawn, off.pawnController));
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return nullptr;
	}
}

bool Alive(void *pawn) {
	__try {
		return pawn && Read<uint8_t>(pawn, off.lifeState) == 0 && Read<int>(pawn, off.health) > 0;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
}

int Team(void *entity) {
	__try {
		return Read<uint8_t>(entity, off.team);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return 0;
	}
}

int Health(void *entity) {
	__try {
		return Read<int>(entity, off.health);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return 0;
	}
}

int Armor(void *pawn) {
	if (!off.armor) return 0;
	__try {
		return Read<int>(pawn, off.armor);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return 0;
	}
}

bool HasHelmet(void *pawn) {
	if (!off.helmet || !api.itemServicesAt) return false;
	__try {
		void *services = Read<void *>(pawn, api.itemServicesAt);
		return services && Read<uint8_t>(services, off.helmet) != 0;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
}

void SetHealth(void *entity, int health) {
	__try {
		if (Read<int>(entity, off.health) == health) return;
		Write<int>(entity, off.health, health);
		MarkChanged(entity, off.health);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
	}
}

const char *PlayerName(void *controller) {
	__try {
		return reinterpret_cast<const char *>(At(controller, off.playerName));
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return "";
	}
}

void SetPlayerName(void *controller, const char *name) {
	__try {
		auto *dst = reinterpret_cast<char *>(At(controller, off.playerName));
		if (0 == strncmp(dst, name, 127)) return;
		strncpy_s(dst, 128, name, _TRUNCATE);
		MarkChanged(controller, off.playerName);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
	}
}

// The proxy entity is looked up rarely: it lives as long as the map, and a miss is cheap to retry.
static void *GameRules() {
	const int tick = TickCount();
	if (!g_gameRules || tick < g_gameRulesCheckedAtTick || tick - g_gameRulesCheckedAtTick > 640) {
		g_gameRulesCheckedAtTick = tick;
		g_gameRules = nullptr;
		for (int i = 1; i < 4096 && !g_gameRules; ++i) {
			void *e = api.entityByIndex(*api.entitySystem, i);
			if (e && 0 == strcmp(ClassNameOf(e), "cs_gamerules")) g_gameRules = Read<void *>(e, off.gameRulesProxy);
		}
	}
	return g_gameRules;
}

bool MatchSettling() {
	__try {
		void *rules = GameRules();
		if (!rules) return false;
		if (Read<uint8_t>(rules, off.warmupPeriod)) return true;
		if (off.freezePeriod && Read<uint8_t>(rules, off.freezePeriod)) return true;
		if (off.gameRestart && Read<uint8_t>(rules, off.gameRestart)) return true;
		if (off.restartRoundTime && Read<float>(rules, off.restartRoundTime) > 0.0f) return true;
		return false;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		g_gameRules = nullptr;
		return false;
	}
}

void HurryMatch() {
	__try {
		void *rules = GameRules();
		if (!rules) return;
		const float now = TickCount() / 64.0f;
		if (off.restartRoundTime && Read<float>(rules, off.restartRoundTime) > now) Write<float>(rules, off.restartRoundTime, now);
		if (off.freezePeriod && off.roundStartTime && Read<uint8_t>(rules, off.freezePeriod) && Read<float>(rules, off.roundStartTime) > now)
			Write<float>(rules, off.roundStartTime, now);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		g_gameRules = nullptr;
	}
}

static void WriteMatchLine(char *text, size_t size) {
	__try {
		if (void *rules = GameRules()) {
			snprintf(text, size, "warmup %d, freeze %d, restart %d, restart time %.1f",
			         int(Read<uint8_t>(rules, off.warmupPeriod)), off.freezePeriod ? int(Read<uint8_t>(rules, off.freezePeriod)) : -1,
			         off.gameRestart ? int(Read<uint8_t>(rules, off.gameRestart)) : -1,
			         off.restartRoundTime ? Read<float>(rules, off.restartRoundTime) : -1.0f);
		}
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		g_gameRules = nullptr;
	}
}

std::string DescribeMatch() {
	char text[128] = "no game rules";
	WriteMatchLine(text, sizeof(text));
	return text;
}

bool Respawn(void *controller) {
	if (!controller || !api.switchToPlayerPawn || !api.canRespawn || !api.tryRespawn || !off.roundStartTime) return false;
	__try {
		void *pawn = PawnOf(controller);
		void *rules = GameRules();
		if (!pawn || !rules) return false;
		// The game's check lets through a pawn that has spawned fewer than two times, which is what its
		// own join path clears this for. With players on both teams it also refuses anyone later than
		// mp_join_grace_time into a round, and that cvar stops at 30 seconds. So for this one call the
		// round started just now, and the setup keeps the cvar at its 30. The time goes straight back.
		const float roundStart = Read<float>(rules, off.roundStartTime);
		Write<float>(rules, off.roundStartTime, TickCount() / 64.0f);
		Write<int>(pawn, off.numSpawns, 0);
		// Asked first, because the switch below leaves a refused player looking at their own body
		// instead of spectating.
		const bool allowed = api.canRespawn(rules, pawn);
		if (allowed) {
			api.switchToPlayerPawn(controller);
			api.tryRespawn(pawn);
		}
		Write<float>(rules, off.roundStartTime, roundStart);
		const bool alive = Alive(pawn);
		MvmDebugLog_Linef("takes.respawn", "controller=%p pawn=%p team=%d allowed=%d alive=%d", controller, pawn, Team(controller), int(allowed), int(alive));
		return alive;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		MvmDebugLog_LinefAlways("takes.fault", "respawn faulted controller=%p", controller);
		return false;
	}
}

void RemoveEntity(void *entity) {
	if (!entity || !api.removeEntity) return;
	__try {
		api.removeEntity(entity);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		MvmDebugLog_LinefAlways("takes.fault", "remove faulted entity=%p", entity);
	}
}

void *OwnerOf(void *entity) {
	__try {
		return EntityFromHandle(Read<uint32_t>(entity, off.ownerEntity));
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return nullptr;
	}
}

int ClipOf(void *weapon) {
	if (!weapon || !off.clip1) return -1;
	__try {
		return Read<int>(weapon, off.clip1);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return -1;
	}
}

void SetArmor(void *pawn, int armor) {
	if (!off.armor) return;
	__try {
		if (Read<int>(pawn, off.armor) == armor) return;
		Write<int>(pawn, off.armor, armor);
		MarkChanged(pawn, off.armor);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
	}
}

static const char *ModelNameOf(void *pawn) {
	if (!off.modelState || !off.modelName) return nullptr;
	__try {
		void *body = Read<void *>(pawn, off.bodyComponent);
		void *node = body ? Read<void *>(body, off.sceneNode) : nullptr;
		return node ? Read<const char *>(node, off.modelState + off.modelName) : nullptr;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return nullptr;
	}
}

std::string ModelOf(void *pawn) {
	const char *name = ModelNameOf(pawn);
	return name ? name : "";
}

bool Pose(void *pawn, float pos[3], float vel[3]) {
	__try {
		void *body = Read<void *>(pawn, off.bodyComponent);
		void *node = body ? Read<void *>(body, off.sceneNode) : nullptr;
		if (!node) return false;
		std::memcpy(pos, At(node, off.absOrigin), 12);
		std::memcpy(vel, At(pawn, off.absVelocity), 12);
		return true;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
}

bool EyeAngles(void *pawn, float &pitch, float &yaw) {
	if (!off.eyeAngles) return false;
	__try {
		pitch = Read<float>(pawn, off.eyeAngles);
		yaw = Read<float>(pawn, off.eyeAngles + 4);
		return true;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
}

bool EyePosition(void *pawn, float out[3]) {
	if (!api.slotEye) return false;
	__try {
		float tmp[3] = {0, 0, 0};
		// Returns a Vector by hidden pointer: (this, &result).
		auto fn = reinterpret_cast<void *(*)(void *, float *)>(Read<void **>(pawn, 0)[api.slotEye / 8]);
		fn(pawn, tmp);
		std::memcpy(out, tmp, 12);
		return true;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
}

bool Teleport(void *pawn, const float *pos, const float *ang, const float *vel) {
	if (!api.slotTeleport || !pawn) return false;
	__try {
		auto fn = reinterpret_cast<void (*)(void *, const float *, const float *, const float *)>(
			Read<void **>(pawn, 0)[api.slotTeleport / 8]);
		fn(pawn, pos, ang, vel);
		return true;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		MvmDebugLog_LinefAlways("takes.fault", "teleport faulted pawn=%p", pawn);
		return false;
	}
}

void *ActiveWeapon(void *pawn) {
	__try {
		void *services = Read<void *>(pawn, api.weaponServicesAt);
		return services ? EntityFromHandle(Read<uint32_t>(services, off.activeWeapon)) : nullptr;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return nullptr;
	}
}

void ListWeapons(void *pawn, std::vector<void *> &out) {
	out.clear();
	__try {
		void *services = Read<void *>(pawn, api.weaponServicesAt);
		if (!services) return;
		// CNetworkUtlVectorBase<CHandle>: count, then the element pointer.
		const int count = Read<int>(services, off.myWeapons);
		auto *handles = Read<uint32_t *>(services, off.myWeapons + 8);
		if (count <= 0 || count > 64 || !handles) return;
		for (int i = 0; i < count; ++i)
			if (void *w = EntityFromHandle(handles[i])) out.push_back(w);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
	}
}

void *FindWeapon(void *pawn, const char *className) {
	std::vector<void *> weapons;
	ListWeapons(pawn, weapons);
	for (void *w : weapons)
		if (0 == strcmp(ClassNameOf(w), className)) return w;
	return nullptr;
}

void *Give(void *pawn, const char *className, bool deploy) {
	if (!api.slotGive || !pawn) return nullptr;
	__try {
		void *weaponServices = Read<void *>(pawn, api.weaponServicesAt);
		void *itemServices = Read<void *>(pawn, api.itemServicesAt);
		if (!weaponServices || !itemServices) return nullptr;
		const uint8_t saved = Read<uint8_t>(weaponServices, api.preventDeployAt);
		Write<uint8_t>(weaponServices, api.preventDeployAt, deploy ? 0 : 1);
		auto fn = reinterpret_cast<void *(*)(void *, const char *, int, void *)>(Read<void **>(itemServices, 0)[api.slotGive / 8]);
		void *item = fn(itemServices, className, -1, nullptr);
		Write<uint8_t>(weaponServices, api.preventDeployAt, saved);
		return item;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		MvmDebugLog_LinefAlways("takes.fault", "give faulted pawn=%p class=%s", pawn, className);
		return nullptr;
	}
}

bool SetAbsOrigin(void *entity, const float pos[3]) {
	if (!api.setAbsOrigin || !entity) return false;
	__try {
		api.setAbsOrigin(entity, pos);
		return true;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		MvmDebugLog_LinefAlways("takes.fault", "set origin faulted entity=%p", entity);
		return false;
	}
}

bool DestroyWeapons(void *pawn) {
	if (!api.destroyWeapons || !pawn) return false;
	__try {
		void *services = Read<void *>(pawn, api.weaponServicesAt);
		if (!services) return false;
		api.destroyWeapons(services, true);
		return true;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		MvmDebugLog_LinefAlways("takes.fault", "destroy weapons faulted pawn=%p", pawn);
		return false;
	}
}

bool SetModel(void *entity, const char *model) {
	if (!api.setModel || !entity || !model || !*model) return false;
	__try {
		api.setModel(entity, model);
		return true;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		MvmDebugLog_LinefAlways("takes.fault", "set model faulted entity=%p model=%s", entity, model);
		return false;
	}
}

namespace {
// What a weapon class shipped with, per module. Keyed by class, never by data pointer: those blocks can
// go away with a map, so a stored pointer could be written to after it was freed. Values are read the
// first time a class is seen, which is before anything here has scaled it.
struct RecoilVariance {
	float angle[2] = {0, 0}, magnitude[2] = {0, 0};
};
std::map<std::string, RecoilVariance> g_recoilOriginals;

struct VDataFields {
	const char *module = "";
	ptrdiff_t subclass = 0, angle = 0, magnitude = 0;
	bool known = false, ok = false;
};
VDataFields g_serverFields, g_clientFields;

const VDataFields &Fields(VDataFields &f, const char *module, const char *entityClass) {
	if (f.known) return f;
	f.known = true;
	f.module = module;
	f.ok = schemaLiveOffset(module, entityClass, "m_nSubclassID", &f.subclass) &&
	       schemaLiveOffset(module, "CCSWeaponBaseVData", "m_flRecoilAngleVariance", &f.angle) &&
	       schemaLiveOffset(module, "CCSWeaponBaseVData", "m_flRecoilMagnitudeVariance", &f.magnitude);
	MvmDebugLog_LinefAlways("takes.recoil", "%s fields ok=%d subclass=0x%llx angle=0x%llx magnitude=0x%llx", module, int(f.ok),
	                        (long long)f.subclass, (long long)f.angle, (long long)f.magnitude);
	return f;
}

// Raw reads and writes only, so this can sit under SEH. CBaseEntity keeps its subclass data pointer
// right after the subclass id token.
bool ReadVariance(void *entity, const VDataFields &f, RecoilVariance &out, void *&vdataOut) {
	__try {
		void *vdata = Read<void *>(entity, f.subclass + 8);
		if (!vdata) return false;
		vdataOut = vdata;
		std::memcpy(out.angle, At(vdata, f.angle), sizeof(out.angle));
		std::memcpy(out.magnitude, At(vdata, f.magnitude), sizeof(out.magnitude));
		return true;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
}

bool WriteVariance(void *vdata, const VDataFields &f, const float angle[2], const float magnitude[2]) {
	__try {
		std::memcpy(At(vdata, f.angle), angle, sizeof(float) * 2);
		std::memcpy(At(vdata, f.magnitude), magnitude, sizeof(float) * 2);
		return true;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
}

bool ScaleVData(void *entity, const char *className, const VDataFields &f, float scale) {
	if (!f.ok || !entity) return false;
	RecoilVariance current;
	void *vdata = nullptr;
	if (!ReadVariance(entity, f, current, vdata)) return false;
	const std::string key = std::string(f.module) + "|" + className;
	auto it = g_recoilOriginals.find(key);
	if (it == g_recoilOriginals.end()) {
		it = g_recoilOriginals.emplace(key, current).first;
		MvmDebugLog_LinefAlways("takes.recoil", "%s angle=(%.3f %.3f) magnitude=(%.3f %.3f)", key.c_str(), current.angle[0], current.angle[1], current.magnitude[0], current.magnitude[1]);
	}
	const RecoilVariance &o = it->second;
	const float angle[2] = {o.angle[0] * scale, o.angle[1] * scale};
	const float magnitude[2] = {o.magnitude[0] * scale, o.magnitude[1] * scale};
	return WriteVariance(vdata, f, angle, magnitude);
}

void *ClientMirror(uint32_t handle) {
	if (!g_GetEntityFromIndex || !g_pEntityList || !*g_pEntityList) return nullptr;
	__try {
		return g_GetEntityFromIndex(*g_pEntityList, int(handle & 0x7fff));
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return nullptr;
	}
}
} // namespace

bool SetRecoilRandomness(void *weapon, float scale) {
	if (!weapon) return false;
	const std::string className = ClassNameOf(weapon);
	if (className.empty()) return false;
	const bool server = ScaleVData(weapon, className.c_str(), Fields(g_serverFields, "server.dll", "CBaseEntity"), scale);
	// The client predicts the local player's recoil from its own copy of the same data, and the
	// entity index is the same on both sides of a local match.
	const bool client = ScaleVData(ClientMirror(HandleOf(weapon)), className.c_str(), Fields(g_clientFields, "client.dll", "C_BaseEntity"), scale);
	return server || client;
}

bool SerializeCmd(void *cmd, std::vector<uint8_t> &out) {
	void *message = At(cmd, kCmdMessage);
	void **vtable = Read<void **>(message, 0);
	const size_t size = reinterpret_cast<size_t (*)(void *)>(vtable[kVtByteSizeLong])(message);
	out.resize(size);
	if (!size) return true;
	// io::EpsCopyOutputStream over a flat buffer, exactly as SerializeWithCachedSizesToArray builds it.
	alignas(16) unsigned char stream[0x48] = {};
	*reinterpret_cast<uint8_t **>(stream) = out.data() + size;
	auto serialize = reinterpret_cast<uint8_t *(*)(void *, uint8_t *, void *)>(vtable[kVtInternalSerialize]);
	const uint8_t *end = serialize(message, out.data(), stream);
	return end == out.data() + size;
}

bool ParseCmd(void *cmd, const uint8_t *data, size_t size) {
	if (!api.parseFromString) return false;
	// A std::string the way MSVC lays it out, pointing at our bytes. Capacity 16 or more means the
	// pointer form; ParseFromString only reads it.
	struct {
		const void *ptr;
		size_t pad;
		size_t size;
		size_t capacity;
	} text{data, 0, size, size < 16 ? 16 : size};
	if (!api.parseFromString(At(cmd, kCmdMessage), &text)) return false;
	reinterpret_cast<void (*)(void *)>(Read<void **>(cmd, 0)[kVtCmdButtonsFromMessage])(cmd);
	return true;
}

void SetCmdNumber(void *cmd, int number) { Write<int>(cmd, kCmdNumber, number); }

std::string DescribeResolve() {
	char line[512];
	snprintf(line, sizeof(line),
		"mimic=%p parse=%p player=%p globals=%p system=%p destroy=%p model=%p teleport=0x%llx give=0x%llx eye=0x%llx "
		"after=0x%llx changed=0x%llx weaponSvc=0x%llx itemSvc=0x%llx moveSvc=0x%llx",
		api.mimic, api.parseFromString, api.playerBySlot, api.globals, api.entitySystem, api.destroyWeapons, api.setModel,
		(long long)api.slotTeleport, (long long)api.slotGive, (long long)api.slotEye, (long long)api.slotAfterCmd,
		(long long)api.slotStateChanged, (long long)api.weaponServicesAt, (long long)api.itemServicesAt,
		(long long)api.movementServicesAt);
	return line;
}

} // namespace Filmmaker::Takes::Server
