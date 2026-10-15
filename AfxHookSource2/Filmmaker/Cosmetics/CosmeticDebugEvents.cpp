// Structured EVENT logging for the SteamID-keyed cosmetic override system: the mvm_debug
// categories cosmetics.uiclick / cosmetics.glove / cosmetics.weapon / cosmetics.spectate /
// skin.live, plus the per-frame change detector that fires a skin.live snapshot on player
// switch / seek / weapon change / loadout change. Split out of CosmeticDebug.cpp (which keeps
// the read-only diagnostics PRINTING) -- see the map in CosmeticDebugInternal.h. Read-only:
// never writes to any entity; every entity read goes through the SEH-guarded POD helpers
// (CosmeticDebugRead + the local ones below).

#include "CosmeticOverrideSystem.h"
#include "CosmeticCatalog.h"
#include "CosmeticModelSwap.h"
#include "CosmeticDebugInternal.h" // shared SEH-guarded POD readers (implemented in CosmeticDebug.cpp)
#include "../Debug/MvmDebugLog.h"      // MvmDebugLog_Active / MvmDebugLog_LinefAlways / MvmAgentLog
#include "CosmeticGloveLabels.h"

#include "../../ClientEntitySystem.h" // CEntityInstance, entity-list globals, CBaseHandle
#include "../../SchemaSystem.h"        // g_clientDllOffsets, g_cosmeticsOffsetsOk
#include "../../MirvTime.h"            // g_MirvTime.GetCurrentDemoTick (seek detection for the live skin log)

#include "../../../Deps/release/prop/cs2/sdk_src/public/cdll_int.h"

#include "../../../Shared/AfxConsole.h"

#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace Filmmaker {

using namespace CosmeticDebugRead;

namespace {

// ---- LIVE per-player skin-state readers (for the mvm_debug "skin.live" log) -------------------------

// POD live skin of a single weapon ENTITY. paint/seed/wear are read from where a demo weapon's REAL
// skin lives -- the networked dynamic attributes (def 6/7/8/80) -- falling back to the local attr list,
// then the C_EconEntity fallback fields. attrSrc names which source supplied the paint so the log shows
// whether the value is the demo's real skin, an override we wrote, or nothing.
struct WeaponSkinLive {
	bool ok = false;
	uint64_t ownerXuid = 0;
	int def = 0;
	int paint = -1;
	int seed = -1;
	float wear = -1.0f;
	int stat = -1;
	char attrSrc[16] = "none";
};

// No __try in this body (it only calls the SEH-guarded POD helpers), so std::string-free + safe to
// build with C++ temporaries. ent must be a weapon-like econ entity.
void ReadWeaponSkinLive(CEntityInstance* ent, WeaponSkinLive* out) {
	*out = WeaponSkinLive{};
	if (!ent || !g_cosmeticsOffsetsOk)
		return;
	const ClientDllOffsets_t& o = g_clientDllOffsets;
	unsigned char* w = (unsigned char*)ent;
	unsigned char* itemView = w + o.C_EconEntity.m_AttributeManager + o.C_AttributeContainer.m_Item;

	WeaponVisualDiag d;
	ReadWeaponVisualDiag(w, itemView, &d);
	if (!d.ok)
		return;
	out->ownerXuid = d.ownerXuid;
	out->def = d.defIndex;

	AttrListDump net, loc;
	ReadAttrList(itemView, o.C_EconItemView.m_NetworkedDynamicAttributes, &net);
	ReadAttrList(itemView, o.C_EconItemView.m_AttributeList, &loc);
	float p = 0, s = 0, wv = 0, st = 0;
	if (AttrValueForDef(net, 6, &p)) {
		out->paint = (int)p;
		std::snprintf(out->attrSrc, sizeof(out->attrSrc), "networked");
		if (AttrValueForDef(net, 7, &s)) out->seed = (int)s;
		if (AttrValueForDef(net, 8, &wv)) out->wear = wv;
		if (AttrValueForDef(net, 80, &st)) out->stat = (int)st;
	} else if (AttrValueForDef(loc, 6, &p)) {
		out->paint = (int)p;
		std::snprintf(out->attrSrc, sizeof(out->attrSrc), "local");
		if (AttrValueForDef(loc, 7, &s)) out->seed = (int)s;
		if (AttrValueForDef(loc, 8, &wv)) out->wear = wv;
		if (AttrValueForDef(loc, 80, &st)) out->stat = (int)st;
	} else {
		// No paint attribute present -> the C_EconEntity fallback fields (only meaningful in fallback-id
		// mode). 0 fallback paint on a networked demo item just means "vanilla / not skinned".
		out->paint = d.fbPaint;
		out->seed = d.fbSeed;
		out->wear = d.fbWear;
		out->stat = d.fbStat;
		std::snprintf(out->attrSrc, sizeof(out->attrSrc), d.fbPaint > 0 ? "fallback" : "none");
	}
	out->ok = true;
}

// POD live identity of the pawn's embedded m_EconGloves item view (def + cache flags). Paint/seed/wear
// are read separately via ReadAttrList on the same glove item view in the logger.
struct GloveLiveInfo {
	bool ok = false;
	int def = 0;
	bool haveQuality = false; int32_t quality = 0;
	bool haveItemId = false;  int32_t itemIdHigh = 0;
	bool haveInit = false;    unsigned char initialized = 0;
};

void ReadGloveLiveInfo(unsigned char* pawn, GloveLiveInfo* out) {
	*out = GloveLiveInfo{};
	const ClientDllOffsets_t& o = g_clientDllOffsets;
	if (!pawn || o.C_CSPlayerPawn.m_EconGloves == 0 || o.C_EconItemView.m_iItemDefinitionIndex == 0)
		return;
	unsigned char* glove = pawn + o.C_CSPlayerPawn.m_EconGloves;
	__try {
		out->def = (int)*(uint16_t*)(glove + o.C_EconItemView.m_iItemDefinitionIndex);
		if (o.C_EconItemView.m_iEntityQuality) { out->quality = *(int32_t*)(glove + o.C_EconItemView.m_iEntityQuality); out->haveQuality = true; }
		if (o.C_EconItemView.m_iItemIDHigh)    { out->itemIdHigh = *(int32_t*)(glove + o.C_EconItemView.m_iItemIDHigh); out->haveItemId = true; }
		if (o.C_EconItemView.m_bInitialized)   { out->initialized = *(unsigned char*)(glove + o.C_EconItemView.m_bInitialized); out->haveInit = true; }
		out->ok = true;
	} __except (1) {
		out->ok = false;
	}
}

struct GloveSnapshot {
	bool ok = false;
	int def = 0;
	int paint = 0;
	float wear = 0.0f;
	int seed = 0;
	uint8_t team = 0;
};

bool ReadGloveSnapshotForSteamId(uint64_t steamId, GloveSnapshot* out) {
	*out = GloveSnapshot{};
	if (steamId == 0)
		return false;
	const ClientDllOffsets_t& o = g_clientDllOffsets;
	if (!g_cosmeticsOffsetsOk || o.C_CSPlayerPawn.m_EconGloves == 0)
		return false;
	const int highest = GetHighestEntityIndex();
	for (int i = 0; i <= highest; ++i) {
		CEntityInstance* ctrl = EntFromIndex(i);
		if (!ctrl || !ctrl->IsPlayerController() || ctrl->GetSteamId() != steamId)
			continue;
		SOURCESDK::CS2::CBaseHandle ph = ctrl->GetPlayerPawnHandle();
		CEntityInstance* pawnEnt = ph.IsValid() ? EntFromIndex(ph.GetEntryIndex()) : nullptr;
		if (!pawnEnt || !pawnEnt->IsPlayerPawn())
			return false;
		unsigned char* pawn = (unsigned char*)pawnEnt;
		if (o.C_BaseEntity.m_iTeamNum) {
			__try { out->team = *(uint8_t*)(pawn + o.C_BaseEntity.m_iTeamNum); }
			__except (1) { out->team = 0; }
		}
		GloveLiveInfo g;
		ReadGloveLiveInfo(pawn, &g);
		if (!g.ok)
			return false;
		out->def = g.def;
		unsigned char* glove = pawn + o.C_CSPlayerPawn.m_EconGloves;
		AttrListDump net, loc;
		ReadAttrList(glove, o.C_EconItemView.m_NetworkedDynamicAttributes, &net);
		ReadAttrList(glove, o.C_EconItemView.m_AttributeList, &loc);
		float p = 0.0f, s = 0.0f, wv = 0.0f;
		if (AttrValueForDef(net, 6, &p)) {
			out->paint = (int)p;
			if (AttrValueForDef(net, 7, &s)) out->seed = (int)s;
			if (AttrValueForDef(net, 8, &wv)) out->wear = wv;
		} else if (AttrValueForDef(loc, 6, &p)) {
			out->paint = (int)p;
			if (AttrValueForDef(loc, 7, &s)) out->seed = (int)s;
			if (AttrValueForDef(loc, 8, &wv)) out->wear = wv;
		}
		out->ok = true;
		return true;
	}
	return false;
}

std::string GloveLabelForSteamId(uint64_t steamId, const CosmeticProfile* prof, bool preferProfileOverride) {
	GloveSnapshot live;
	ReadGloveSnapshotForSteamId(steamId, &live);
	if (preferProfileOverride && prof && prof->gloves.set && prof->gloves.defIndex > 0)
		return CosmeticGloveLabels::FormatGloveSkinLabel(prof->gloves.defIndex, prof->gloves.paintKit);
	if (live.ok)
		return CosmeticGloveLabels::FormatGloveSkinLabel(live.def, live.paint);
	if (live.team != 0)
		return CosmeticGloveLabels::TeamDefaultGloveLabel(live.team);
	return "(unknown gloves)";
}

std::string s_pendingGloveUiLabel;

void StorePendingGloveLabelInternal(const char* uilogText) {
	if (!uilogText || !*uilogText)
		return;
	if (std::strncmp(uilogText, "[gloves]", 8) != 0)
		return;
	const char* p = uilogText + 8;
	while (*p == ' ')
		++p;
	const char* defParen = std::strstr(p, " (def ");
	if (defParen)
		s_pendingGloveUiLabel.assign(p, defParen - p);
	else
		s_pendingGloveUiLabel = p;
}

std::string TakePendingGloveLabelInternal() {
	std::string out = s_pendingGloveUiLabel;
	s_pendingGloveUiLabel.clear();
	return out;
}

// Finds a weapon entity owned by `steamId` whose live item-definition index == targetDef, so the
// snapshot can reach a weapon the player OWNS but is not currently holding (holstered / inventory) --
// the override is keyed by owner XUID + def, so the matching world entity exists even when the weapon
// is not deployed. Mirrors CosmeticOverrideSystem.cpp's LooksLikeWeaponEntity + TryReadWeaponEconInfo
// gating. SEH-guarded POD body (no C++ objects needing unwind, so __try is legal here). Returns the
// entity (and its index via outIndex) or nullptr if none matched.
CEntityInstance* FindOwnedWeaponByDef(uint64_t steamId, int targetDef, int* outIndex) {
	if (outIndex) *outIndex = -1;
	if (steamId == 0 || targetDef <= 0 || !g_cosmeticsOffsetsOk)
		return nullptr;
	const ClientDllOffsets_t& o = g_clientDllOffsets;
	const int highest = GetHighestEntityIndex();
	for (int i = 0; i <= highest; ++i) {
		CEntityInstance* ent = EntFromIndex(i);
		if (!ent)
			continue;
		const char* cls = ent->GetClassName();
		const char* clientCls = ent->GetClientClassName();
		bool weaponish = (cls && std::strstr(cls, "weapon_")) || (cls && std::strstr(cls, "Weapon"))
			|| (clientCls && std::strstr(clientCls, "Weapon"));
		if (!weaponish)
			continue;
		unsigned char* w = (unsigned char*)ent;
		bool match = false;
		__try {
			uint32_t xLow = *(uint32_t*)(w + o.C_EconEntity.m_OriginalOwnerXuidLow);
			uint32_t xHigh = *(uint32_t*)(w + o.C_EconEntity.m_OriginalOwnerXuidHigh);
			uint64_t xuid = ((uint64_t)xHigh << 32) | (uint64_t)xLow;
			unsigned char* itemView = w + o.C_EconEntity.m_AttributeManager + o.C_AttributeContainer.m_Item;
			int liveDef = (int)*(uint16_t*)(itemView + o.C_EconItemView.m_iItemDefinitionIndex);
			match = (xuid == steamId && liveDef == targetDef);
		} __except (1) {
			match = false;
		}
		if (match) {
			if (outIndex) *outIndex = i;
			return ent;
		}
	}
	return nullptr;
}

} // namespace

void Cosmetics_LogWeaponSnapshot(const char* cmd, const char* phase, uint64_t steamId, int targetDef) {
	CosmeticOverrideSystem& sys = CosmeticsRef();

	// Always emit SOMETHING so a skin click that produced no visible change is still explained
	// (the user's complaint was that nothing showed up at all). Each early-out states why.
	if (!g_cosmeticsOffsetsOk) {
		advancedfx::Message("%s cosmetics uiclick %s: (econ offsets unresolved -- cannot read live weapon)\n", cmd, phase);
		if (MvmDebugLog_Active())
			MvmDebugLog_LinefAlways("cosmetics.uiclick", "%s offsetsUnresolved", phase);
		return;
	}
	if (!sys.InDemoContext()) {
		advancedfx::Message("%s cosmetics uiclick %s: (not in a demo -- no live weapon to read)\n", cmd, phase);
		if (MvmDebugLog_Active())
			MvmDebugLog_LinefAlways("cosmetics.uiclick", "%s notInDemo", phase);
		return;
	}

	// Prefer the OWNED weapon that matches the picked def (reaches a holstered/not-deployed weapon);
	// fall back to the spectated player's active/held weapon when targetDef<=0 or nothing matched.
	const uint64_t owner = (steamId != 0) ? steamId : sys.CurrentSpectatedSteamId();
	int weaponIndex = -1;
	CEntityInstance* weapon = FindOwnedWeaponByDef(owner, targetDef, &weaponIndex);
	const char* source = "ownedDef";
	if (!weapon) {
		const int pawnIndex = sys.CurrentSpectatedPawnIndex();
		CEntityInstance* pawn = EntFromIndex(pawnIndex);
		if (!pawn || !pawn->IsPlayerPawn()) {
			advancedfx::Message("%s cosmetics uiclick %s: (def=%d not owned by %llu and no spectated pawn=%d)\n",
				cmd, phase, targetDef, (unsigned long long)owner, pawnIndex);
			if (MvmDebugLog_Active())
				MvmDebugLog_LinefAlways("cosmetics.uiclick", "%s noPawn targetDef=%d owner=%llu pawn=%d",
					phase, targetDef, (unsigned long long)owner, pawnIndex);
			return;
		}
		SOURCESDK::CS2::CBaseHandle wh = pawn->GetActiveWeaponHandle();
		weapon = wh.IsValid() ? EntFromIndex(wh.GetEntryIndex()) : nullptr;
		weaponIndex = wh.IsValid() ? wh.GetEntryIndex() : -1;
		source = (targetDef > 0) ? "activeWeapon(def-not-found)" : "activeWeapon";
		if (!weapon) {
			advancedfx::Message("%s cosmetics uiclick %s: (def=%d not found and pawn=%d has no active weapon)\n",
				cmd, phase, targetDef, pawnIndex);
			if (MvmDebugLog_Active())
				MvmDebugLog_LinefAlways("cosmetics.uiclick", "%s noWeapon targetDef=%d pawn=%d", phase, targetDef, pawnIndex);
			return;
		}
	}

	const char* cls = weapon->GetClassName();
	const ClientDllOffsets_t& o = g_clientDllOffsets;
	unsigned char* w = (unsigned char*)weapon;
	unsigned char* itemView = w + o.C_EconEntity.m_AttributeManager + o.C_AttributeContainer.m_Item;

	// All field reads happen inside the POD-only SEH helpers (no __try in this function), so std::string
	// temporaries below are fine (no MSVC C2712).
	WeaponVisualDiag d;
	ReadWeaponVisualDiag(w, itemView, &d);

	AttrListDump networked;
	ReadAttrList(itemView, o.C_EconItemView.m_NetworkedDynamicAttributes, &networked);
	float nPaint = 0, nSeed = 0, nWear = 0;
	bool hP = AttrValueForDef(networked, 6, &nPaint);
	bool hS = AttrValueForDef(networked, 7, &nSeed);
	bool hW = AttrValueForDef(networked, 8, &nWear);

	char netPaint[24], netSeed[24], netWear[24];
	if (hP) std::snprintf(netPaint, sizeof(netPaint), "%d", (int)nPaint); else std::snprintf(netPaint, sizeof(netPaint), "absent");
	if (hS) std::snprintf(netSeed, sizeof(netSeed), "%d", (int)nSeed); else std::snprintf(netSeed, sizeof(netSeed), "absent");
	if (hW) std::snprintf(netWear, sizeof(netWear), "%.3f", nWear); else std::snprintf(netWear, sizeof(netWear), "absent");

	// The decisive line: networked def6 paint is where a demo weapon's REAL skin lives, so comparing
	// it before vs after the click shows whether the override actually changed what the game reads.
	advancedfx::Message("%s cosmetics uiclick %s: src=%s idx=%d cls='%s' def=%d ok=%d netPaint=%s netSeed=%s netWear=%s fbPaint=%d fbWear=%.3f\n",
		cmd, phase, source, weaponIndex, cls ? cls : "?", d.defIndex, d.ok ? 1 : 0,
		netPaint, netSeed, netWear, d.fbPaint, d.fbWear);
	if (MvmDebugLog_Active())
		MvmDebugLog_LinefAlways("cosmetics.uiclick",
			"%s src=%s idx=%d cls='%s' def=%d ok=%d netPaint=%s netSeed=%s netWear=%s fbPaint=%d fbWear=%.3f",
			phase, source, weaponIndex, cls ? cls : "?", d.defIndex, d.ok ? 1 : 0,
			netPaint, netSeed, netWear, d.fbPaint, d.fbWear);
}

void Cosmetics_StorePendingUiGloveLabel(const char* uilogText) {
	StorePendingGloveLabelInternal(uilogText);
}

void Cosmetics_LogGlovePick(uint64_t steamId, int newDef, int newPaint, float newWear, int newSeed) {
	if (!MvmDebugLog_Active())
		return;
	CosmeticOverrideSystem& sys = CosmeticsRef();
	const std::string playerName = sys.NameForSteamId(steamId);
	const char* nameDisp = playerName.empty() ? "(unnamed)" : playerName.c_str();

	GloveSnapshot liveBefore;
	ReadGloveSnapshotForSteamId(steamId, &liveBefore);
	std::string beforeLabel;
	if (liveBefore.ok)
		beforeLabel = CosmeticGloveLabels::FormatGloveSkinLabel(liveBefore.def, liveBefore.paint);
	else if (liveBefore.team != 0)
		beforeLabel = CosmeticGloveLabels::TeamDefaultGloveLabel(liveBefore.team);
	else
		beforeLabel = "(unknown gloves)";

	std::string afterLabel = TakePendingGloveLabelInternal();
	if (afterLabel.empty())
		afterLabel = CosmeticGloveLabels::FormatGloveSkinLabel(newDef, newPaint);

	MvmDebugLog_LinefAlways("cosmetics.glove",
		"PICK player='%s' steam=%llu before='%s' after='%s' wantDef=%d wantPaint=%d wantWear=%.4f wantSeed=%d liveBefore(def=%d paint=%d wear=%.4f)",
		nameDisp, (unsigned long long)steamId, beforeLabel.c_str(), afterLabel.c_str(),
		newDef, newPaint, newWear, newSeed,
		liveBefore.ok ? liveBefore.def : -1, liveBefore.ok ? liveBefore.paint : -1,
		liveBefore.ok ? liveBefore.wear : -1.0f);

	char data[384];
	std::snprintf(data, sizeof(data),
		"\"player\":\"%s\",\"steamId\":%llu,\"before\":\"%s\",\"after\":\"%s\",\"wantDef\":%d,\"wantPaint\":%d",
		nameDisp, (unsigned long long)steamId, beforeLabel.c_str(), afterLabel.c_str(), newDef, newPaint);
	MvmAgentLog("DBG", "CosmeticDebugEvents.cpp:LogGlovePick", "glove_pick", data);
}

void Cosmetics_LogWeaponPick(uint64_t steamId, int defIndex, int newPaint, float newWear, int newSeed, int statTrak) {
	if (!MvmDebugLog_Active())
		return;
	CosmeticOverrideSystem& sys = CosmeticsRef();
	const std::string playerName = sys.NameForSteamId(steamId);
	const char* nameDisp = playerName.empty() ? "(unnamed)" : playerName.c_str();

	int beforePaint = -1;
	int beforeLegacy = -2;
	int weaponIdx = -1;
	CEntityInstance* weapon = FindOwnedWeaponByDef(steamId, defIndex, &weaponIdx);
	if (weapon) {
		WeaponSkinLive ws;
		ReadWeaponSkinLive(weapon, &ws);
		if (ws.ok) {
			beforePaint = ws.paint;
			if (beforePaint > 0)
				beforeLegacy = PaintKitLegacyModel(beforePaint);
		}
	}

	const int wantLegacy = PaintKitLegacyModel(newPaint);
	const uint64_t wantMask = ResolveMeshMask(newPaint, /*knife=*/false, sys.MeshLegacyMode(),
		1, 2); // log with default masks; tunables live in sys but 1/2 match defaults

	MvmDebugLog_LinefAlways("cosmetics.weapon",
		"PICK player='%s' steam=%llu def=%d idx=%d obs=%u before(paint=%d legacy=%d) want(paint=%d legacy=%d mask=%llu wear=%.4f seed=%d st=%d)",
		nameDisp, (unsigned long long)steamId, defIndex, weaponIdx,
		(unsigned)sys.CurrentObserverMode(),
		beforePaint, beforeLegacy, newPaint, wantLegacy, (unsigned long long)wantMask,
		newWear, newSeed, statTrak);
	char data[512];
	std::snprintf(data, sizeof(data),
		"\"player\":\"%s\",\"steamId\":%llu,\"def\":%d,\"idx\":%d,\"obs\":%u,"
		"\"beforePaint\":%d,\"beforeLegacy\":%d,\"wantPaint\":%d,\"wantLegacy\":%d,\"wantMask\":%llu",
		nameDisp, (unsigned long long)steamId, defIndex, weaponIdx, (unsigned)sys.CurrentObserverMode(),
		beforePaint, beforeLegacy, newPaint, wantLegacy, (unsigned long long)wantMask);
	MvmAgentLog("FP-VM", "CosmeticDebugEvents.cpp:LogWeaponPick", "weapon_pick", data);
}

void Cosmetics_LogSpectateTargetChange(uint64_t fromSteamId, uint64_t toSteamId, const char* reason) {
	if (!MvmDebugLog_Active())
		return;
	CosmeticOverrideSystem& sys = CosmeticsRef();

	std::string fromName = fromSteamId ? sys.NameForSteamId(fromSteamId) : std::string();
	std::string toName = toSteamId ? sys.NameForSteamId(toSteamId) : std::string();
	if (fromName.empty()) fromName = fromSteamId ? "(unnamed)" : "(none)";
	if (toName.empty()) toName = toSteamId ? "(unnamed)" : "(none)";

	const CosmeticProfile* fromProf = fromSteamId ? sys.Store().Find(fromSteamId) : nullptr;
	const CosmeticProfile* toProf = toSteamId ? sys.Store().Find(toSteamId) : nullptr;
	const std::string fromGloves = fromSteamId
		? GloveLabelForSteamId(fromSteamId, fromProf, true) : std::string("(n/a)");
	const std::string toGloves = toSteamId
		? GloveLabelForSteamId(toSteamId, toProf, true) : std::string("(n/a)");
	const int toProfiled = toProf ? 1 : 0;
	const int gloveRearm = (toProf && toProf->gloves.set && toProf->gloves.defIndex > 0) ? 1 : 0;

	MvmDebugLog_LinefAlways("cosmetics.spectate",
		"SWITCH reason=%s obs=%u pawn=%d from='%s' steam=%llu gloves='%s' -> to='%s' steam=%llu gloves='%s' profiled=%d gloveRearm=%d",
		reason ? reason : "?", (unsigned)sys.CurrentObserverMode(), sys.CurrentSpectatedPawnIndex(),
		fromName.c_str(), (unsigned long long)fromSteamId, fromGloves.c_str(),
		toName.c_str(), (unsigned long long)toSteamId, toGloves.c_str(), toProfiled, gloveRearm);

	char data[384];
	std::snprintf(data, sizeof(data),
		"\"reason\":\"%s\",\"fromName\":\"%s\",\"fromSteam\":%llu,\"toName\":\"%s\",\"toSteam\":%llu,\"gloveRearm\":%d",
		reason ? reason : "?", fromName.c_str(), (unsigned long long)fromSteamId,
		toName.c_str(), (unsigned long long)toSteamId, gloveRearm);
	MvmAgentLog("DBG", "CosmeticDebugEvents.cpp:LogSpectateSwitch", "spectate_switch", data);
}

// Emit ONE full snapshot of the currently-spectated player's LIVE equipped cosmetics into the
// mvm_debug log (category "skin.live"). Covers agent model, gloves+skin, and every owned econ weapon
// (knife/primary/secondary/other) with the def/paint/seed/wear the game is actually rendering AND the
// override the system WOULD apply -- so what renders can be compared against what we set. Read-only;
// every entity read goes through the SEH-guarded POD helpers above (this function itself has no __try,
// so it may freely use std::string). No-op unless mvm_debug is active.
void Cosmetics_LogLiveSkinState(const char* reason) {
	if (!MvmDebugLog_Active())
		return;
	CosmeticOverrideSystem& sys = CosmeticsRef();
	if (!g_cosmeticsOffsetsOk || !sys.InDemoContext())
		return;
	const ClientDllOffsets_t& o = g_clientDllOffsets;

	const int pawnIndex = sys.CurrentSpectatedPawnIndex();
	const uint64_t steamId = sys.CurrentSpectatedSteamId();
	const uint8_t obs = sys.CurrentObserverMode();
	int tick = 0; g_MirvTime.GetCurrentDemoTick(tick);
	const std::string name = sys.NameForSteamId(steamId);
	const CosmeticProfile* prof = (steamId != 0) ? sys.Store().Find(steamId) : nullptr;

	MvmDebugLog_LinefAlways("skin.live",
		"== reason=%s steam=%llu name='%s' pawn=%d obs=%u tick=%d profiled=%d enabled=%d",
		reason ? reason : "?", (unsigned long long)steamId, name.c_str(), pawnIndex,
		(unsigned)obs, tick, prof ? 1 : 0, sys.Enabled() ? 1 : 0);

	CEntityInstance* pawn = EntFromIndex(pawnIndex);
	if (pawnIndex < 0 || !pawn || !pawn->IsPlayerPawn()) {
		MvmDebugLog_LinefAlways("skin.live", "   (no spectated player pawn)");
		return;
	}

	int activeIdx = -1;
	{
		SOURCESDK::CS2::CBaseHandle wh = pawn->GetActiveWeaponHandle();
		if (wh.IsValid()) activeIdx = wh.GetEntryIndex();
	}

	// AGENT / player model.
	{
		ModelNameResult agent; ReadPawnModelName(pawnIndex, &agent);
		std::string ov = (prof && prof->agent.set && !prof->agent.model.empty())
			? prof->agent.model : std::string("(none)");
		MvmDebugLog_LinefAlways("skin.live", "   agent model='%s' override='%s'",
			agent.ok ? agent.name : "(unresolved)", ov.c_str());
	}

	// GLOVES (the pawn's embedded m_EconGloves item view -- this IS the third-person glove source; if
	// it reads the override def/paint, the engine should render it without any world-model swap).
	if (o.C_CSPlayerPawn.m_EconGloves == 0) {
		MvmDebugLog_LinefAlways("skin.live", "   gloves (m_EconGloves offset unresolved on this build)");
	} else {
		GloveLiveInfo g; ReadGloveLiveInfo((unsigned char*)pawn, &g);
		unsigned char* glove = (unsigned char*)pawn + o.C_CSPlayerPawn.m_EconGloves;
		AttrListDump net, loc;
		ReadAttrList(glove, o.C_EconItemView.m_NetworkedDynamicAttributes, &net);
		ReadAttrList(glove, o.C_EconItemView.m_AttributeList, &loc);
		int gp = -1, gs = -1; float gw = -1.0f; const char* src = "none";
		float p = 0, s = 0, wv = 0;
		if (AttrValueForDef(net, 6, &p)) { gp = (int)p; src = "networked"; if (AttrValueForDef(net, 7, &s)) gs = (int)s; if (AttrValueForDef(net, 8, &wv)) gw = wv; }
		else if (AttrValueForDef(loc, 6, &p)) { gp = (int)p; src = "local"; if (AttrValueForDef(loc, 7, &s)) gs = (int)s; if (AttrValueForDef(loc, 8, &wv)) gw = wv; }
		char ov[112];
		if (prof && prof->gloves.set)
			std::snprintf(ov, sizeof(ov), "def=%d paint=%d wear=%.3f seed=%d", prof->gloves.defIndex, prof->gloves.paintKit, prof->gloves.wear, prof->gloves.seed);
		else
			std::snprintf(ov, sizeof(ov), "(none)");
		const int glovePaintLog = (gp >= 0) ? gp : 0;
		const std::string liveLabel = g.ok
			? CosmeticGloveLabels::FormatGloveSkinLabel(g.def, glovePaintLog) : std::string("(unresolved)");
		MvmDebugLog_LinefAlways("skin.live",
			"   gloves label='%s' liveDef=%d quality=%d itemIdHigh=%d init=%d paint=%d seed=%d wear=%.3f attrSrc=%s netCount=%d override(%s)",
			liveLabel.c_str(),
			g.def, g.haveQuality ? g.quality : -999, g.haveItemId ? g.itemIdHigh : -999,
			g.haveInit ? (int)g.initialized : -1, gp, gs, gw, src, net.ok ? net.count : -1, ov);
	}

	// WEAPONS owned by this player (walk the whole entity list; a player can carry several at once,
	// plus dropped guns still carry the original owner XUID). Classify each into a slot and mark the
	// active/deployed one. "any other skin-related item" the player owns shows here too (slot=none).
	const int highest = GetHighestEntityIndex();
	int logged = 0;
	for (int i = 0; i <= highest && logged < 32; ++i) {
		CEntityInstance* ent = EntFromIndex(i);
		if (!ent)
			continue;
		const char* cls = ent->GetClassName();
		const char* clientCls = ent->GetClientClassName();
		bool weaponish = (cls && std::strstr(cls, "weapon_")) || (cls && std::strstr(cls, "Weapon"))
			|| (clientCls && std::strstr(clientCls, "Weapon"));
		if (!weaponish)
			continue;
		WeaponSkinLive ws; ReadWeaponSkinLive(ent, &ws);
		if (!ws.ok || ws.ownerXuid != steamId)
			continue;
		++logged;
		CosmeticSlot slot = CosmeticCatalog::IsKnifeDef(ws.def)
			? CosmeticSlot::Knife : CosmeticCatalog::SlotForDefIndex(ws.def);
		const CosmeticItem* item = nullptr;
		if (slot == CosmeticSlot::Knife) { if (prof && prof->knife.set) item = &prof->knife; }
		else if (prof) { const CosmeticItem* c = prof->FindWeapon(ws.def); if (c && c->set) item = c; }
		char ov[112];
		if (item)
			std::snprintf(ov, sizeof(ov), "def=%d paint=%d wear=%.3f seed=%d st=%d", item->defIndex, item->paintKit, item->wear, item->seed, item->statTrak);
		else
			std::snprintf(ov, sizeof(ov), "(none)");
		ModelNameResult wm; ReadEntityModelName(ent, &wm);
		// Legacy classification of the LIVE paint kit (1=legacy CS:GO mesh / 0=modern CS2 mesh /
		// -1=unknown / -2=no paint). A weapon .vmdl holds both meshes; if a skin is authored for the
		// mesh the weapon is NOT currently showing, it renders as default -- compare this between a
		// skin that renders and one that doesn't to confirm the mesh-group mask is the cause.
		int liveLegacy = (slot != CosmeticSlot::Knife && ws.paint > 0) ? PaintKitLegacyModel(ws.paint) : -2;
		MvmDebugLog_LinefAlways("skin.live",
			"   weapon slot=%s idx=%d cls='%s' liveDef=%d active=%d paint=%d seed=%d wear=%.4f stat=%d attrSrc=%s meshLegacy=%d worldModel='%s' override(%s)",
			SlotLabel(slot), i, cls ? cls : "?", ws.def, (i == activeIdx) ? 1 : 0,
			ws.paint, ws.seed, ws.wear, ws.stat, ws.attrSrc, liveLegacy, wm.ok ? wm.name : "(unresolved)", ov);
	}
	if (logged == 0)
		MvmDebugLog_LinefAlways("skin.live", "   (no econ weapons owned by this player in the entity list right now)");

	// First-person viewmodel (HUD-arms child) -- often differs from the world weapon entity above.
	if (pawn && activeIdx >= 0) {
		CEntityInstance* activeEnt = EntFromIndex(activeIdx);
		const char* activeCls = activeEnt ? activeEnt->GetClassName() : nullptr;
		int vmIdx = -1;
		int vmPaint = -1;
		uint64_t vmMesh = 0;
		const bool vmFound = ReadActiveViewmodelWeaponState((unsigned char*)pawn, activeCls,
			&vmIdx, &vmPaint, &vmMesh);
		int vmLegacy = (vmPaint > 0) ? PaintKitLegacyModel(vmPaint) : -2;
		MvmDebugLog_LinefAlways("skin.live",
			"   viewmodel worldIdx=%d vmIdx=%d cls='%s' vmPaint=%d vmMesh=%llu vmLegacy=%d found=%d",
			activeIdx, vmIdx, activeCls ? activeCls : "?", vmPaint,
			(unsigned long long)vmMesh, vmLegacy, vmFound ? 1 : 0);
	} else {
		MvmDebugLog_LinefAlways("skin.live", "   viewmodel (no active weapon / pawn)");
	}
}

// Per-MAIN-thread-frame change detector for the live skin log. Cheap reads each frame (only while
// mvm_debug is active) of the spectated player + active weapon def/paint + glove def/paint + demo
// tick; when any of those change relative to the previous frame -- a player switch, a weapon switch,
// a loadout/skin change, or a demo seek (tick jump) -- it fires ONE full Cosmetics_LogLiveSkinState
// snapshot tagged with the reason. State is function-static (single main-thread caller).
void Cosmetics_TickSkinStateLog() {
	if (!MvmDebugLog_Active())
		return;
	CosmeticOverrideSystem& sys = CosmeticsRef();
	if (!g_cosmeticsOffsetsOk || !sys.InDemoContext())
		return;

	static bool s_have = false;
	static uint64_t s_steam = 0;
	static int s_activeDef = -1;
	static int s_activePaint = INT_MIN;
	static int s_gloveDef = -1;
	static int s_glovePaint = INT_MIN;
	static int s_prevTick = -1;

	const uint64_t steam = sys.CurrentSpectatedSteamId();
	const int pawnIndex = sys.CurrentSpectatedPawnIndex();

	int activeDef = -1, activePaint = INT_MIN, gloveDef = -1, glovePaint = INT_MIN;
	CEntityInstance* pawn = EntFromIndex(pawnIndex);
	if (pawn && pawn->IsPlayerPawn()) {
		const ClientDllOffsets_t& o = g_clientDllOffsets;
		SOURCESDK::CS2::CBaseHandle wh = pawn->GetActiveWeaponHandle();
		CEntityInstance* weapon = wh.IsValid() ? EntFromIndex(wh.GetEntryIndex()) : nullptr;
		if (weapon) {
			WeaponSkinLive ws; ReadWeaponSkinLive(weapon, &ws);
			if (ws.ok) { activeDef = ws.def; activePaint = ws.paint; }
		}
		if (o.C_CSPlayerPawn.m_EconGloves) {
			GloveLiveInfo g; ReadGloveLiveInfo((unsigned char*)pawn, &g);
			if (g.ok) gloveDef = g.def;
			unsigned char* glove = (unsigned char*)pawn + o.C_CSPlayerPawn.m_EconGloves;
			AttrListDump net; ReadAttrList(glove, o.C_EconItemView.m_NetworkedDynamicAttributes, &net);
			float p = 0; if (AttrValueForDef(net, 6, &p)) glovePaint = (int)p;
		}
	}

	int tick = 0;
	const bool haveTick = g_MirvTime.GetCurrentDemoTick(tick);
	bool seek = false;
	if (haveTick && s_prevTick >= 0) {
		int d = tick - s_prevTick;
		if (d < 0) d = -d;
		seek = (d > 64); // normal playback advances ~1 tick/frame; a seek jumps far
	}
	if (haveTick)
		s_prevTick = tick;

	const char* reason = nullptr;
	if (!s_have) reason = "init";
	else if (steam != s_steam) reason = "player-switch";
	else if (seek) reason = "seek";
	else if (activeDef != s_activeDef) reason = "weapon-change";
	else if (activePaint != s_activePaint || gloveDef != s_gloveDef || glovePaint != s_glovePaint) reason = "loadout-change";

	if (!reason)
		return;

	s_have = true;
	s_steam = steam;
	s_activeDef = activeDef;
	s_activePaint = activePaint;
	s_gloveDef = gloveDef;
	s_glovePaint = glovePaint;

	// Seed state silently until a player is actually being spectated, so the first real snapshot is the
	// baseline ("init") rather than an empty pre-spectate frame.
	if (steam == 0 && pawnIndex < 0)
		return;

	if (reason && 0 == std::strcmp(reason, "player-switch"))
		Cosmetics_LogSpectateTargetChange(s_steam, steam, reason);

	Cosmetics_LogLiveSkinState(reason);
}

} // namespace Filmmaker
