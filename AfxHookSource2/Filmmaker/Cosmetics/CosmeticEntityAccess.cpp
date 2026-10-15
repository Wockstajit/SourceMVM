#include "CosmeticEntityAccess.h"
#include "CosmeticThemeMath.h"

#include "../../ClientEntitySystem.h" // CEntityInstance, entity-list globals, CBaseHandle
#include "../../SchemaSystem.h"       // g_clientDllOffsets

#include "../../../Deps/release/prop/AfxHookSource/SourceSdkShared.h"

#include <cstdint>
#include <cstring>

namespace Filmmaker {

// Resolves an entity by index the same way MirvCosmetics.cpp / CameraEditorHud.cpp do: bounds +
// null-list checks before touching the (possibly stale) global pointers.
CEntityInstance* CosmeticEntFromIndex(int index) {
	if (index < 0 || index > GetHighestEntityIndex() || !g_pEntityList || !*g_pEntityList || !g_GetEntityFromIndex)
		return nullptr;
	return (CEntityInstance*)g_GetEntityFromIndex(*g_pEntityList, index);
}

// Same heuristic as CameraEditorHud.cpp's CosmeticLooksLikeWeaponEntity: matches both server-style
// "weapon_*" class names and the client "CWeaponX" / "C_WeaponX" schema class names, so it is
// resilient to either naming scheme being present at runtime.
bool CosmeticLooksLikeWeaponEntity(CEntityInstance* entity) {
	const char* className = entity ? entity->GetClassName() : nullptr;
	const char* clientClass = entity ? entity->GetClientClassName() : nullptr;
	return (className && std::strstr(className, "weapon_"))
		|| (className && std::strstr(className, "Weapon"))
		|| (clientClass && std::strstr(clientClass, "Weapon"));
}

unsigned char* CosmeticPawnForSteamId(uint64_t steamId) {
	if (steamId == 0)
		return nullptr;
	const int highest = GetHighestEntityIndex();
	for (int i = 0; i <= highest; ++i) {
		CEntityInstance* controller = CosmeticEntFromIndex(i);
		if (!controller || !controller->IsPlayerController() || controller->GetSteamId() != steamId)
			continue;
		SOURCESDK::CS2::CBaseHandle handle = controller->GetPlayerPawnHandle();
		CEntityInstance* pawn = handle.IsValid() ? CosmeticEntFromIndex(handle.GetEntryIndex()) : nullptr;
		return pawn && pawn->IsPlayerPawn() ? (unsigned char*)pawn : nullptr;
	}
	return nullptr;
}

void CosmeticEnumeratePlayers(CosmeticPlayerRef* out, int maxOut, int* outCount) {
	if (outCount) *outCount = 0;
	if (!out || maxOut <= 0) return;
	int n = 0;
	const int highest = GetHighestEntityIndex();
	for (int i = 0; i <= highest && n < maxOut; ++i) {
		CEntityInstance* controller = CosmeticEntFromIndex(i);
		if (!controller || !controller->IsPlayerController())
			continue;
		const uint64_t steamId = controller->GetSteamId();
		// Team comes from the PAWN, not the controller: an override must never put a T model on a
		// CT player, and the pawn is the copy that is correct on the frame we are reading.
		SOURCESDK::CS2::CBaseHandle handle = controller->GetPlayerPawnHandle();
		CEntityInstance* pawn = handle.IsValid() ? CosmeticEntFromIndex(handle.GetEntryIndex()) : nullptr;
		const int team = (pawn && pawn->IsPlayerPawn()) ? pawn->GetTeam() : 0;
		if (team != 2 && team != 3) continue; // no playable pawn / spectator
		const auto controllerHandle = controller->GetHandle();
		if (!controllerHandle.IsValid()) continue;
		out[n].controllerHandle = controllerHandle.ToInt();
		out[n].steamId = steamId;
		out[n].team = team;
		++n;
	}
	if (outCount) *outCount = n;
}

// SEH-guarded scalar reads off an entity (used by the pawn glove/agent pass; kept as free functions
// so the caller can use C++ objects without tripping MSVC C2712 "__try + object unwinding").
float CosmeticSafeReadEntityFloat(unsigned char* base, ptrdiff_t off, float fallback) {
	if (!base || off == 0) return fallback;
	__try { return *(float*)(base + off); } __except (1) { return fallback; }
}

int CosmeticSafeReadGloveDefIndex(unsigned char* pawn) {
	const ClientDllOffsets_t& o = g_clientDllOffsets;
	if (!pawn || o.C_CSPlayerPawn.m_EconGloves == 0 || o.C_EconItemView.m_iItemDefinitionIndex == 0)
		return 0;
	__try {
		return (int)*(uint16_t*)(pawn + o.C_CSPlayerPawn.m_EconGloves + o.C_EconItemView.m_iItemDefinitionIndex);
	} __except (1) {
		return 0;
	}
}

int CosmeticReadThemePawnIdentity(CEntityInstance* controller, CEntityInstance* pawn,
	CosmeticThemePawnIdentity* out, int* health) {
	const auto& o = g_clientDllOffsets;
	if (!out || !health || !o.CEntityInstance.m_pEntity || !o.CBasePlayerController.m_hPawn
		|| !o.C_BasePlayerPawn.m_hController || !o.CBasePlayerController.m_steamID
		|| !o.C_BaseEntity.m_iTeamNum || !o.C_BaseEntity.m_iHealth) return -1;
	if (!controller || !pawn) return 0;
	__try {
		if (!controller->IsPlayerController() || !pawn->IsPlayerPawn()) return 0;
		const auto ch = controller->GetHandle();
		const auto ph = pawn->GetHandle();
		if (!ch.IsValid() || !ph.IsValid() || controller->GetPlayerPawnHandle().ToInt() != ph.ToInt()
			|| pawn->GetPlayerControllerHandle().ToInt() != ch.ToInt()) return 0;
		const int team = *(const uint8_t*)((unsigned char*)pawn + o.C_BaseEntity.m_iTeamNum);
		if (team != 2 && team != 3) return -1; // transient spectator/spawn state, not a new baseline
		CosmeticThemePawnIdentity identity;
		identity.pawn = (uintptr_t)pawn;
		identity.pawnHandle = ph.ToInt(); identity.controllerHandle = ch.ToInt();
		identity.steamId = controller->GetSteamId(); identity.team = team;
		const int currentHealth = *(const int32_t*)((unsigned char*)pawn + o.C_BaseEntity.m_iHealth);
		*out = identity;
		*health = currentHealth;
		return 1;
	} __except (1) { return -1; }
}

int CosmeticResolveThemePawn(uint32_t controllerHandle, CEntityInstance** pawn,
	CosmeticThemePawnIdentity* out, int* health) {
	if (!pawn) return -1;
	__try {
		const SOURCESDK::CS2::CBaseHandle ch(controllerHandle);
		CEntityInstance* ctrl = ch.IsValid() ? CosmeticEntFromIndex(ch.GetEntryIndex()) : nullptr;
		if (!ctrl || !ctrl->IsPlayerController() || ctrl->GetHandle().ToInt() != controllerHandle) return 0;
		const auto ph = ctrl->GetPlayerPawnHandle();
		CEntityInstance* current = ph.IsValid() ? CosmeticEntFromIndex(ph.GetEntryIndex()) : nullptr;
		const int result = CosmeticReadThemePawnIdentity(ctrl, current, out, health);
		if (result == 1) *pawn = current;
		return result;
	} __except (1) { return -1; }
}

// SEH-guarded read of an entity's LIVE rendered model path (CModelState::m_ModelName) via the same
// body-component/skeleton chain CosmeticDebug.cpp uses. Lets the diagnostic log show whether a knife/
// weapon entity is actually rendering the swapped model or still the original (shadow_daggers vs the
// target nomad/bayonet). POD-only body so __try is permitted. out[0]='\0' on any failure.
void CosmeticReadEntityModelPath(CEntityInstance* ent, char* out, size_t outSize) {
	if (out && outSize) out[0] = '\0';
	if (!ent || !out || outSize == 0)
		return;
	const ClientDllOffsets_t& o = g_clientDllOffsets;
	if (o.ModelChain.m_CBodyComponent == 0 || o.ModelChain.m_skeletonInstance == 0
		|| o.ModelChain.m_modelState == 0 || o.ModelChain.m_ModelName == 0)
		return;
	unsigned char* p = (unsigned char*)ent;
	__try {
		unsigned char* bodyComp = *(unsigned char**)(p + o.ModelChain.m_CBodyComponent);
		if ((uintptr_t)bodyComp <= 0x10000) return;
		unsigned char* modelState = bodyComp + o.ModelChain.m_skeletonInstance + o.ModelChain.m_modelState;
		const char* name = *(const char**)(modelState + o.ModelChain.m_ModelName);
		if ((uintptr_t)name <= 0x10000) return;
		size_t i = 0;
		for (; name[i] && i + 1 < outSize; ++i) out[i] = name[i];
		out[i] = '\0';
	} __except (1) {
		if (outSize) out[0] = '\0';
	}
}

// SEH-guarded read of an entity's LIVE rendered mesh-group mask -- see ReadEntityMeshGroupMask in
// CosmeticModelSwap.cpp (shared with viewmodel mirror diagnostics).


// SEH-isolated vtable call for the OPTIONAL forced re-composite (see CosmeticOverrideSystem.h /
// SetRecompose). POD-only body (no C++ objects) so __try/__except is permitted; a wrong index
// access-violates HERE and is reported via the return value instead of crashing. Signature matches
// CBasePlayerWeapon::UpdateComposite(bool) -- on x64 __thiscall == __fastcall. (1 ==
// EXCEPTION_EXECUTE_HANDLER; the literal avoids pulling in <windows.h> and its macro clashes.)
typedef void* (__fastcall* UpdateComposite_t)(void* thisptr, int arg);
bool CosmeticSafeVCall(void* obj, int idx, int arg) {
	__try {
		void** vt = *(void***)obj;
		void* fn = vt[idx];
		if (!fn) return false;
		((UpdateComposite_t)fn)(obj, arg);
		return true;
	} __except (1) {
		return false;
	}
}

bool CosmeticTryReadWeaponEconInfo(unsigned char* w, ptrdiff_t offXuidLow, ptrdiff_t offXuidHigh,
	ptrdiff_t offAttrMgr, ptrdiff_t offItem, ptrdiff_t offDef, WeaponEconRead* out) {
	__try {
		uint32_t xLow = *(uint32_t*)(w + offXuidLow);
		uint32_t xHigh = *(uint32_t*)(w + offXuidHigh);
		out->xuid = ((uint64_t)xHigh << 32) | (uint64_t)xLow;
		out->itemView = w + offAttrMgr + offItem;
		out->liveDef = (int)*(uint16_t*)(out->itemView + offDef);
		return true;
	} __except (1) {
		return false;
	}
}


// THE real skin write (Phase 2 breakthrough). A networked demo weapon stores its actual skin in
// m_NetworkedDynamicAttributes (def 6 = paint kit, 7 = seed, 8 = wear, 80 = StatTrak) -- the
// m_nFallback* fields are only consulted when the item has NO networked attributes, so on a demo
// item they are ignored. This OVERWRITES the matching attribute VALUES in place (it cannot add new
// ones -- a missing def, e.g. StatTrak on a non-ST gun, is skipped). Walks the same vector layout as
// CosmeticDebug.cpp::ReadAttrList. POD-only body so __try/__except is permitted. vectorField =
// itemView + networkedOff + attrSubOff (the CAttributeList::m_Attributes vector).
void CosmeticWriteNetworkedSkinAttributes(unsigned char* itemView, ptrdiff_t networkedOff, ptrdiff_t attrSubOff,
	int stride, ptrdiff_t defOff, ptrdiff_t valOff, int paintKit, int seed, float wear, int statTrak,
	AttrWriteResult* out) {
	*out = AttrWriteResult{};
	if (networkedOff == 0 || attrSubOff == 0 || defOff == 0 || valOff == 0 || stride <= 0)
		return;
	unsigned char* vectorField = itemView + networkedOff + attrSubOff;
	__try {
		// Accept both observed vector layouts (count+ptr at +0/+8, or ptr+count at +0/+16), the same
		// tolerance as ReadAttrList.
		int count = *(int*)vectorField;
		unsigned char* data = *(unsigned char**)(vectorField + 8);
		if (!(count > 0 && count <= 128 && (uintptr_t)data > 0x10000)) {
			data = *(unsigned char**)vectorField;
			count = *(int*)(vectorField + 16);
		}
		if (!(count > 0 && count <= 128 && (uintptr_t)data > 0x10000)) {
			out->ok = true; // valid but empty -- nothing to overwrite
			return;
		}
		for (int i = 0; i < count; ++i) {
			unsigned char* attr = data + (ptrdiff_t)i * stride;
			int def = (int)*(uint16_t*)(attr + defOff);
			float nv;
			if (def == 6) { nv = (float)paintKit; out->hasPaint = true; }
			else if (def == 7) nv = (float)seed;
			else if (def == 8) nv = wear;
			else if (def == 80 && statTrak >= 0) nv = (float)statTrak;
			else continue;
			float* pv = (float*)(attr + valOff);
			if (*pv != nv) { *pv = nv; out->changed = true; }
			++out->written;
		}
		out->ok = true;
	} __except (1) {
		out->ok = false;
	}
}

// The proven write recipe from MirvCosmetics::Cosmetics_RunFrame, generalized to an arbitrary
// econ entity + arbitrary fallback offsets. Every pointer/offset is resolved by the caller BEFORE
// entering this function so the __try body only ever touches POD pointers/primitives -- required
// because __try/__except may not contain C++ objects that need unwinding.
//
// w        = entity base pointer (unsigned char*)
// itemView = w + m_AttributeManager + m_Item (the embedded C_EconItemView)
// offItemIdHigh/offPaint/offWear/offSeed/offStat = C_EconEntity fallback field offsets (from w)
// offDef                                          = C_EconItemView::m_iItemDefinitionIndex (from itemView)
// offItemIdLow/offAccountId                       = optional (0 = skip), from itemView
// offInit/offInitTags                             = optional (0 = skip), from itemView
// offRestoreMaterial/offImageReq/offImageTried/offCachedFile = optional item-view cache hints
// offAttrParity                                  = optional CAttributeManager cache hint
// offVisualsData/offClearUgc/offReloadEvent      = optional C_CSWeaponBase visuals-cache hints
// knifeDefOverride > 0 requests a def/model swap (best-effort; see header comment on agent/gloves
// for what is and is not safe to do here -- this is just numeric def swap, not a model load).
// markStaleEnabled = do the visuals-stale field writes when a value changed (per-frame auto-rebuild).
// forceStale       = do them unconditionally (the manual "rebuild once" path), even when unchanged.
ApplyResult CosmeticApplyWrite(
	unsigned char* w,
	unsigned char* itemView,
	unsigned char* attrManager,
	ptrdiff_t offItemIdHigh,
	ptrdiff_t offPaint,
	ptrdiff_t offWear,
	ptrdiff_t offSeed,
	ptrdiff_t offStat,
	ptrdiff_t offDef,
	ptrdiff_t offItemIdLow,
	ptrdiff_t offAccountId,
	ptrdiff_t offInit,
	ptrdiff_t offInitTags,
	ptrdiff_t offAttrInit,
	ptrdiff_t offRestoreMaterial,
	ptrdiff_t offImageReq,
	ptrdiff_t offImageTried,
	ptrdiff_t offCachedFile,
	ptrdiff_t offAttrParity,
	ptrdiff_t offVisualsData,
	ptrdiff_t offClearUgc,
	ptrdiff_t offReloadEvent,
	int32_t paintKit,
	float wear,
	int32_t seed,
	int32_t statTrak,
	uint32_t accountId,
	int knifeDefOverride,
	bool writeFallbackId,
	bool markStaleEnabled,
	bool forceStale,
	CosmeticRebuildFlags flags) {
	__try {
		int32_t* pItemIdHigh = (int32_t*)(itemView + offItemIdHigh);
		int32_t* pPaint = (int32_t*)(w + offPaint);
		float* pWear = (float*)(w + offWear);
		int32_t* pSeed = (int32_t*)(w + offSeed);
		int32_t* pStat = (int32_t*)(w + offStat);
		uint16_t* pDef = (uint16_t*)(itemView + offDef);

		bool restoreItemId = !writeFallbackId && (*pItemIdHigh == -1);
		bool reverted = writeFallbackId && (*pItemIdHigh != -1);
		bool need = reverted
			|| restoreItemId
			|| (*pPaint != paintKit)
			|| (*pWear != wear)
			|| (*pSeed != seed)
			|| (*pStat != statTrak)
			|| (knifeDefOverride > 0 && *pDef != (uint16_t)knifeDefOverride);

		// itemIDHigh=-1 forces the legacy fallback-field path. Opt-in only: it INVALIDATES the item
		// id (breaks the UI inventory read -- "no AK") and is unnecessary now that we overwrite the
		// networked attributes directly. Default OFF (see m_fallbackId / "cosmetics fallback").
		if (writeFallbackId) {
			*pItemIdHigh = -1;
			if (offAccountId) *(uint32_t*)(itemView + offAccountId) = accountId; // best-effort
			if (offItemIdLow) *(uint32_t*)(itemView + offItemIdLow) = 0;         // best-effort
		} else if (restoreItemId) {
			// Older builds / live diagnostics could leave demo weapons in fallback mode, which makes
			// CS2's loadout HUD lose the real weapon icon. In normal mode keep the demo item id valid.
			*pItemIdHigh = 0;
		}
		*pPaint = paintKit;
		*pWear = wear;
		*pSeed = seed;
		*pStat = statTrak;
		if (knifeDefOverride > 0) *pDef = (uint16_t)knifeDefOverride; // best-effort knife DEF swap

		// The visuals-stale field writes. Gated on a value changing AND per-frame auto-rebuild being on
		// (markStaleEnabled), OR forceStale (the "rebuild once" path) which re-asserts unconditionally.
		// EACH individual write is ALSO gated on its rebuild flag (all default OFF) so the harmful ones
		// (clearUgc/initialized blank the HUD weapon icon) are not written unless explicitly enabled for
		// research -- see CosmeticRebuildFlags / "cosmetics rebuildflags". Default = none -> HUD-safe.
		bool doStaleWrites = (need && markStaleEnabled) || forceStale;
		if (doStaleWrites) {
			if (flags.initialized) {
				if (offInit) *(bool*)(itemView + offInit) = false;
				if (offInitTags) *(bool*)(itemView + offInitTags) = false;
			}
			if (flags.imageCache) {
				if (offRestoreMaterial) *(bool*)(itemView + offRestoreMaterial) = true;
				if (offImageReq) *(bool*)(itemView + offImageReq) = false;
				if (offImageTried) *(bool*)(itemView + offImageTried) = false;
				if (offCachedFile) *(char*)(itemView + offCachedFile) = 0;
			}
			if (flags.attrParity && attrManager && offAttrParity) {
				int32_t* parity = (int32_t*)(attrManager + offAttrParity);
				*parity = *parity + 1;
			}
			if (flags.visualsDataSet && offVisualsData) *(bool*)(w + offVisualsData) = false;
			if (flags.clearUgc && offClearUgc) *(bool*)(w + offClearUgc) = true;
			if (flags.reloadEvent && offReloadEvent) {
				// Set m_nCustomEconReloadEventId NEGATIVE (not ++). Binary analysis of the live client.dll
				// (Docs/archive/cosmetics-recompose-research.md, 2026-06-28) shows C_CSWeaponBase vtable slot 11
				// (the econ OnDataChanged path) only requests a "clientside_reload_custom_econ" composite
				// rebuild when this field is < 0 ("cmp dword [weapon+0x18bc],0 / jge skip"); a positive value
				// (the old ++ bump) is the skip case, which is why it never rebuilt. -1 == "reload me".
				int32_t* reloadEvent = (int32_t*)(w + offReloadEvent);
				*reloadEvent = -1;
			}
			// Clear C_EconEntity::m_bAttributesInitialized on the WEAPON (offset from w, NOT itemView)
			// to try to force an econ attribute / composite re-init. See Docs/archive/cosmetics-recompose-research.md.
			if (flags.attrInit && offAttrInit) *(bool*)(w + offAttrInit) = false;
		}

		ApplyResult r;
		r.patched = true;
		r.reverted = (reverted && need);
		r.needComposite = need;
		return r;
	} __except (1) {
		// A misclassified entity / bad offset access-violates HERE and is swallowed instead of
		// crashing the game (1 == EXCEPTION_EXECUTE_HANDLER; the literal avoids <windows.h> macro
		// clashes the same way MirvCosmetics::CosmeticSafeVCall does).
		return ApplyResult{};
	}
}

} // namespace Filmmaker
