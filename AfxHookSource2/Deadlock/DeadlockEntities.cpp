#include "DeadlockEntities.h"

#include "DeadlockAddresses.h"
#include "../Globals.h"
#include "DeadlockEntityMath.h"
#include "DeadlockSchema.h"
#include "../Filmmaker/Debug/MvmDebugLog.h"
#include "../../Shared/AfxConsole.h"
#include "../../Shared/binutils.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace Deadlock {

namespace {

// ---------------------------------------------------------------------------------------------
// The list.
// ---------------------------------------------------------------------------------------------

typedef void* (__fastcall* GetEntityFromIndex_t)(void* entityList, int index);

void** g_pEntityList = nullptr;
GetEntityFromIndex_t g_GetEntityFromIndex = nullptr;

// The entity table's capacity. The engine's own iterator returns a COUNT of live entities rather
// than a high-water index, so walking to it would stop early on a sparse table; Source 2's table
// is 0x4000 slots and the cost of asking for a null one is a bounds check.
constexpr int kMaxEntities = 0x4000;

// ---------------------------------------------------------------------------------------------
// Guarded reads.
//
// WHY SEH AND NOT VirtualQuery. The first version checked every pointer with VirtualQuery before
// reading it: a kernel call per pointer, three of them per entity slot plus a string allocation in
// the class-name read. The hero walk measured 36-47 ms PER FRAME -- the whole of a 15 fps hook, and
// the reason entity integration was switched off. A structured-exception guard costs nothing until
// it fires. The dangerous reads live in these small functions because MSVC does not allow __try in
// a function that constructs objects; every caller copies into plain buffers first.
//
// Every fault is counted. In normal use the count stays 0, and `entities` reports it, so a stale
// offset shows up as a number rather than as a follow camera that quietly does nothing.
// ---------------------------------------------------------------------------------------------

unsigned long long g_faults = 0;

bool SafeReadBytes(const void* src, void* dst, size_t bytes) {
	if (!src) return false;
	__try {
		std::memcpy(dst, src, bytes);
		return true;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		++g_faults;
		return false;
	}
}

bool SafeReadPtr(const void* at, void** out) {
	*out = nullptr;
	return SafeReadBytes(at, out, sizeof(void*)) && *out != nullptr;
}

// Copies a NUL-terminated string of at most cap-1 characters. False on a fault, an empty string, or
// no terminator inside the bound: a wrong pointer into committed memory reads as garbage, not a name.
bool SafeCopyCString(const char* src, char* dst, size_t cap) {
	dst[0] = 0;
	if (!src || cap < 2) return false;
	__try {
		size_t i = 0;
		for (; i + 1 < cap && src[i]; ++i) dst[i] = src[i];
		dst[i] = 0;
		return i > 0 && i + 1 < cap;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		++g_faults;
		dst[0] = 0;
		return false;
	}
}

// The engine's accessor itself walks chunk pointers, and a table being torn down during a level
// change can fault inside it.
bool SafeGetEntity(GetEntityFromIndex_t fn, void* list, int index, void** out) {
	*out = nullptr;
	__try {
		*out = fn(list, index);
		return *out != nullptr;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		++g_faults;
		*out = nullptr;
		return false;
	}
}

// ---------------------------------------------------------------------------------------------
// The fields. Resolved once, by name, and each one independently: a build that renames the health
// field should cost the release-on-death rule and nothing else.
// ---------------------------------------------------------------------------------------------

struct Fields {
	std::ptrdiff_t gameSceneNode = -1;    // C_BaseEntity::m_pGameSceneNode
	std::ptrdiff_t absOrigin = -1;        // CGameSceneNode::m_vecAbsOrigin
	std::ptrdiff_t absRotation = -1;      // CGameSceneNode::m_angAbsRotation
	std::ptrdiff_t viewOffset = -1;       // m_vecViewOffset (not declared on C_BasePlayerPawn/C_BaseEntity on 2026-09-13)
	std::ptrdiff_t health = -1;           // C_BaseEntity::m_iHealth
	std::ptrdiff_t lifeState = -1;        // C_BaseEntity::m_lifeState (measured +0x35c)
	std::ptrdiff_t teamNum = -1;          // C_BaseEntity::m_iTeamNum (measured +0x3f3)
	std::ptrdiff_t eyeAngles = -1;        // C_CitadelPlayerPawn::m_angEyeAngles (measured +0x11b8)
	std::ptrdiff_t controllerHandle = -1; // C_BasePlayerPawn::m_hController (measured +0x10b0)
	std::ptrdiff_t pawnHandle = -1;       // CBasePlayerController::m_hPawn (measured +0x6bc)
	std::ptrdiff_t playerName = -1;       // CBasePlayerController::m_iszPlayerName (measured +0x6f0)
	std::ptrdiff_t heroComponent = -1;    // C_CitadelPlayerPawn::m_CCitadelHeroComponent (measured +5816)
	std::ptrdiff_t spawnedHero = -1;      // CCitadelHeroComponent::m_spawnedHero (measured +24; type probed)
	std::ptrdiff_t heroId = -1;           // CitadelHeroData_t::m_HeroID (measured +40)
	std::ptrdiff_t heroSearchName = -1;   // CitadelHeroData_t::m_strHeroSearchName (measured +56)
	std::ptrdiff_t heroModelName = -1;    // CitadelHeroData_t::m_strModelName (measured +1320)
	bool resolved = false;
};

Fields g_f;

// Source 2 moves fields between a base class and its derived class across builds, so every lookup
// names every class it could plausibly live on -- Deadlock's own class first.
std::ptrdiff_t FieldAny(const char* const* classes, int count, const char* field) {
	return Schema_FieldOffsetAny(classes, count, field);
}

void ResolveFields() {
	if (g_f.resolved) return;
	if (!Schema_Ready()) return;

	static const char* kEntity[] = { "C_BaseEntity", "CBaseEntity" };
	static const char* kNode[] = { "CGameSceneNode" };
	static const char* kPawn[] = { "C_CitadelPlayerPawn", "C_BasePlayerPawn", "C_BaseModelEntity", "C_BaseEntity" };
	static const char* kController[] = { "CCitadelPlayerController", "CBasePlayerController" };

	g_f.gameSceneNode = FieldAny(kEntity, 2, "m_pGameSceneNode");
	g_f.absOrigin = FieldAny(kNode, 1, "m_vecAbsOrigin");
	g_f.absRotation = FieldAny(kNode, 1, "m_angAbsRotation");
	g_f.viewOffset = FieldAny(kPawn, 4, "m_vecViewOffset");
	g_f.health = FieldAny(kEntity, 2, "m_iHealth");
	g_f.lifeState = FieldAny(kEntity, 2, "m_lifeState");
	g_f.teamNum = FieldAny(kEntity, 2, "m_iTeamNum");
	g_f.eyeAngles = FieldAny(kPawn, 2, "m_angEyeAngles");
	g_f.controllerHandle = FieldAny(kPawn, 2, "m_hController");
	g_f.pawnHandle = FieldAny(kController, 2, "m_hPawn");
	g_f.playerName = FieldAny(kController, 2, "m_iszPlayerName");
	static const char* kCitadelPawn[] = { "C_CitadelPlayerPawn" };
	static const char* kHeroComponent[] = { "CCitadelHeroComponent" };
	static const char* kHeroData[] = { "CitadelHeroData_t" };
	g_f.heroComponent = FieldAny(kCitadelPawn, 1, "m_CCitadelHeroComponent");
	g_f.spawnedHero = FieldAny(kHeroComponent, 1, "m_spawnedHero");
	g_f.heroId = FieldAny(kHeroData, 1, "m_HeroID");
	g_f.heroSearchName = FieldAny(kHeroData, 1, "m_strHeroSearchName");
	g_f.heroModelName = FieldAny(kHeroData, 1, "m_strModelName");
	g_f.resolved = true;

	if (Filmmaker::MvmDebugLog_Active()) {
		Filmmaker::MvmDebugLog_Linef("deadlock.ents",
			"fields sceneNode=%zd absOrigin=%zd absRot=%zd viewOff=%zd health=%zd life=%zd team=%zd "
			"eyeAngles=%zd hController=%zd hPawn=%zd playerName=%zd",
			g_f.gameSceneNode, g_f.absOrigin, g_f.absRotation, g_f.viewOffset, g_f.health,
			g_f.lifeState, g_f.teamNum, g_f.eyeAngles, g_f.controllerHandle, g_f.pawnHandle,
			g_f.playerName);
	}
}

unsigned char* SceneNode(void* entity) {
	if (!entity || g_f.gameSceneNode < 0) return nullptr;
	void* node = nullptr;
	return SafeReadPtr((unsigned char*)entity + g_f.gameSceneNode, &node) ? (unsigned char*)node : nullptr;
}

bool ReadVec3(const void* at, double out[3]) {
	float v[3];
	if (!SafeReadBytes(at, v, sizeof(v))) return false;
	for (int i = 0; i < 3; ++i)
		if (!std::isfinite(v[i])) return false;
	for (int i = 0; i < 3; ++i) out[i] = (double)v[i];
	return true;
}

// The identity block, two pointers deep from the entity: entity+0x10 is the identity. Fixed
// Source 2 shape rather than a schema field -- these two strings live in the entity identity,
// not in the class layout, so no schema lookup could name them even if one wanted to.
bool IdentityPtr(void* entity, void*& outIdentity) {
	outIdentity = nullptr;
	return entity && SafeReadPtr((unsigned char*)entity + 0x10, &outIdentity);
}

bool StringAtOffset(void* base, std::ptrdiff_t offset, char* buf, size_t cap) {
	buf[0] = 0;
	if (!base) return false;
	void* str = nullptr;
	if (!SafeReadPtr((unsigned char*)base + offset, &str)) return false;
	return SafeCopyCString((const char*)str, buf, cap);
}

// identity+0x20 is the DESIGNER class name ("player", "citadel_player_controller") -- the
// string every other reader in this file already relies on.
bool ClassNameInto(void* entity, char* buf, size_t cap) {
	void* identity = nullptr;
	buf[0] = 0;
	return IdentityPtr(entity, identity) && StringAtOffset(identity, 0x20, buf, cap);
}

// identity+0x18 is the pawn's own HERO CODENAME ("hero_atlas"), confirmed live: probing a hero
// pawn printed this string at exactly this offset, right next to the class name at +0x20, while
// the SAME offset on a controller entity read as a null pointer -- this is a pawn-only field,
// which matches Deadlock having no concept of a controller's "hero" independent of its pawn.
// Measured directly; no schema field or CitadelHeroData_t chain involved (that chain, probed the
// same session, resolved to garbage -- see the P4a hero-probe evidence).
bool CodenameInto(void* entity, char* buf, size_t cap) {
	void* identity = nullptr;
	buf[0] = 0;
	return IdentityPtr(entity, identity) && StringAtOffset(identity, 0x18, buf, cap);
}

// The pawn's own controller, via m_hController -- the SAME handle `entities probe` already
// prints, here actually used rather than only displayed. Verified live both directions: a hero
// pawn's m_hController decoded to its controller's index, and that controller's own m_hPawn
// decoded back to the pawn -- so trusting the mask alone (without re-walking the round trip
// every call) is measured, not assumed. The resolved entity's class name is still checked here,
// cheaply, so a stale or reused handle on some future build fails this lookup instead of handing
// back an unrelated entity.
void* ResolveController(void* pawn) {
	if (!pawn || g_f.controllerHandle < 0) return nullptr;
	std::uint32_t handle = 0;
	if (!SafeReadBytes((unsigned char*)pawn + g_f.controllerHandle, &handle, sizeof(handle))) return nullptr;
	if (!HandleIsValid(handle)) return nullptr;
	void* controller = Entities_Get(HandleToEntityIndex(handle));
	char cls[64];
	if (!ClassNameInto(controller, cls, sizeof(cls))) return nullptr;
	if (!std::strstr(cls, "controller")) return nullptr;
	return controller;
}

// DEADLOCK'S HERO PAWN IS ENTITY CLASS `player`, and that is measured rather than assumed.
//
// The name is the ENTITY class -- lowercase, from the entity's own identity block -- not the C++
// client class. In a loaded replay the table holds `citadel_player_controller` for each slot and
// `player` for each spawned hero body; the schema's `C_CitadelPlayerPawn` is what that pawn is made
// of, and never what this string says. The controller is deliberately NOT a hero: it is the player's
// slot, it has no body and no position to frame.
constexpr const char* kHeroClass = "player";

bool IsPrintable(const char* s) {
	size_t n = 0;
	for (; s[n]; ++n)
		if ((unsigned char)s[n] < 0x20 || (unsigned char)s[n] > 0x7e) return false;
	return n >= 3;
}

void DumpQwords(const char* label, const void* base, int count) {
	advancedfx::Message("    %s @%p\n", label, base);
	for (int i = 0; i < count; ++i) {
		std::uint64_t q = 0;
		const void* at = (const unsigned char*)base + i * 8;
		if (!SafeReadBytes(at, &q, sizeof(q))) {
			advancedfx::Message("      +0x%02x  <unreadable>\n", i * 8);
			break;
		}
		char s[48];
		const bool str = SafeCopyCString((const char*)(std::uintptr_t)q, s, sizeof(s)) && IsPrintable(s);
		if (str) advancedfx::Message("      +0x%02x  %016llx  \"%s\"\n", i * 8, (unsigned long long)q, s);
		else advancedfx::Message("      +0x%02x  %016llx\n", i * 8, (unsigned long long)q);
	}
}

// Reads CitadelHeroData_t through a candidate address and prints what it finds, or nothing when the
// address does not look like hero data. Used twice by the probe, for the two ways `m_spawnedHero` could
// be laid out, because the schema names the field but not its type.
void ProbeHeroData(const char* how, void* heroData) {
	if (!heroData || g_f.heroId < 0) {
		advancedfx::Message("    hero via %s: no address\n", how);
		return;
	}
	int heroId = -1;
	SafeReadBytes((unsigned char*)heroData + g_f.heroId, &heroId, sizeof(heroId));
	char search[96], model[160];
	void* searchPtr = nullptr;
	void* modelPtr = nullptr;
	const bool haveSearch = g_f.heroSearchName >= 0
		&& SafeReadPtr((unsigned char*)heroData + g_f.heroSearchName, &searchPtr)
		&& SafeCopyCString((const char*)searchPtr, search, sizeof(search)) && IsPrintable(search);
	const bool haveModel = g_f.heroModelName >= 0
		&& SafeReadPtr((unsigned char*)heroData + g_f.heroModelName, &modelPtr)
		&& SafeCopyCString((const char*)modelPtr, model, sizeof(model)) && IsPrintable(model);
	advancedfx::Message("    hero via %s @%p: id=%d search='%s' model='%s'\n", how, heroData, heroId,
		haveSearch ? search : "", haveModel ? model : "");
}

void ProbeHandle(const char* label, void* entity, std::ptrdiff_t offset) {
	if (offset < 0) {
		advancedfx::Message("    %s: field not resolved\n", label);
		return;
	}
	std::uint32_t h = 0;
	if (!SafeReadBytes((unsigned char*)entity + offset, &h, sizeof(h))) {
		advancedfx::Message("    %s (+0x%zx): unreadable\n", label, (size_t)offset);
		return;
	}
	// Both index widths are printed because Deadlock's handle layout is not assumed from CS2's.
	const int idx15 = (int)(h & 0x7fff);
	const int idx14 = (int)(h & 0x3fff);
	char c15[64], c14[64];
	ClassNameInto(Entities_Get(idx15), c15, sizeof(c15));
	ClassNameInto(Entities_Get(idx14), c14, sizeof(c14));
	advancedfx::Message("    %s (+0x%zx): raw=0x%08x  &0x7fff=%d ('%s')  &0x3fff=%d ('%s')\n",
		label, (size_t)offset, h, idx15, c15, idx14, c14);
}

} // namespace

bool Entities_InitClient(HMODULE clientDll) {
	// Both patterns are CS2's and both hit Deadlock's client.dll unchanged -- this is shared
	// Source 2 entity-system code rather than game logic. They are still resolved here rather
	// than assumed, and still recorded in the address table, because "transfers today" is not
	// "is not version-coupled".
	//
	// The first pattern is the function that publishes the "Entities/Client Entity Count"
	// counter: it loads the entity-list global and immediately calls the iterator. The
	// displacement at +18 is the global; the call at +27 is the iterator we do not use.
	const size_t listFn = getAddress(clientDll,
		"40 55 53 48 8D AC 24 ?? ?? ?? ?? 48 81 EC ?? ?? ?? ?? 48 8B 0D ?? ?? ?? ?? 33 D2 "
		"E8 ?? ?? ?? ??");
	if (listFn) {
		const size_t insnEnd = listFn + 18 + 7;
		int32_t disp = 0;
		std::memcpy(&disp, (void*)(listFn + 18 + 3), sizeof(disp));
		g_pEntityList = (void**)(insnEnd + (ptrdiff_t)disp);
	}
	Addresses_RecordExternal("client_entity_list", "client", false,
	                         (size_t)g_pEntityList, clientDll, 0x37D5A20);

	// GetEntityFromIndex: the index-to-pointer accessor, recognisable by its 0x7ffe bound check
	// and the shift/mask chunk walk that follows.
	const size_t getFn = getAddress(clientDll, "4C 8D 49 10 81 FA FE 7F 00 00");
	g_GetEntityFromIndex = (GetEntityFromIndex_t)getFn;
	Addresses_RecordExternal("client_entity_from_index", "client", false,
	                         getFn, clientDll, 0x14A4DD0);

	return g_pEntityList != nullptr && g_GetEntityFromIndex != nullptr;
}

bool Entities_Ready() {
	ResolveFields();
	return g_pEntityList && g_GetEntityFromIndex && g_f.gameSceneNode >= 0 && g_f.absOrigin >= 0;
}

int Entities_HighestIndex() { return kMaxEntities - 1; }

void* Entities_Get(int index) {
	if (!g_pEntityList || !g_GetEntityFromIndex) return nullptr;
	if (index < 0 || index >= kMaxEntities) return nullptr;
	void* list = nullptr;
	if (!SafeReadPtr(g_pEntityList, &list)) return nullptr;
	void* ent = nullptr;
	return SafeGetEntity(g_GetEntityFromIndex, list, index, &ent) ? ent : nullptr;
}

std::string Entities_ClassName(void* entity) {
	char buf[128];
	if (!ClassNameInto(entity, buf, sizeof(buf))) return std::string();
	return std::string(buf);
}

bool Entities_ClassIs(void* entity, const char* className) {
	char buf[64];
	if (!className || !ClassNameInto(entity, buf, sizeof(buf))) return false;
	return 0 == std::strcmp(buf, className);
}

bool Entities_Origin(void* entity, double out[3]) {
	unsigned char* node = SceneNode(entity);
	if (!node || g_f.absOrigin < 0) return false;
	return ReadVec3(node + g_f.absOrigin, out);
}

bool Entities_Angles(void* entity, double out[3]) {
	unsigned char* node = SceneNode(entity);
	if (!node || g_f.absRotation < 0) return false;
	return ReadVec3(node + g_f.absRotation, out);
}

bool Entities_EyeOrigin(void* entity, double out[3]) {
	if (!Entities_Origin(entity, out)) return false;
	if (g_f.viewOffset < 0) return true;   // no view offset on this build: the feet are what we have
	double offset[3];
	if (!ReadVec3((unsigned char*)entity + g_f.viewOffset, offset)) return true;
	for (int i = 0; i < 3; ++i) out[i] += offset[i];
	return true;
}

std::string Entities_HeroCodename(void* pawn) {
	char buf[64];
	return CodenameInto(pawn, buf, sizeof(buf)) ? std::string(buf) : std::string();
}

std::string Entities_PlayerNameFor(void* pawn) {
	void* controller = ResolveController(pawn);
	if (!controller || g_f.playerName < 0) return std::string();
	// INLINE, not a pointer -- confirmed live: a controller's own m_iszPlayerName read the actual
	// player name directly at that offset (the SAME field on a PAWN, by contrast, is unpopulated,
	// because m_iszPlayerName belongs to the controller class -- this is exactly why the follow
	// camera has to hop pawn -> controller for a name at all).
	char buf[128];
	return SafeCopyCString((const char*)controller + g_f.playerName, buf, sizeof(buf))
		? std::string(buf) : std::string();
}

int Entities_Health(void* entity) {
	if (!entity || g_f.health < 0) return -1;
	int value = -1;
	if (!SafeReadBytes((unsigned char*)entity + g_f.health, &value, sizeof(value))) return -1;
	// A health of tens of millions is a wrong offset, not a tanky hero.
	if (value < -1 || value > 1000000) return -1;
	return value;
}

unsigned long long Entities_FaultCount() { return g_faults; }

namespace {
// Set only where a scan actually ran (Entities_Heroes), read by Entities_HeroCount(). A hero
// count of 0 before the first scan is correct, not a lie -- the follow panel greys the target
// controls in that state anyway, which is exactly what "no heroes yet" should look like.
int g_lastHeroCount = 0;
}

int Entities_HeroCount() { return g_lastHeroCount; }

// The hero scan does not walk the whole 0x4000-slot table. Player pawns and their controllers are
// networked player slots at the bottom of it -- measured in a live Deadlock replay, controllers at
// 1-9 and `player` pawns at 69-82 -- and this limit is that measurement with two orders of headroom.
// Each visited slot now costs an accessor call and a couple of guarded copies, not kernel calls.
constexpr int kHeroScanLimit = 1024;

void Entities_Heroes(std::vector<HeroEntry>& out) {
	out.clear();
	if (!Entities_Ready()) return;

	for (int i = 0; i <= kHeroScanLimit; ++i) {
		void* ent = Entities_Get(i);
		if (!ent || !Entities_ClassIs(ent, kHeroClass)) continue;

		HeroEntry e;
		e.index = i;
		e.className = kHeroClass;
		e.health = Entities_Health(ent);
		e.alive = e.health > 0;
		if (!Entities_Origin(ent, e.origin)) continue;

		// Deadlock's two sides, on Source's usual team numbering. Left empty rather than guessed
		// when the field did not resolve: a picker that labels both sides "Amber" is worse than one
		// that labels neither. (The byte width and the 2/3 mapping are checked by `entities probe`.)
		if (g_f.teamNum >= 0) {
			unsigned char team = 0;
			if (SafeReadBytes((unsigned char*)ent + g_f.teamNum, &team, 1)) {
				if (team == 2) e.team = "Amber";
				else if (team == 3) e.team = "Sapphire";
			}
		}

		// The two REAL names, resolved live rather than guessed: the hero codename off the pawn's
		// own identity block, the player's name off its controller. Either can come back empty on a
		// bot or an unlinked slot; ComposeHeroListName is what decides the fallback, in one place,
		// so the picker's label and this row's stored fields never disagree about what "unknown"
		// looks like.
		e.codename = Entities_HeroCodename(ent);
		e.playerName = Entities_PlayerNameFor(ent);
		e.name = ComposeHeroListName(e.playerName, HeroDisplayNameFromCodename(e.codename),
		                              (int)out.size() + 1);
		out.push_back(e);
	}
}

int Entities_NearestHero(const double from[3]) {
	std::vector<HeroEntry> heroes;
	const unsigned long long startMs = GetTickCount64();
	Entities_Heroes(heroes);
	g_lastHeroCount = (int)heroes.size();
	if (Filmmaker::MvmDebugLog_Active())
		Filmmaker::MvmDebugLog_Linef("deadlock.perf.ents", "scanMs=%llu heroes=%d via=nearest",
		                             GetTickCount64() - startMs, (int)heroes.size());
	int best = -1;
	double bestDistSq = 0.0;
	for (const HeroEntry& h : heroes) {
		const double dx = h.origin[0] - from[0];
		const double dy = h.origin[1] - from[1];
		const double dz = h.origin[2] - from[2];
		const double d = dx * dx + dy * dy + dz * dz;
		if (best < 0 || d < bestDistSq) { best = h.index; bestDistSq = d; }
	}
	return best;
}

void Entities_HeroesCached(std::vector<HeroEntry>& out) {
	// Throttled to 10 Hz: a picker (or the overlay's boxes) does not need per-frame freshness, so
	// the roster is rescanned at most every 100 ms and replayed from the cache in between. ONE
	// cache, shared by Entities_AppendHeroesJson and the overlay drawer's per-frame box list, so a
	// caller cannot end up walking the entity table twice in the same frame for two different
	// consumers of the same data.
	static std::vector<HeroEntry> s_cache;
	static unsigned long long s_nextRebuildMs = 0;
	const unsigned long long nowMs = GetTickCount64();
	if (nowMs < s_nextRebuildMs) {
		out = s_cache;
		return;
	}
	s_nextRebuildMs = nowMs + 100;

	const unsigned long long startMs = GetTickCount64();
	Entities_Heroes(s_cache);
	g_lastHeroCount = (int)s_cache.size();
	if (Filmmaker::MvmDebugLog_Active())
		Filmmaker::MvmDebugLog_Linef("deadlock.perf.ents", "scanMs=%llu heroes=%d via=cache",
		                             GetTickCount64() - startMs, (int)s_cache.size());
	out = s_cache;
}

void Entities_AppendHeroesJson(std::string& out) {
	std::vector<HeroEntry> heroes;
	Entities_HeroesCached(heroes);

	std::string built;
	built.reserve(512);
	built += "[";
	char buf[256];
	for (size_t i = 0; i < heroes.size(); ++i) {
		const HeroEntry& h = heroes[i];
		// `id` rather than `index`, because that is the key the picker reads and sends back as
		// `mirv_filmmaker follow hero <id>`. One name for one thing on both sides of the bridge.
		std::snprintf(buf, sizeof(buf),
		              "%s{\"id\":%d,\"name\":\"%s\",\"player\":\"%s\",\"hero\":\"%s\","
		              "\"team\":\"%s\",\"alive\":%s,\"health\":%d}",
		              i ? "," : "", h.index, h.name.c_str(), h.playerName.c_str(),
		              HeroDisplayNameFromCodename(h.codename).c_str(), h.team.c_str(),
		              h.alive ? "true" : "false", h.health);
		built += buf;
	}
	built += "]";
	out += built;
}

void Entities_List(const char* filter, int maxRows) {
	ResolveFields();
	if (!g_pEntityList || !g_GetEntityFromIndex) {
		advancedfx::Warning("entities: the list did not resolve; nothing to walk.\n");
		return;
	}
	if (maxRows <= 0) maxRows = 60;
	const std::string want = filter ? filter : "";

	// A console diagnostic, not a per-frame path: it walks the WHOLE table even after the print cap,
	// so the summary counts what is actually there rather than what fitted on screen.
	const unsigned long long startMs = GetTickCount64();
	int seen = 0, printed = 0, unnamed = 0;
	for (int i = 0; i <= Entities_HighestIndex(); ++i) {
		void* ent = Entities_Get(i);
		if (!ent) continue;
		++seen;
		const std::string cls = Entities_ClassName(ent);
		if (cls.empty()) { ++unnamed; continue; }
		if (!want.empty() && cls.find(want) == std::string::npos) continue;
		if (printed >= maxRows) continue;
		double o[3] = { 0, 0, 0 };
		const bool haveOrigin = Entities_Origin(ent, o);
		advancedfx::Message("  [%4d] %-40s %s\n", i, cls.c_str(),
		                    haveOrigin ? "" : "(no origin)");
		++printed;
	}
	advancedfx::Message("  %d live entit%s, %d printed, %d with no readable class name, walk %llu ms.\n",
	                    seen, seen == 1 ? "y" : "ies", printed, unnamed,
	                    GetTickCount64() - startMs);
}

void Entities_Probe(int index) {
	ResolveFields();
	// Probing reads arbitrary qwords as string pointers on purpose, so its faults are expected and are
	// kept out of the counter that normal use must hold at zero.
	const unsigned long long faultsBefore = g_faults;
	void* ent = Entities_Get(index);
	if (!ent) {
		advancedfx::Message("entities probe %d: no entity at that index\n", index);
		g_faults = faultsBefore;
		return;
	}
	char cls[128];
	ClassNameInto(ent, cls, sizeof(cls));
	advancedfx::Message("entities probe [%d] class='%s' entity=%p\n", index, cls, ent);
	DumpQwords("entity", ent, 6);
	void* identity = nullptr;
	if (SafeReadPtr((unsigned char*)ent + 0x10, &identity)) DumpQwords("identity", identity, 8);

	double v[3];
	if (Entities_Origin(ent, v))
		advancedfx::Message("    origin       %.2f %.2f %.2f\n", v[0], v[1], v[2]);
	if (Entities_Angles(ent, v))
		advancedfx::Message("    absRotation  %.2f %.2f %.2f\n", v[0], v[1], v[2]);
	if (g_f.eyeAngles >= 0 && ReadVec3((unsigned char*)ent + g_f.eyeAngles, v))
		advancedfx::Message("    eyeAngles    %.2f %.2f %.2f  (+0x%zx)\n", v[0], v[1], v[2], (size_t)g_f.eyeAngles);
	if (g_f.viewOffset >= 0 && ReadVec3((unsigned char*)ent + g_f.viewOffset, v))
		advancedfx::Message("    viewOffset   %.2f %.2f %.2f  (+0x%zx)\n", v[0], v[1], v[2], (size_t)g_f.viewOffset);
	else
		advancedfx::Message("    viewOffset   field %s\n", g_f.viewOffset >= 0 ? "unreadable" : "not resolved");
	advancedfx::Message("    health       %d\n", Entities_Health(ent));
	if (g_f.lifeState >= 0) {
		unsigned char life = 0;
		if (SafeReadBytes((unsigned char*)ent + g_f.lifeState, &life, 1))
			advancedfx::Message("    lifeState    %u  (+0x%zx)\n", life, (size_t)g_f.lifeState);
	}
	if (g_f.teamNum >= 0) {
		unsigned char teamByte = 0;
		int teamInt = 0;
		SafeReadBytes((unsigned char*)ent + g_f.teamNum, &teamByte, 1);
		SafeReadBytes((unsigned char*)ent + g_f.teamNum, &teamInt, sizeof(teamInt));
		advancedfx::Message("    teamNum      byte=%u int32=%d  (+0x%zx)\n", teamByte, teamInt, (size_t)g_f.teamNum);
	}
	ProbeHandle("m_hController", ent, g_f.controllerHandle);
	ProbeHandle("m_hPawn", ent, g_f.pawnHandle);
	if (g_f.playerName >= 0) {
		char inlineName[128], pointedName[128];
		const bool inl = SafeCopyCString((const char*)ent + g_f.playerName, inlineName, sizeof(inlineName));
		void* namePtr = nullptr;
		const bool ptr = SafeReadPtr((unsigned char*)ent + g_f.playerName, &namePtr)
			&& SafeCopyCString((const char*)namePtr, pointedName, sizeof(pointedName));
		advancedfx::Message("    playerName   inline='%s'%s  pointer='%s'%s  (+0x%zx)\n",
			inl ? inlineName : "", inl ? "" : " (none)", ptr ? pointedName : "", ptr ? "" : " (none)",
			(size_t)g_f.playerName);
	}
	if (g_f.heroComponent >= 0) {
		const void* component = (unsigned char*)ent + g_f.heroComponent;
		DumpQwords("hero component", component, 8);
		if (g_f.spawnedHero >= 0) {
			void* direct = nullptr;
			SafeReadPtr((unsigned char*)component + g_f.spawnedHero, &direct);
			ProbeHeroData("m_spawnedHero as pointer", direct);
			void* indirect = nullptr;
			if (direct) SafeReadPtr(direct, &indirect);
			ProbeHeroData("m_spawnedHero as pointer-to-pointer", indirect);
		}
	} else {
		advancedfx::Message("    hero component: field not resolved\n");
	}
	g_faults = faultsBefore;
}

void Entities_Report() {
	ResolveFields();
	advancedfx::Message("  entities:  list=%s accessor=%s  faults=%llu\n",
	                    g_pEntityList ? "resolved" : "MISSING",
	                    g_GetEntityFromIndex ? "resolved" : "MISSING", g_faults);
	advancedfx::Message("             fields sceneNode=%zd origin=%zd rotation=%zd viewOffset=%zd "
	                    "health=%zd life=%zd team=%zd eyeAngles=%zd hController=%zd hPawn=%zd name=%zd\n",
	                    g_f.gameSceneNode, g_f.absOrigin, g_f.absRotation, g_f.viewOffset,
	                    g_f.health, g_f.lifeState, g_f.teamNum, g_f.eyeAngles,
	                    g_f.controllerHandle, g_f.pawnHandle, g_f.playerName);
	if (!Entities_Ready()) {
		advancedfx::Warning("             NOT READY -- the follow camera has nothing to lock on to.\n");
		return;
	}

	const unsigned long long startMs = GetTickCount64();
	std::vector<HeroEntry> heroes;
	Entities_Heroes(heroes);
	advancedfx::Message("             %d hero pawn(s), scan %llu ms\n", (int)heroes.size(), GetTickCount64() - startMs);
	for (const HeroEntry& h : heroes) {
		advancedfx::Message("               [%d] %s %s hp=%d at %.1f %.1f %.1f\n",
		                    h.index, h.name.c_str(),
		                    h.team.empty() ? "?" : h.team.c_str(), h.health,
		                    h.origin[0], h.origin[1], h.origin[2]);
	}
}

void Entities_ListHeroes(int maxRows) {
	if (maxRows <= 0) maxRows = 32;
	if (!Entities_Ready()) {
		advancedfx::Warning("listentities: the entity list is not ready; nothing to walk.\n");
		return;
	}
	const unsigned long long startMs = GetTickCount64();
	std::vector<HeroEntry> heroes;
	Entities_Heroes(heroes);
	g_lastHeroCount = (int)heroes.size();
	advancedfx::Message("  [idx] player            hero              team      hp    alive\n");
	int printed = 0;
	for (const HeroEntry& h : heroes) {
		if (printed >= maxRows) break;
		advancedfx::Message("  [%3d] %-17s %-17s %-9s %-5d %s\n", h.index,
		                    h.playerName.empty() ? "?" : h.playerName.c_str(),
		                    HeroDisplayNameFromCodename(h.codename).c_str(),
		                    h.team.empty() ? "?" : h.team.c_str(), h.health,
		                    h.alive ? "yes" : "no");
		++printed;
	}
	advancedfx::Message("  %d hero(s), %d printed, scan %llu ms.\n",
	                    (int)heroes.size(), printed, GetTickCount64() - startMs);
}

} // namespace Deadlock
