// Diagnostics for the SteamID-keyed cosmetic override system: "mirv_filmmaker cosmetics status"
// and the spectated-player econ dump used while wiring up new overrides. Read-only: never writes
// to any entity. Entity field reads are SEH-guarded (POD-only bodies inside __try/__except) the
// same way CosmeticOverrideSystem.cpp's ApplyCosmeticWrite / MirvCosmetics::DebugWeapon are, since
// a stale/misclassified entity pointer here would otherwise access-violate the game process.
//
// This TU is the diagnostics PRINTING half of the debug subsystem and the home of the shared
// SEH-guarded readers (CosmeticDebugRead). The structured mvm_debug EVENT loggers live in
// CosmeticDebugEvents.cpp -- see the map in CosmeticDebugInternal.h.

#include "CosmeticOverrideSystem.h"
#include "CosmeticCatalog.h"
#include "CosmeticModelSwap.h"
#include "CosmeticDirectComposite.h"
#include "CosmeticDebugInternal.h" // the shared SEH-guarded POD readers implemented below

#include "../Platform/TextEncoding.h"

#include "../../ClientEntitySystem.h" // CEntityInstance, entity-list globals, CBaseHandle
#include "../../SchemaSystem.h"        // g_clientDllOffsets, g_cosmeticsOffsetsOk
#include "../../DeathMsg.h"            // g_CurrentGameCamera (observer probe)
#include "../Filmmaker.h"              // CameraTimeline_NativeCameraMode (observer probe)
#include "../Debug/MvmDebugLog.h"

#include "../../../Deps/release/prop/cs2/sdk_src/public/cdll_int.h"

#include "../../../Shared/AfxConsole.h"

#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

extern SOURCESDK::CS2::ISource2EngineToClient* g_pEngineToClient;

namespace Filmmaker {

// ---- Shared SEH-guarded POD readers (declared in CosmeticDebugInternal.h) --------------------------
// Used by both this TU's printers and CosmeticDebugEvents.cpp's loggers.

namespace CosmeticDebugRead {

// Same bounds-checked resolve used by CosmeticOverrideSystem.cpp / MirvCosmetics.cpp.
CEntityInstance* EntFromIndex(int index) {
	if (index < 0 || index > GetHighestEntityIndex() || !g_pEntityList || !*g_pEntityList || !g_GetEntityFromIndex)
		return nullptr;
	return (CEntityInstance*)g_GetEntityFromIndex(*g_pEntityList, index);
}

// Walks the embedded attribute vector at (itemView + listOff + C_AttributeList.m_Attributes) and
// copies up to 24 {defIndex,value} pairs out. Accepts both observed vector layouts (count+ptr and
// ptr+count), the same tolerance as CameraEditorHud.cpp::TryMakeAttributeVector. SEH-guarded: a bad
// pointer/offset faults HERE and is swallowed. listOff==0 means "field not present on this build".
void ReadAttrList(unsigned char* itemView, ptrdiff_t listOff, AttrListDump* out) {
	*out = AttrListDump{};
	const ClientDllOffsets_t& o = g_clientDllOffsets;
	if (listOff == 0 || o.C_AttributeList.m_Attributes == 0
		|| o.CEconItemAttribute.m_iAttributeDefinitionIndex == 0 || o.CEconItemAttribute.m_flValue == 0)
		return;
	out->resolved = true;

	int stride = (int)o.CEconItemAttribute.m_size;
	int minStride = (int)o.CEconItemAttribute.m_flValue + (int)sizeof(float);
	if (stride < minStride) stride = minStride;

	unsigned char* vectorField = itemView + listOff + o.C_AttributeList.m_Attributes;
	__try {
		int count = *(int*)vectorField;
		unsigned char* data = *(unsigned char**)(vectorField + 8);
		if (!(count > 0 && count <= 128 && (uintptr_t)data > 0x10000)) {
			data = *(unsigned char**)vectorField;
			count = *(int*)(vectorField + 16);
		}
		if (!(count > 0 && count <= 128 && (uintptr_t)data > 0x10000)) {
			out->ok = true; // a valid but EMPTY list (count 0) -- distinguish from a fault
			out->count = 0;
			return;
		}
		int n = count > 24 ? 24 : count;
		for (int i = 0; i < n; ++i) {
			unsigned char* attr = data + (ptrdiff_t)i * stride;
			out->items[i].def = (int)*(uint16_t*)(attr + o.CEconItemAttribute.m_iAttributeDefinitionIndex);
			out->items[i].val = *(float*)(attr + o.CEconItemAttribute.m_flValue);
		}
		out->count = n;
		out->ok = true;
	} __except (1) {
		out->ok = false;
	}
}

// Extracts a specific attribute def's value out of an already-read AttrListDump. Returns false if the
// def is not present (e.g. StatTrak on a non-ST gun). The list reader already SEH-guarded the read.
bool AttrValueForDef(const AttrListDump& d, int def, float* outVal) {
	if (!d.ok) return false;
	for (int i = 0; i < d.count; ++i) {
		if (d.items[i].def == def) { if (outVal) *outVal = d.items[i].val; return true; }
	}
	return false;
}

// Reads everything in WeaponVisualDiag off a weapon entity. Caller resolves itemView; this only
// touches POD pointers inside __try. Optional offsets (the C_CSWeaponBase visuals flags) are gated by
// != 0 so a missing field reports have*=false instead of reading a bogus address.
void ReadWeaponVisualDiag(unsigned char* w, unsigned char* itemView, WeaponVisualDiag* out) {
	*out = WeaponVisualDiag{};
	const ClientDllOffsets_t& o = g_clientDllOffsets;
	__try {
		uint32_t xLow = *(uint32_t*)(w + o.C_EconEntity.m_OriginalOwnerXuidLow);
		uint32_t xHigh = *(uint32_t*)(w + o.C_EconEntity.m_OriginalOwnerXuidHigh);
		out->ownerXuid = ((uint64_t)xHigh << 32) | (uint64_t)xLow;
		out->defIndex = (int)*(uint16_t*)(itemView + o.C_EconItemView.m_iItemDefinitionIndex);
		out->itemIdHigh = *(int32_t*)(itemView + o.C_EconItemView.m_iItemIDHigh);
		if (o.C_EconItemView.m_iItemIDLow) out->itemIdLow = *(uint32_t*)(itemView + o.C_EconItemView.m_iItemIDLow);
		if (o.C_EconItemView.m_iAccountID) out->accountId = *(uint32_t*)(itemView + o.C_EconItemView.m_iAccountID);
		out->fbPaint = *(int32_t*)(w + o.C_EconEntity.m_nFallbackPaintKit);
		out->fbWear = *(float*)(w + o.C_EconEntity.m_flFallbackWear);
		out->fbSeed = *(int32_t*)(w + o.C_EconEntity.m_nFallbackSeed);
		out->fbStat = *(int32_t*)(w + o.C_EconEntity.m_nFallbackStatTrak);
		if (o.C_CSWeaponBase.m_bVisualsDataSet) {
			out->visualsDataSet = *(unsigned char*)(w + o.C_CSWeaponBase.m_bVisualsDataSet);
			out->haveVisualsData = true;
		}
		if (o.C_CSWeaponBase.m_bClearWeaponIdentifyingUGC) {
			out->clearUgc = *(unsigned char*)(w + o.C_CSWeaponBase.m_bClearWeaponIdentifyingUGC);
			out->haveClearUgc = true;
		}
		if (o.C_CSWeaponBase.m_nCustomEconReloadEventId) {
			out->reloadEvent = *(int32_t*)(w + o.C_CSWeaponBase.m_nCustomEconReloadEventId);
			out->haveReloadEvent = true;
		}
		if (o.C_EconEntity.m_bAttributesInitialized) {
			out->attrInit = *(unsigned char*)(w + o.C_EconEntity.m_bAttributesInitialized);
			out->haveAttrInit = true;
		}
		out->ok = true;
	} __except (1) {
		out->ok = false;
	}
}

// Walks the read-only model-state chain on a player pawn and copies its model path
// (CModelState::m_ModelName) into a POD buffer. SEH-guarded: a bad pointer/offset faults here and is
// swallowed. Chain: pawn + m_CBodyComponent(ptr deref) + m_skeletonInstance + m_modelState + m_ModelName.
void ReadPawnModelName(int pawnIndex, ModelNameResult* out) {
	*out = ModelNameResult{};
	CEntityInstance* pawn = EntFromIndex(pawnIndex);
	if (!pawn || !pawn->IsPlayerPawn())
		return;
	const ClientDllOffsets_t& o = g_clientDllOffsets;
	if (o.ModelChain.m_CBodyComponent == 0 || o.ModelChain.m_skeletonInstance == 0
		|| o.ModelChain.m_modelState == 0 || o.ModelChain.m_ModelName == 0)
		return;
	unsigned char* p = (unsigned char*)pawn;
	__try {
		unsigned char* bodyComp = *(unsigned char**)(p + o.ModelChain.m_CBodyComponent);
		if ((uintptr_t)bodyComp > 0x10000) {
			unsigned char* modelState = bodyComp + o.ModelChain.m_skeletonInstance + o.ModelChain.m_modelState;
			const char* name = *(const char**)(modelState + o.ModelChain.m_ModelName);
			if ((uintptr_t)name > 0x10000) {
				size_t i = 0;
				for (; name[i] && i + 1 < sizeof(out->name); ++i)
					out->name[i] = name[i];
				out->name[i] = '\0';
				out->ok = (i > 0);
			}
		}
	} __except (1) {
		out->ok = false;
	}
}

// Generalized model-name read: same CModelState::m_ModelName chain as ReadPawnModelName, but for ANY
// entity (a weapon entity is a C_BaseAnimGraph and carries the same body-component/skeleton chain),
// so we can dump the weapon's own world-model path. SEH-guarded POD body.
void ReadEntityModelName(CEntityInstance* ent, ModelNameResult* out) {
	*out = ModelNameResult{};
	if (!ent)
		return;
	const ClientDllOffsets_t& o = g_clientDllOffsets;
	if (o.ModelChain.m_CBodyComponent == 0 || o.ModelChain.m_skeletonInstance == 0
		|| o.ModelChain.m_modelState == 0 || o.ModelChain.m_ModelName == 0)
		return;
	unsigned char* p = (unsigned char*)ent;
	__try {
		unsigned char* bodyComp = *(unsigned char**)(p + o.ModelChain.m_CBodyComponent);
		if ((uintptr_t)bodyComp > 0x10000) {
			unsigned char* modelState = bodyComp + o.ModelChain.m_skeletonInstance + o.ModelChain.m_modelState;
			const char* name = *(const char**)(modelState + o.ModelChain.m_ModelName);
			if ((uintptr_t)name > 0x10000) {
				size_t i = 0;
				for (; name[i] && i + 1 < sizeof(out->name); ++i)
					out->name[i] = name[i];
				out->name[i] = '\0';
				out->ok = (i > 0);
			}
		}
	} __except (1) {
		out->ok = false;
	}
}

const char* SlotLabel(CosmeticSlot slot) {
	switch (slot) {
	case CosmeticSlot::Primary: return "primary";
	case CosmeticSlot::Secondary: return "secondary";
	case CosmeticSlot::Knife: return "knife";
	case CosmeticSlot::Gloves: return "gloves";
	case CosmeticSlot::Agent: return "agent";
	default: return "none";
	}
}

} // namespace CosmeticDebugRead

using namespace CosmeticDebugRead;

// ---- Print-only helpers ----------------------------------------------------------------------------

namespace {

// POD result of the SEH-guarded weapon-econ read -- no constructors, safe to populate inside
// __try/__except (which may not contain C++ objects that need unwinding).
struct WeaponDebugInfo {
	bool ok = false;
	int weaponIndex = -1;
	char className[64] = {};
	uint64_t ownerXuid = 0;
	int itemDefIndex = 0;
	int32_t itemIdHigh = 0;
	int32_t fallbackPaintKit = 0;
	float fallbackWear = 0.0f;
};

// Resolves the spectated pawn's active weapon and SEH-guarded-reads its econ fields. Mirrors
// MirvCosmetics::Cosmetics_DebugWeapon, generalized to also report the original-owner XUID (which
// is what the apply loop keys profiles off of) and the weapon's class name.
bool ReadWeaponDebugInfo(int pawnIndex, WeaponDebugInfo* out) {
	if (!out || !g_cosmeticsOffsetsOk)
		return false;

	CEntityInstance* pawn = EntFromIndex(pawnIndex);
	if (!pawn || !pawn->IsPlayerPawn())
		return false;

	SOURCESDK::CS2::CBaseHandle wh = pawn->GetActiveWeaponHandle();
	if (!wh.IsValid())
		return false;

	CEntityInstance* weapon = EntFromIndex(wh.GetEntryIndex());
	if (!weapon)
		return false;

	// GetClassName() returns a C++ const char*; copy it into the POD struct BEFORE entering the
	// __try so the guarded block only ever touches raw pointers/primitives.
	const char* cls = weapon->GetClassName();
	out->weaponIndex = wh.GetEntryIndex();
	out->className[0] = '\0';
	if (cls) {
		size_t i = 0;
		for (; cls[i] != '\0' && i + 1 < sizeof(out->className); ++i)
			out->className[i] = cls[i];
		out->className[i] = '\0';
	}

	const ClientDllOffsets_t& o = g_clientDllOffsets;
	unsigned char* w = (unsigned char*)weapon;

	__try {
		unsigned char* itemView = w + o.C_EconEntity.m_AttributeManager + o.C_AttributeContainer.m_Item;
		uint32_t xLow = *(uint32_t*)(w + o.C_EconEntity.m_OriginalOwnerXuidLow);
		uint32_t xHigh = *(uint32_t*)(w + o.C_EconEntity.m_OriginalOwnerXuidHigh);
		out->ownerXuid = ((uint64_t)xHigh << 32) | (uint64_t)xLow;
		out->itemDefIndex = (int)*(uint16_t*)(itemView + o.C_EconItemView.m_iItemDefinitionIndex);
		out->itemIdHigh = *(int32_t*)(itemView + o.C_EconItemView.m_iItemIDHigh);
		out->fallbackPaintKit = *(int32_t*)(w + o.C_EconEntity.m_nFallbackPaintKit);
		out->fallbackWear = *(float*)(w + o.C_EconEntity.m_flFallbackWear);
		out->ok = true;
	} __except (1) {
		// A bad offset / misclassified entity access-violates HERE; swallow it instead of taking
		// down the game (1 == EXCEPTION_EXECUTE_HANDLER; the literal avoids <windows.h> macro
		// clashes, matching MirvCosmetics::SafeVCall / CosmeticOverrideSystem::ApplyCosmeticWrite).
		out->ok = false;
	}
	return out->ok;
}

void PrintAttrList(const char* cmd, const char* label, ptrdiff_t listOff, const AttrListDump& d) {
	if (!d.resolved) {
		advancedfx::Message("    %s: (offset unresolved -- field not on this build)\n", label);
		return;
	}
	if (!d.ok) {
		advancedfx::Message("    %s @+0x%zx: (read faulted)\n", label, (size_t)listOff);
		return;
	}
	if (d.count == 0) {
		advancedfx::Message("    %s @+0x%zx: (empty)\n", label, (size_t)listOff);
		return;
	}
	advancedfx::Message("    %s @+0x%zx: %d attr(s)\n", label, (size_t)listOff, d.count);
	for (int i = 0; i < d.count; ++i) {
		uint32_t bits = 0;
		std::memcpy(&bits, &d.items[i].val, sizeof(bits));
		advancedfx::Message("      def=%d value=%.4f bits=%08x\n", d.items[i].def, d.items[i].val, bits);
	}
}

// Resolves the spectated pawn's active weapon and dumps BOTH attribute lists (local + networked)
// so we can see where a networked/demo player's paint kit actually lives. Read-only.
void DumpSpectatedWeaponAttributes(int pawnIndex, const char* cmd) {
	if (!g_cosmeticsOffsetsOk)
		return;
	CEntityInstance* pawn = EntFromIndex(pawnIndex);
	if (!pawn || !pawn->IsPlayerPawn())
		return;
	SOURCESDK::CS2::CBaseHandle wh = pawn->GetActiveWeaponHandle();
	if (!wh.IsValid())
		return;
	CEntityInstance* weapon = EntFromIndex(wh.GetEntryIndex());
	if (!weapon)
		return;

	const ClientDllOffsets_t& o = g_clientDllOffsets;
	unsigned char* w = (unsigned char*)weapon;
	unsigned char* itemView = w + o.C_EconEntity.m_AttributeManager + o.C_AttributeContainer.m_Item;

	AttrListDump local, networked;
	ReadAttrList(itemView, o.C_EconItemView.m_AttributeList, &local);
	ReadAttrList(itemView, o.C_EconItemView.m_NetworkedDynamicAttributes, &networked);

	advancedfx::Message("  attribute lists (paintKit=def6, seed=def7, wear=def8, stattrak=def80):\n");
	PrintAttrList(cmd, "m_AttributeList", o.C_EconItemView.m_AttributeList, local);
	PrintAttrList(cmd, "m_NetworkedDynamicAttributes", o.C_EconItemView.m_NetworkedDynamicAttributes, networked);
}

void PrintItemLine(const char* label, const CosmeticItem& item) {
	if (!item.set)
		return;
	advancedfx::Message("    %s def=%d paint=%d wear=%.4f seed=%d st=%d\n",
		label, item.defIndex, item.paintKit, item.wear, item.seed, item.statTrak);
}

} // namespace

void CosmeticOverrideSystem::PrintRecordedWeapons(uint32_t fullHandle) const {
    advancedfx::Message("cosmetics recorded: snapshots=%zu pending=%zu materialBusy=%d filter=%u (journal only; no live reads)\n",
        RecordedWeaponSnapshots(), RecordedWeaponPending(), WeaponMaterialBusy() ? 1 : 0, fullHandle);
    for (const auto& entry : m_recordedWeapons) {
        const auto& saved = entry.second;
        if (fullHandle != ~0u && saved.handle != fullHandle) continue;
        advancedfx::Message("  handle=%u entity=%p owner=%llu profileOwner=%llu def=%d pending=%d restored=%d meshSelected=%d due=%llu lastComposite=%llu mesh=%llu\n",
            saved.handle, (void*)saved.entity, (unsigned long long)saved.owner, (unsigned long long)saved.profileOwner,
            saved.definition, saved.pending ? 1 : 0, saved.restored ? 1 : 0, saved.meshSelected ? 1 : 0,
            (unsigned long long)saved.dueFrame, (unsigned long long)saved.lastCompositeFrame, (unsigned long long)saved.mesh);
        if (fullHandle == ~0u) continue; // default enumeration stays compact for dense demos
        const char* rawNames[] = {"itemHigh", "itemLow", "account", "paint", "wearBits", "seed", "stat"};
        for (unsigned i = 0; i < 7; ++i) {
            const auto& owned = saved.rawOwned[i];
            advancedfx::Message("    %s original=0x%08x confirmed=%d:0x%08llx attempted=%d:0x%08llx\n", rawNames[i],
                saved.Raw(i), owned.haveConfirmed ? 1 : 0, (unsigned long long)owned.confirmed,
                owned.haveAttempted ? 1 : 0, (unsigned long long)owned.attempted);
        }
        for (unsigned i = 0; i < 4; ++i) {
            const auto& net = saved.networkOwned[i];
            const auto& local = saved.localOwned[i];
            advancedfx::Message("    attr=%u network={original=0x%llx confirmed=%d:0x%llx attempted=%d:0x%llx} local={original=0x%llx confirmed=%d:0x%llx attempted=%d:0x%llx}\n",
                CosmeticThemeAttributes::Definition(i), (unsigned long long)saved.networked.Encoded(i),
                net.haveConfirmed ? 1 : 0, (unsigned long long)net.confirmed, net.haveAttempted ? 1 : 0, (unsigned long long)net.attempted,
                (unsigned long long)saved.local.Encoded(i), local.haveConfirmed ? 1 : 0, (unsigned long long)local.confirmed,
                local.haveAttempted ? 1 : 0, (unsigned long long)local.attempted);
        }
        advancedfx::Message("    renderer target={confirmed=%d:%llu attempted=%d:%llu} scratch={confirmed=%d:%llu attempted=%d:%llu}\n",
            saved.meshOwned.haveConfirmed ? 1 : 0, (unsigned long long)saved.meshOwned.confirmed,
            saved.meshOwned.haveAttempted ? 1 : 0, (unsigned long long)saved.meshOwned.attempted,
            saved.meshScratchOwned.haveConfirmed ? 1 : 0, (unsigned long long)saved.meshScratchOwned.confirmed,
            saved.meshScratchOwned.haveAttempted ? 1 : 0, (unsigned long long)saved.meshScratchOwned.attempted);
    }
}

void Cosmetics_PrintStatus(const char* cmd) {
	CosmeticOverrideSystem& sys = CosmeticsRef();
	const CosmeticFrameStats& stats = sys.LastFrameStats();

	advancedfx::Message("%s cosmetics: enabled=%d armed=%d offsetsResolved=%d demoContext=%d debug=%d\n",
		cmd, sys.Enabled() ? 1 : 0, sys.Armed() ? 1 : 0, sys.OffsetsAvailable() ? 1 : 0, sys.InDemoContext() ? 1 : 0, sys.Debug() ? 1 : 0);

	advancedfx::Message("  theme state: active=%d savedProfiles=%zu savedBots=%zu pawnModels=%zu pawnPending=%zu\n",
		sys.ThemeActive() ? 1 : 0, sys.ThemeSavedProfiles(), sys.ThemeSavedBots(), sys.ThemeOriginalModels(), sys.ThemePawnPending());
	advancedfx::Message("  recorded weapons: snapshots=%zu pending=%zu materialBusy=%d (details: cosmetics recorded [fullHandle])\n",
		sys.RecordedWeaponSnapshots(), sys.RecordedWeaponPending(), sys.WeaponMaterialBusy() ? 1 : 0);
	advancedfx::Message("  live picks: queued=%zu nudgePhase=%d nudgePending=%d owedPause=%zu owedComposite=%zu\n",
		sys.PendingLivePicks(), sys.NudgePhase(), sys.NudgePending() ? 1 : 0,
		sys.QueuedOwedPauseCount(), sys.OwedCompositeCount());

	advancedfx::Message("  seek refresh: pending=%d hold=%d\n",
		sys.SeekRefreshPending() ? 1 : 0, sys.SeekHoldActive() ? 1 : 0);
	const std::string upath = WideToUtf8(sys.ProjectDemoPath());
	advancedfx::Message("  profiles=%zu saved in the project of '%s'\n", sys.Store().All().size(), upath.c_str());

	advancedfx::Message("  lastFrame: scanned=%d matched=%d patched=%d reverted=%d frame=%llu\n",
		stats.entitiesScanned, stats.entitiesMatched, stats.entitiesPatched, stats.entitiesReverted,
		(unsigned long long)stats.frame);
	advancedfx::Message("  attrs: listsRead=%d valuesWritten=%d valuesChanged=%d empty=%d namedSetter=%d\n",
		stats.attrListsRead, stats.attrValuesWritten, stats.attrValuesChanged, stats.attrListsEmpty,
		stats.namedSetterApplied);

	int vtComp = 0, vtSec = 0;
	sys.GetVtIdx(&vtComp, &vtSec);
	advancedfx::Message("  recompose=%d faulted=%d vtComp=%d vtSec=%d vtArg=%d fallbackId=%d\n",
		sys.Recompose() ? 1 : 0, sys.RecomposeFaulted() ? 1 : 0, vtComp, vtSec, sys.VtArg(), sys.FallbackId() ? 1 : 0);
	advancedfx::Message("  paintkitbridge=%d cvarFound=%d value=%d forced=%d (global deploy-time cl_paintkit_override)\n",
		sys.PaintkitBridge() ? 1 : 0, sys.PaintkitBridgeCvarFound() ? 1 : 0,
		sys.PaintkitBridgeLastValue(), sys.PaintkitBridgeForcedValue());
	advancedfx::Message("  directComposite: resolved=%d calls=%d faulted=%d ownerOffset=0x%llx rttiOwnerOffset=%lld\n",
		stats.directCompositeResolved, stats.directCompositeCalls, stats.directCompositeFaulted,
		(unsigned long long)sys.CompositeOwnerOffset(), (long long)DirectCompositeOwnerOffset());
	const char* meshModeStr = sys.MeshLegacyMode() == -2 ? "auto" : (sys.MeshLegacyMode() == -1 ? "modern" : "legacy");
	advancedfx::Message("  modelswap=%d knifeType=%d resolved=%d  mesh=%s(modern=%llu legacy=%llu)\n",
		sys.ModelSwap() ? 1 : 0, sys.KnifeModelSwap() ? 1 : 0,
		sys.ModelSwapResolved() ? 1 : 0, meshModeStr,
		(unsigned long long)sys.MaskModern(), (unsigned long long)sys.MaskLegacy());
	advancedfx::Message("  lastFrame modelSwap: knife=%d weaponMesh=%d pawns=%d gloves=%d agents=%d\n",
		stats.knifeModelsApplied, stats.weaponMeshFixed, stats.pawnsScanned, stats.glovesApplied, stats.agentsApplied);
	advancedfx::Message("  ticknudge=%d ticks=%d totalNudges=%llu (auto play-out so body swaps re-render)\n",
		sys.TickNudge() ? 1 : 0, sys.TickNudgeTicks(), (unsigned long long)sys.TotalNudges());
	advancedfx::Message("  totalApplied: knife=%llu weaponMesh=%llu gloves=%llu agents=%llu (cumulative, proof of execution)\n",
		(unsigned long long)sys.TotalKnifeApplied(), (unsigned long long)sys.TotalWeaponMeshApplied(),
		(unsigned long long)sys.TotalGlovesApplied(), (unsigned long long)sys.TotalAgentsApplied());
	const ModelSwapResolveStatus& ms = ResolveModelSwapFns();
	advancedfx::Message("  modelSwapFns: setModel=%d meshMask=%d subclass=%d bodyGroup=%d bodyGroupChoice=%d staticData=%d econSchema=%d\n",
		ms.setModel ? 1 : 0, ms.setMeshGroupMask ? 1 : 0, ms.updateSubclass ? 1 : 0, ms.setBodyGroup ? 1 : 0,
		ms.updateBodyGroupChoice ? 1 : 0, ms.getStaticData ? 1 : 0, ms.econItemSystem ? 1 : 0);

	if (sys.Store().All().empty())
		advancedfx::Message("  (no profiles)\n");

	for (const auto& [steamId, profile] : sys.Store().All()) {
		advancedfx::Message("  %llu '%s':\n", (unsigned long long)steamId, profile.name.c_str());
		// Per-weapon overrides, one line each (label by def index so each gun is distinguishable).
		for (const auto& [def, item] : profile.weapons) {
			if (!item.set)
				continue;
			char label[32];
			std::snprintf(label, sizeof(label), "weapon[%d]", def);
			PrintItemLine(label, item);
		}
		PrintItemLine("knife", profile.knife);
		if (profile.gloves.set) {
			advancedfx::Message("    gloves def=%d paint=%d wear=%.4f seed=%d%s\n",
				profile.gloves.defIndex, profile.gloves.paintKit, profile.gloves.wear, profile.gloves.seed,
				sys.ModelSwap() ? "" : " (modelswap OFF -> not applied)");
		}
		if (profile.agent.set) {
			advancedfx::Message("    agent model='%s'%s\n", profile.agent.model.c_str(),
				sys.ModelSwap() ? "" : " (modelswap OFF -> not applied)");
		}
		if (profile.Empty())
			advancedfx::Message("    (empty)\n");
	}

	// Also dump the live spectated player's weapon econ state when watching a demo, so "status" is
	// a one-stop view (global flags + stored profiles + what the apply loop sees right now).
	if (sys.InDemoContext())
		Cosmetics_PrintSpectatedDebug(cmd);
}

void Cosmetics_PrintSpectatedDebug(const char* cmd) {
	CosmeticOverrideSystem& sys = CosmeticsRef();

	const uint8_t obsMode = sys.CurrentObserverMode();
	const int pawnIndex = sys.CurrentSpectatedPawnIndex();
	const uint64_t steamId = sys.CurrentSpectatedSteamId();
	const CosmeticProfile* profile = (steamId != 0) ? sys.Store().Find(steamId) : nullptr;

	advancedfx::Message("%s cosmetics: spectate: obsMode=%u pawn=%d steam=%llu profiled=%d\n",
		cmd, (unsigned int)obsMode, pawnIndex, (unsigned long long)steamId, profile ? 1 : 0);

	if (pawnIndex < 0) {
		advancedfx::Warning("%s cosmetics: no spectated pawn.\n", cmd);
		return;
	}
	if (!g_cosmeticsOffsetsOk) {
		advancedfx::Warning("%s cosmetics: econ offsets did not resolve; cannot read weapon state.\n", cmd);
		return;
	}

	WeaponDebugInfo info;
	if (!ReadWeaponDebugInfo(pawnIndex, &info)) {
		advancedfx::Warning("%s cosmetics: pawn=%d has no active weapon resolved (or read faulted).\n", cmd, pawnIndex);
		return;
	}

	advancedfx::Message("  weapon: index=%d class='%s' ownerXuid=%llu defIndex=%d itemIdHigh=%d paintKit=%d wear=%.4f\n",
		info.weaponIndex, info.className, (unsigned long long)info.ownerXuid, info.itemDefIndex,
		info.itemIdHigh, info.fallbackPaintKit, info.fallbackWear);

	// Dump both attribute lists so we can see where the real (networked) paint kit lives -- the
	// fallback fields above read 0 for a real demo item (itemIdHigh != -1).
	DumpSpectatedWeaponAttributes(pawnIndex, cmd);

	// Read-only agent/player-model path (for the modal's AGENT display).
	ModelNameResult mdl;
	ReadPawnModelName(pawnIndex, &mdl);
	advancedfx::Message("  agent model: %s\n", mdl.ok ? mdl.name : "(unresolved)");

	if (!profile) {
		advancedfx::Message("  (no profile for steamId=%llu -- nothing would be overridden)\n", (unsigned long long)steamId);
		return;
	}

	// Echo what the override WOULD apply for the currently-held item, for before/after visibility.
	CosmeticSlot liveSlot = CosmeticCatalog::IsKnifeDef(info.itemDefIndex)
		? CosmeticSlot::Knife
		: CosmeticCatalog::SlotForDefIndex(info.itemDefIndex);

	const CosmeticItem* wouldApply = nullptr;
	if (liveSlot == CosmeticSlot::Knife && profile->knife.set) {
		wouldApply = &profile->knife;
	} else {
		const CosmeticItem* cand = profile->FindWeapon(info.itemDefIndex);
		if (cand && cand->set)
			wouldApply = cand;
	}

	if (wouldApply) {
		advancedfx::Message("  would apply: slot=%s def=%d paint=%d wear=%.4f seed=%d st=%d\n",
			SlotLabel(liveSlot), wouldApply->defIndex, wouldApply->paintKit, wouldApply->wear,
			wouldApply->seed, wouldApply->statTrak);
	} else {
		advancedfx::Message("  would apply: (no matching slot override for the currently-held item, slot=%s)\n",
			SlotLabel(liveSlot));
	}
}

bool Cosmetics_SpectateSteamId(const char* cmd, uint64_t targetSteam) {
	CosmeticOverrideSystem& sys = CosmeticsRef();
	if (!sys.InDemoContext()) {
		advancedfx::Warning("%s cosmetics spectate: not in a demo.\n", cmd);
		return false;
	}
	if (targetSteam == 0) {
		advancedfx::Warning("%s cosmetics spectate: invalid steamId.\n", cmd);
		return false;
	}

	int specIndex = 0;
	int targetSpecIndex = -1;
	unsigned int targetHealth = 0;
	const int highest = GetHighestEntityIndex();
	for (int i = 0; i <= highest; ++i) {
		CEntityInstance* ent = EntFromIndex(i);
		if (!ent || !ent->IsPlayerController())
			continue;
		specIndex++;
		if (ent->GetSteamId() != targetSteam)
			continue;

		SOURCESDK::CS2::CBaseHandle pawnHandle = ent->GetPlayerPawnHandle();
		if (!pawnHandle.IsValid()) {
			advancedfx::Message("%s cosmetics spectate: steam=%llu dead (no pawn) -- skipped.\n",
				cmd, (unsigned long long)targetSteam);
			return false;
		}
		CEntityInstance* pawn = EntFromIndex(pawnHandle.GetEntryIndex());
		if (!pawn || !pawn->IsPlayerPawn()) {
			advancedfx::Message("%s cosmetics spectate: steam=%llu dead (pawn gone) -- skipped.\n",
				cmd, (unsigned long long)targetSteam);
			return false;
		}
		targetHealth = pawn->GetHealth();
		if (targetHealth <= 0) {
			advancedfx::Message("%s cosmetics spectate: steam=%llu dead (health=%u) -- skipped.\n",
				cmd, (unsigned long long)targetSteam, targetHealth);
			return false;
		}
		targetSpecIndex = specIndex;
		break;
	}
	if (targetSpecIndex < 0) {
		advancedfx::Warning("%s cosmetics spectate: steam=%llu not in match.\n",
			cmd, (unsigned long long)targetSteam);
		return false;
	}

	// Prefer direct observer-target write when the local viewer pawn exists; otherwise
	// fall back to a single spec_player call (only for alive targets -- never scan 1..10).
	CEntityInstance* viewerPawn = AfxGetLocalViewerPawn();
	bool usedObserverWrite = false;
	if (viewerPawn) {
		for (int i = 0; i <= highest; ++i) {
			CEntityInstance* ent = EntFromIndex(i);
			if (!ent || !ent->IsPlayerController() || ent->GetSteamId() != targetSteam)
				continue;
			SOURCESDK::CS2::CBaseHandle targetPawnHandle = ent->GetPlayerPawnHandle();
			unsigned char* viewer = (unsigned char*)viewerPawn;
			void* pObs = nullptr;
			__try {
				pObs = *(void**)(viewer + g_clientDllOffsets.C_BasePlayerPawn.m_pObserverServices);
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				pObs = nullptr;
			}
			if (pObs) {
				unsigned char* obs = (unsigned char*)pObs;
				__try {
					*(uint8_t*)(obs + g_clientDllOffsets.CPlayer_ObserverServices.m_iObserverMode) = 2;
					*(unsigned int*)(obs + g_clientDllOffsets.CPlayer_ObserverServices.m_hObserverTarget) =
						targetPawnHandle.ToInt();
					usedObserverWrite = true;
				} __except (EXCEPTION_EXECUTE_HANDLER) {
					usedObserverWrite = false;
				}
			}
			break;
		}
	}

	if (!g_pEngineToClient) {
		advancedfx::Warning("%s cosmetics spectate: engine client unavailable.\n", cmd);
		return false;
	}
	if (usedObserverWrite) {
		g_pEngineToClient->ExecuteClientCmd(0, "spec_mode 2", true);
	} else {
		char specCmd[32];
		std::snprintf(specCmd, sizeof(specCmd), "spec_player %d", targetSpecIndex);
		g_pEngineToClient->ExecuteClientCmd(0, "spec_mode 2", true);
		g_pEngineToClient->ExecuteClientCmd(0, specCmd, true);
	}

	advancedfx::Message("%s cosmetics spectate: steam=%llu specIndex=%d health=%u ok.\n",
		cmd, (unsigned long long)targetSteam, targetSpecIndex, targetHealth);
	return true;
}

void Cosmetics_PrintVisualDiag(const char* cmd) {
	CosmeticOverrideSystem& sys = CosmeticsRef();

	if (!g_cosmeticsOffsetsOk) {
		advancedfx::Warning("%s cosmetics visualdiag: econ offsets did not resolve; cannot read weapon state.\n", cmd);
		return;
	}
	if (!sys.InDemoContext()) {
		advancedfx::Warning("%s cosmetics visualdiag: not in a demo (no weapon to inspect).\n", cmd);
		return;
	}

	const int pawnIndex = sys.CurrentSpectatedPawnIndex();
	const uint64_t steamId = sys.CurrentSpectatedSteamId();
	if (pawnIndex < 0) {
		advancedfx::Warning("%s cosmetics visualdiag: no spectated pawn (spectate a player first).\n", cmd);
		return;
	}

	CEntityInstance* pawn = EntFromIndex(pawnIndex);
	if (!pawn || !pawn->IsPlayerPawn()) {
		advancedfx::Warning("%s cosmetics visualdiag: pawn=%d not resolvable.\n", cmd, pawnIndex);
		return;
	}
	const unsigned int pawnHealth = pawn->GetHealth();
	advancedfx::Message("  spectatePawn=%d spectateHealth=%u spectateSteam=%llu\n",
		pawnIndex, pawnHealth, (unsigned long long)steamId);
	if (pawnHealth <= 0) {
		advancedfx::Warning("%s cosmetics visualdiag: pawn=%d is dead (health=%u) -- skip.\n",
			cmd, pawnIndex, pawnHealth);
		return;
	}
	SOURCESDK::CS2::CBaseHandle wh = pawn->GetActiveWeaponHandle();
	CEntityInstance* weapon = wh.IsValid() ? EntFromIndex(wh.GetEntryIndex()) : nullptr;
	if (!weapon) {
		advancedfx::Warning("%s cosmetics visualdiag: pawn=%d has no active weapon resolved.\n", cmd, pawnIndex);
		return;
	}

	const ClientDllOffsets_t& o = g_clientDllOffsets;
	unsigned char* w = (unsigned char*)weapon;
	unsigned char* itemView = w + o.C_EconEntity.m_AttributeManager + o.C_AttributeContainer.m_Item;

	// Class names (read-only const char*; GetClassName is a C++ call, kept out of any SEH helper).
	const char* cls = weapon->GetClassName();
	const char* clientCls = weapon->GetClientClassName();

	// All entity field reads happen inside POD-only SEH helpers (ReadWeaponVisualDiag / ReadAttrList /
	// ReadEntityModelName) -- this function itself has NO __try, so it is free to use std::string
	// temporaries below without tripping MSVC C2712 (no __try + object-unwinding in one function).
	WeaponVisualDiag d;
	ReadWeaponVisualDiag(w, itemView, &d);

	AttrListDump networked, local;
	ReadAttrList(itemView, o.C_EconItemView.m_NetworkedDynamicAttributes, &networked);
	ReadAttrList(itemView, o.C_EconItemView.m_AttributeList, &local);

	ModelNameResult model;
	ReadEntityModelName(weapon, &model);

	advancedfx::Message("%s cosmetics visualdiag (READ-ONLY):\n", cmd);
	advancedfx::Message("  weapon: index=%d class='%s' clientClass='%s'\n",
		wh.GetEntryIndex(), cls ? cls : "(null)", clientCls ? clientCls : "(null)");
	advancedfx::Message("  activeWeaponHandle: raw=0x%08x entry=%d serial=%d valid=%d\n",
		(unsigned int)wh.ToInt(), wh.GetEntryIndex(), wh.GetSerialNumber(), wh.IsValid() ? 1 : 0);

	if (!d.ok) {
		advancedfx::Warning("  (visual-diag field read faulted -- offsets may be stale)\n");
		return;
	}

	advancedfx::Message("  spectatePawn=%d ownerXuid=%llu spectateSteam=%llu defIndex=%d\n",
		pawnIndex, (unsigned long long)d.ownerXuid, (unsigned long long)steamId, d.defIndex);
	advancedfx::Message("  itemId: high=%d low=%u accountId=%u\n", d.itemIdHigh, d.itemIdLow, d.accountId);
	advancedfx::Message("  fallback: paint=%d wear=%.4f seed=%d stattrak=%d\n",
		d.fbPaint, d.fbWear, d.fbSeed, d.fbStat);

	// Networked dynamic attributes -- where a demo weapon's real skin lives (def6/7/8/80).
	float nPaint = 0, nSeed = 0, nWear = 0, nStat = 0;
	bool hP = AttrValueForDef(networked, 6, &nPaint);
	bool hS = AttrValueForDef(networked, 7, &nSeed);
	bool hW = AttrValueForDef(networked, 8, &nWear);
	bool hT = AttrValueForDef(networked, 80, &nStat);
	advancedfx::Message("  networkedAttrs: count=%d  paint(def6)=%s  seed(def7)=%s  wear(def8)=%s  stattrak(def80)=%s\n",
		networked.ok ? networked.count : -1,
		hP ? std::to_string((int)nPaint).c_str() : "(absent)",
		hS ? std::to_string((int)nSeed).c_str() : "(absent)",
		hW ? std::to_string(nWear).c_str() : "(absent)",
		hT ? std::to_string((int)nStat).c_str() : "(absent)");
	advancedfx::Message("  localAttrs (m_AttributeList): count=%d (usually empty for demo items)\n",
		local.ok ? local.count : -1);

	// The visuals-cache flags + their resolved schema offsets, so the values can be plugged straight
	// into an IDA/Ghidra xref pass (search for byte access at weapon_base + these offsets).
	advancedfx::Message("  visuals flags (the rebuild gate):\n");
	if (d.haveVisualsData)
		advancedfx::Message("    m_bVisualsDataSet            = %u   @+0x%zx\n", (unsigned)d.visualsDataSet, (size_t)o.C_CSWeaponBase.m_bVisualsDataSet);
	else
		advancedfx::Message("    m_bVisualsDataSet            = (offset unresolved)\n");
	if (d.haveClearUgc)
		advancedfx::Message("    m_bClearWeaponIdentifyingUGC = %u   @+0x%zx\n", (unsigned)d.clearUgc, (size_t)o.C_CSWeaponBase.m_bClearWeaponIdentifyingUGC);
	else
		advancedfx::Message("    m_bClearWeaponIdentifyingUGC = (offset unresolved)\n");
	if (d.haveReloadEvent)
		advancedfx::Message("    m_nCustomEconReloadEventId   = %d   @+0x%zx\n", d.reloadEvent, (size_t)o.C_CSWeaponBase.m_nCustomEconReloadEventId);
	else
		advancedfx::Message("    m_nCustomEconReloadEventId   = (offset unresolved)\n");
	if (d.haveAttrInit)
		advancedfx::Message("    m_bAttributesInitialized     = %u   @+0x%zx (on C_EconEntity)\n", (unsigned)d.attrInit, (size_t)o.C_EconEntity.m_bAttributesInitialized);

	// Echo which stale-mark writes are currently ENABLED, so the live flag values above can be tied
	// back to what we are (or are not) writing. All-OFF is the HUD-safe default.
	const CosmeticRebuildFlags& rf = sys.GetRebuildFlags();
	advancedfx::Message("  rebuildflags (enabled writes): visualsdata=%d clearugc=%d reloadevent=%d "
		"initialized=%d attrinit=%d imagecache=%d attrparity=%d\n",
		rf.visualsDataSet ? 1 : 0, rf.clearUgc ? 1 : 0, rf.reloadEvent ? 1 : 0,
		rf.initialized ? 1 : 0, rf.attrInit ? 1 : 0, rf.imageCache ? 1 : 0, rf.attrParity ? 1 : 0);

	advancedfx::Message("  worldModel: %s\n", model.ok ? model.name : "(unresolved)");

	int effectivePaint = hP ? (int)nPaint : d.fbPaint;
	int paintLegacy = (effectivePaint > 0) ? PaintKitLegacyModel(effectivePaint) : -2;
	const uint64_t worldMesh = ReadEntityMeshGroupMask(weapon);
	int vmIdx = -1;
	int vmPaint = -1;
	uint64_t vmMesh = 0;
	const bool vmFound = ReadActiveViewmodelWeaponState((unsigned char*)pawn, cls, &vmIdx, &vmPaint, &vmMesh);
	const char* meshModeLabel = "auto";
	if (sys.MeshLegacyMode() == 1) meshModeLabel = "legacy";
	else if (sys.MeshLegacyMode() == -1) meshModeLabel = "modern";
	advancedfx::Message("  mesh: worldMask=%llu vmMask=%llu vmIdx=%d vmFound=%d paintLegacy=%d "
		"meshMode=%s maskModern=%llu maskLegacy=%llu\n",
		(unsigned long long)worldMesh, (unsigned long long)vmMesh, vmIdx, vmFound ? 1 : 0,
		paintLegacy, meshModeLabel, (unsigned long long)sys.MaskModern(), (unsigned long long)sys.MaskLegacy());

	advancedfx::Message("  note: viewmodel is a separate C_BaseViewModel entity sharing this econ item; "
		"a skin change alters the composited material, not the model path above.\n");
}

// On-screen mesh-group readout for MovieHud.cpp. Reads the SAME live fields the renderer itself
// consults (CModelState::m_MeshGroupMask via ReadEntityMeshGroupMask) every call -- never a cached
// or "last write" value -- so what this reports can never drift from what the console/screenshot
// shows a moment later. No writes.
namespace {
bool MeshHudFail(CosmeticMeshHudInfo* out, const char* reason) {
	std::snprintf(out->reason, sizeof(out->reason), "%s", reason);
	return false;
}
} // namespace

bool Cosmetics_GetSpectatedWeaponMeshHud(CosmeticMeshHudInfo* out) {
	if (!out)
		return false;
	*out = CosmeticMeshHudInfo{};
	if (!g_cosmeticsOffsetsOk)
		return MeshHudFail(out, "no-offsets");
	CosmeticOverrideSystem& sys = CosmeticsRef();
	if (!sys.InDemoContext())
		return MeshHudFail(out, "no-demo");

	const int pawnIndex = sys.CurrentSpectatedPawnIndex();
	if (pawnIndex < 0)
		return MeshHudFail(out, "no-spectate");
	CEntityInstance* pawn = EntFromIndex(pawnIndex);
	if (!pawn || !pawn->IsPlayerPawn())
		return MeshHudFail(out, "pawn-gone");
	if (pawn->GetHealth() <= 0)
		return MeshHudFail(out, "dead");

	SOURCESDK::CS2::CBaseHandle wh = pawn->GetActiveWeaponHandle();
	CEntityInstance* weapon = wh.IsValid() ? EntFromIndex(wh.GetEntryIndex()) : nullptr;
	if (!weapon)
		return MeshHudFail(out, "no-weapon");

	const char* cls = weapon->GetClassName(); // validated entity; raw call matches the rest of this file
	if (cls) {
		const char* label = (0 == std::strncmp(cls, "weapon_", 7)) ? cls + 7 : cls;
		std::snprintf(out->weaponLabel, sizeof(out->weaponLabel), "%s", label);
	}

	const ClientDllOffsets_t& o = g_clientDllOffsets;
	unsigned char* w = (unsigned char*)weapon;
	unsigned char* itemView = w + o.C_EconEntity.m_AttributeManager + o.C_AttributeContainer.m_Item;

	WeaponVisualDiag d;
	ReadWeaponVisualDiag(w, itemView, &d);
	AttrListDump networked;
	ReadAttrList(itemView, o.C_EconItemView.m_NetworkedDynamicAttributes, &networked);
	float nPaint = 0;
	const bool hP = AttrValueForDef(networked, 6, &nPaint);
	const int effectivePaint = hP ? (int)nPaint : (d.ok ? d.fbPaint : 0);
	out->paintLegacy = (effectivePaint > 0) ? PaintKitLegacyModel(effectivePaint) : -2;

	out->worldMask = ReadEntityMeshGroupMask(weapon);

	// Viewmodel: the strict scene-child resolver only finds HOLSTERED weapons on this client build
	// (the actively-held weapon's first-person model is a pooled high-index C_CS2HudModelWeapon, not
	// a scene-node child of the arms entity -- see the wide-scan fallback in
	// CosmeticModelSwap.cpp:RefreshViewmodelWeapons, which this mirrors read-only).
	int vmIdx = -1, vmPaint = -1;
	uint64_t vmMesh = 0;
	bool vmFound = ReadActiveViewmodelWeaponState((unsigned char*)pawn, cls, &vmIdx, &vmPaint, &vmMesh);
	if (!vmFound) {
		ModelNameResult worldModel;
		ReadEntityModelName(weapon, &worldModel);
		if (worldModel.ok && worldModel.name[0]) {
			for (int idx = 0x4000; idx < 0x8000 && !vmFound; ++idx) {
				CEntityInstance* hud = EntFromIndex(idx);
				if (!hud)
					continue;
				const char* hcls = nullptr;
				const char* hccls = nullptr;
				__try {
					hcls = hud->GetClassName();
					hccls = hud->GetClientClassName();
				} __except (1) {
					continue;
				}
				const bool isHudWeapon = (hcls && std::strstr(hcls, "hudmodel_weapon"))
					|| (hccls && std::strstr(hccls, "HudModelWeapon"));
				if (!isHudWeapon)
					continue;
				ModelNameResult hudModel;
				ReadEntityModelName(hud, &hudModel);
				if (!hudModel.ok || !hudModel.name[0] || 0 != _stricmp(hudModel.name, worldModel.name))
					continue;
				vmMesh = ReadEntityMeshGroupMask(hud);
				vmFound = true;
			}
		}
	}
	out->vmFound = vmFound;
	out->vmMask = vmMesh;
	out->ok = true;
	return true;
}

// ---- Observer probe ("cosmetics observerprobe") ---------------------------------------------------
// Read-only. Answers "who is the camera on, and which of our resolvers can see it": the local
// split-screen controller, the pawn its m_hPawn names and that pawn's observer services, then every
// player pawn with its own observer mode/target, health and render-eye distance to the game camera,
// next to what AfxGetSpectatedPawnIndex / Held / UserId and the demo bar's camera label return.
// Written to the console and (always) to the mvm_debug log so a third-person or free-cam session can
// be compared with first person line by line.

struct ObserverProbeRow {
	bool ok = false;
	bool pawn = false;
	bool controller = false;
	int health = 0;
	int team = 0;
	int obsMode = 0;
	int obsTarget = -1;
	int controllerIndex = -1;
	int originalController = -1;
	int pawnIndex = -1;
	int playerPawn = -1;
	float eye[3] = {};
	const char* cls = nullptr;
};

static void ProbeEntity(CEntityInstance* ent, ObserverProbeRow* r) {
	*r = ObserverProbeRow{};
	if (!ent) return;
	__try {
		r->cls = ent->GetClientClassName();
		r->pawn = ent->IsPlayerPawn();
		r->controller = ent->IsPlayerController();
		r->team = ent->GetTeam();
		if (r->pawn) {
			r->health = (int)ent->GetHealth();
			r->obsMode = ent->GetObserverMode();
			const SOURCESDK::CS2::CBaseHandle t = ent->GetObserverTarget();
			r->obsTarget = t.IsValid() ? t.GetEntryIndex() : -1;
			const SOURCESDK::CS2::CBaseHandle c = ent->GetPlayerControllerHandle();
			r->controllerIndex = c.IsValid() ? c.GetEntryIndex() : -1;
			if (g_clientDllOffsets.C_CSPlayerPawn.m_hOriginalController) {
				const SOURCESDK::CS2::CBaseHandle oc = SOURCESDK::CS2::CEntityHandle::CEntityHandle(*(unsigned int*)((unsigned char*)ent + g_clientDllOffsets.C_CSPlayerPawn.m_hOriginalController));
				r->originalController = oc.IsValid() ? oc.GetEntryIndex() : -1;
			}
			ent->GetRenderEyeOrigin(r->eye);
		}
		if (r->controller) {
			const SOURCESDK::CS2::CBaseHandle p = ent->GetPlayerPawnHandle();
			r->pawnIndex = p.IsValid() ? p.GetEntryIndex() : -1;
			if (g_clientDllOffsets.CCSPlayerController.m_hPlayerPawn) {
				const SOURCESDK::CS2::CBaseHandle pp = SOURCESDK::CS2::CEntityHandle::CEntityHandle(*(unsigned int*)((unsigned char*)ent + g_clientDllOffsets.CCSPlayerController.m_hPlayerPawn));
				r->playerPawn = pp.IsValid() ? pp.GetEntryIndex() : -1;
			}
		}
		r->ok = true;
	} __except (1) {
		r->ok = false;
	}
}

void Cosmetics_PrintObserverProbe(const char* cmd) {
	const double* camO = g_CurrentGameCamera.origin;
	const double* camA = g_CurrentGameCamera.angles;
	int observerTarget = -1;
	const int observerMode = AfxGetLocalObserverState(&observerTarget);
	const int raw = AfxGetSpectatedPawnIndex();
	const int held = AfxGetSpectatedPawnIndexHeld();
	const int userId = AfxGetSpectatedUserId();
	const int label = CameraTimeline_NativeCameraMode();
	char line[512];
	std::snprintf(line, sizeof(line),
		"camera=%.1f %.1f %.1f ang=%.1f %.1f label=%d localObserver mode=%d target=%d raw=%d held=%d userId=%d",
		camO[0], camO[1], camO[2], camA[0], camA[1], label, observerMode, observerTarget, raw, held, userId);
	advancedfx::Message("%s cosmetics observerprobe: %s\n", cmd, line);
	MvmDebugLog_LinefAlways("customize.observerprobe", "%s", line);

	if (CEntityInstance* viewer = AfxGetLocalViewerPawn()) {
		const SOURCESDK::CS2::CBaseHandle h = viewer->GetHandle();
		std::snprintf(line, sizeof(line), "localViewerPawn=%d", h.IsValid() ? h.GetEntryIndex() : -2);
	} else {
		std::snprintf(line, sizeof(line), "localViewerPawn=-1");
	}
	advancedfx::Message("%s cosmetics observerprobe: %s\n", cmd, line);
	MvmDebugLog_LinefAlways("customize.observerprobe", "%s", line);

	const int highest = GetHighestEntityIndex();
	for (int i = 0; i <= highest; ++i) {
		CEntityInstance* ent = CosmeticDebugRead::EntFromIndex(i);
		if (!ent) continue;
		ObserverProbeRow r;
		ProbeEntity(ent, &r);
		if (!r.ok || (!r.pawn && !r.controller)) continue;
		if (r.pawn) {
			const double dx = r.eye[0] - camO[0], dy = r.eye[1] - camO[1], dz = r.eye[2] - camO[2];
			std::snprintf(line, sizeof(line),
				"pawn %d class=%s team=%d health=%d obsMode=%d obsTarget=%d controller=%d originalController=%d eye=%.1f %.1f %.1f eyeDist=%.1f",
				i, r.cls ? r.cls : "?", r.team, r.health, r.obsMode, r.obsTarget, r.controllerIndex, r.originalController,
				r.eye[0], r.eye[1], r.eye[2], std::sqrt(dx * dx + dy * dy + dz * dz));
		} else {
			const char* name = nullptr;
			uint64_t steam = 0;
			__try { name = ent->GetSanitizedPlayerName(); steam = ent->GetSteamId(); } __except (1) { name = nullptr; }
			std::snprintf(line, sizeof(line), "controller %d class=%s team=%d pawn=%d playerPawn=%d steam=%llu name=%s",
				i, r.cls ? r.cls : "?", r.team, r.pawnIndex, r.playerPawn, (unsigned long long)steam, name ? name : "?");
		}
		advancedfx::Message("%s cosmetics observerprobe: %s\n", cmd, line);
		MvmDebugLog_LinefAlways("customize.observerprobe", "%s", line);
	}
}

} // namespace Filmmaker
