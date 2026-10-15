// Glove swap: SEH-guarded writes into the pawn's embedded m_EconGloves C_EconItemView
// (def index / quality / account id / paint attributes) plus the body-group and
// gloves-changed rebuild that makes the engine recompose the glove render. The engine-owned
// arms viewmodel performs the final render rebuild; do not replace it with a glove-only model.

#include "CosmeticModelSwapInternal.h"

#include "../../ClientEntitySystem.h"   // CEntityInstance, CBaseHandle
#include "../../SchemaSystem.h"          // g_clientDllOffsets
#include "../Debug/MvmDebugLog.h"
#include "CosmeticEntityAccess.h"           // MvmDebugLog_* + MvmAgentLog + MvmCrashWatch_Arm

#include "../../../Shared/binutils.h"

#include <cstdio>

namespace Filmmaker {
namespace modelswap {

namespace {

// Overwrite paint/seed/wear attrs in-place on one C_EconItemView attribute list (networked or local).
void WriteGlovePaintAttrs(unsigned char* glove, ptrdiff_t listOff, int paintKit, float wear, int seed,
	int* paintW, int* seedW, int* wearW, int* attrVec, int* attrCount) {
	const ClientDllOffsets_t& o = g_clientDllOffsets;
	if (!glove || !listOff || !o.C_AttributeList.m_Attributes
		|| !o.CEconItemAttribute.m_iAttributeDefinitionIndex || !o.CEconItemAttribute.m_flValue
		|| !o.CEconItemAttribute.m_size)
		return;
	unsigned char* vec = glove + listOff + o.C_AttributeList.m_Attributes;
	int count = *(int*)vec;
	unsigned char* data = *(unsigned char**)(vec + 8);
	if (!(count > 0 && count <= 128 && (uintptr_t)data > 0x10000)) {
		data = *(unsigned char**)vec;
		count = *(int*)(vec + 16);
	}
	if (count <= 0 || count > 128 || (uintptr_t)data <= 0x10000)
		return;
	if (listOff == o.C_EconItemView.m_NetworkedDynamicAttributes) {
		if (attrVec) *attrVec = 1;
		if (attrCount) *attrCount = count;
	}
	const int stride = (int)o.CEconItemAttribute.m_size;
	for (int i = 0; i < count; ++i) {
		unsigned char* attr = data + (ptrdiff_t)i * stride;
		const int def = (int)*(uint16_t*)(attr + o.CEconItemAttribute.m_iAttributeDefinitionIndex);
		float* pv = (float*)(attr + o.CEconItemAttribute.m_flValue);
		if (def == 6) { *pv = (float)paintKit; if (paintW) *paintW = 1; }
		else if (def == 7) { *pv = (float)seed; if (seedW) *seedW = 1; }
		else if (def == 8) { *pv = wear; if (wearW) *wearW = 1; }
	}
}

void ReadGlovePaintAttrs(unsigned char* glove, ptrdiff_t listOff, int* paint, int* seed, float* wear) {
	const ClientDllOffsets_t& o = g_clientDllOffsets;
	if (!glove || !paint || !listOff || !o.C_AttributeList.m_Attributes
		|| !o.CEconItemAttribute.m_iAttributeDefinitionIndex || !o.CEconItemAttribute.m_flValue
		|| !o.CEconItemAttribute.m_size)
		return;
	unsigned char* vec = glove + listOff + o.C_AttributeList.m_Attributes;
	int count = *(int*)vec;
	unsigned char* data = *(unsigned char**)(vec + 8);
	if (!(count > 0 && count <= 128 && (uintptr_t)data > 0x10000)) {
		data = *(unsigned char**)vec;
		count = *(int*)(vec + 16);
	}
	if (count <= 0 || count > 128 || (uintptr_t)data <= 0x10000)
		return;
	const int stride = (int)o.CEconItemAttribute.m_size;
	for (int i = 0; i < count; ++i) {
		unsigned char* attr = data + (ptrdiff_t)i * stride;
		const int def = (int)*(uint16_t*)(attr + o.CEconItemAttribute.m_iAttributeDefinitionIndex);
		const float v = *(float*)(attr + o.CEconItemAttribute.m_flValue);
		if (def == 6) *paint = (int)v;
		else if (def == 7 && seed) *seed = (int)v;
		else if (def == 8 && wear) *wear = v;
	}
}

// SEH-guarded glove field write + pawn/HUD-arms rebuild.
bool SafeApplyGloves(unsigned char* pawn, int gloveDef, int paintKit, float wear, int seed, uint32_t accountId,
	bool composePaintKit) {
	const ClientDllOffsets_t& o = g_clientDllOffsets;
	const ptrdiff_t offGloves = o.C_CSPlayerPawn.m_EconGloves;
	if (!pawn || offGloves == 0 || o.C_EconItemView.m_iItemDefinitionIndex == 0) {
		if (MvmDebugLog_Active())
			MvmDebugLog_Linef("cosmetics.glove", "ABORT pawn=%d offGloves=0x%llx defIdxOff=0x%llx (offsets missing)",
				pawn ? 1 : 0, (unsigned long long)offGloves,
				(unsigned long long)o.C_EconItemView.m_iItemDefinitionIndex);
		return false;
	}
	unsigned char* glove = pawn + offGloves; // embedded C_EconItemView
	char gloveModel[260] = {};
	const char* modelFail = "unknown";
	// Diagnostics: POD counters filled inside the SEH block, logged after it (see "cosmetics.glove").
	int dAttrVec = 0, dAttrCount = 0, dPaintW = 0, dSeedW = 0, dWearW = 0, dCoreFaulted = 0, dBodyFaulted = 0;
	int dNamedSetter = 0, dConstructPk = 0;
	int dBodyGroupNum = 0, dBodyGroupStr = 0;
	int dGlovesChanged = 0, dTeam = 0, dBootstrapped = 0;
	const bool trace = MvmDebugLog_Active();
	int pawnIdx = -1;
	__try {
		CEntityInstance* pawnInst = (CEntityInstance*)pawn;
		if (!pawnInst->IsPlayerPawn())
			return false;
		SOURCESDK::CS2::CBaseHandle ph = pawnInst->GetHandle();
		if (ph.IsValid())
			pawnIdx = ph.GetEntryIndex();
		if (o.C_BaseEntity.m_iTeamNum)
			dTeam = (int)*(uint8_t*)(pawn + o.C_BaseEntity.m_iTeamNum);
	} __except (1) {
		return false;
	}
	if (pawnIdx <= 0 || (dTeam != 2 && dTeam != 3))
		return false;
	__try {
		uint16_t curDef = *(uint16_t*)(glove + o.C_EconItemView.m_iItemDefinitionIndex);
		// Demo pawns often have m_EconGloves def=0 until the engine initializes default gloves.
		// Bootstrap team defaults before writing a custom glove (nerv clear-then-reapply pattern).
		if (curDef == 0) {
			const uint16_t bootDef = (dTeam == 2) ? 5028 : 5029;
			*(uint16_t*)(glove + o.C_EconItemView.m_iItemDefinitionIndex) = bootDef;
			dBootstrapped = 1;
		}
		*(uint16_t*)(glove + o.C_EconItemView.m_iItemDefinitionIndex) = (uint16_t)gloveDef;
		if (o.C_EconItemView.m_iEntityQuality)
			*(int32_t*)(glove + o.C_EconItemView.m_iEntityQuality) = 3; // QUALITY_UNUSUAL
		if (o.C_EconItemView.m_iAccountID)
			*(uint32_t*)(glove + o.C_EconItemView.m_iAccountID) = accountId;
		if (o.C_EconItemView.m_iItemIDHigh)
			*(uint32_t*)(glove + o.C_EconItemView.m_iItemIDHigh) = 1;
		if (o.C_EconItemView.m_iItemIDLow)
			*(uint32_t*)(glove + o.C_EconItemView.m_iItemIDLow) = accountId;
		if (o.C_EconItemView.m_iItemID) {
			uint64_t itemId = ((uint64_t)1u << 32) | (uint64_t)accountId;
			*(uint64_t*)(glove + o.C_EconItemView.m_iItemID) = itemId;
		}
		if (o.C_EconItemView.m_bDisallowSOC)
			*(bool*)(glove + o.C_EconItemView.m_bDisallowSOC) = false;
		if (o.C_EconItemView.m_bRestoreCustomMaterialAfterPrecache)
			*(bool*)(glove + o.C_EconItemView.m_bRestoreCustomMaterialAfterPrecache) = true;
		// Overwrite paint attributes on networked list, then local list if still empty.
		WriteGlovePaintAttrs(glove, o.C_EconItemView.m_NetworkedDynamicAttributes, paintKit, wear, seed,
			&dPaintW, &dSeedW, &dWearW, &dAttrVec, &dAttrCount);
		if (!dPaintW)
			WriteGlovePaintAttrs(glove, o.C_EconItemView.m_AttributeList, paintKit, wear, seed,
				&dPaintW, &dSeedW, &dWearW, nullptr, nullptr);
		// Missing-attribute fallback: a DEFAULT glove has no networked paint attribute to overwrite
		// (dPaintW stays 0 above), so paint can never be written by the vector path -- apply it through
		// the engine named-setter instead (the same path the weapon loop uses), which adds the attribute
		// to the item view if absent. Without this, default gloves could not be painted at all.
		if (paintKit > 0 && !dPaintW && g_fns.setAttributeValueByName) {
			g_fns.setAttributeValueByName(glove, "set item texture prefab", (float)paintKit);
			g_fns.setAttributeValueByName(glove, "set item texture wear", wear);
			g_fns.setAttributeValueByName(glove, "set item texture seed", (float)seed);
			dNamedSetter = 1;
		}
		if (o.C_EconItemView.m_bInitialized)
			*(bool*)(glove + o.C_EconItemView.m_bInitialized) = true;
	} __except (1) {
		dCoreFaulted = 1;
	}
	if (!dCoreFaulted && composePaintKit && paintKit > 0 && g_fns.constructPaintKit) {
		__try {
			g_fns.constructPaintKit(glove);
			dConstructPk = 1;
		} __except (1) {
			if (trace) MvmDebugLog_LinefAlways("glove.swap", "step=constructPk.FAULT idx=%d", pawnIdx);
		}
	}
	// Body-group / glove-changed flags rebuild render composites -- only on the first burst frame.
	// Keep the POD flags independent from the engine calls: on the July 2026 client, one moved
	// body-group signature faulted inside this combined block and prevented m_bNeedToReApplyGloves
	// from ever being set. The documented glove recipe uses the named group, not the extra numeric
	// call, so drop the latter and guard each remaining engine call separately.
	if (!dCoreFaulted && composePaintKit) {
		__try {
			if (o.C_CSPlayerPawn.m_nEconGlovesChanged) {
				uint8_t* pChg = (uint8_t*)(pawn + o.C_CSPlayerPawn.m_nEconGlovesChanged);
				*pChg = (uint8_t)(*pChg + 1u);
				dGlovesChanged = 1;
			}
			if (o.C_CSPlayerPawn.m_bNeedToReApplyGloves)
				*(bool*)(pawn + o.C_CSPlayerPawn.m_bNeedToReApplyGloves) = true;
		} __except (1) {
			dBodyFaulted = 1;
		}
		// "first_or_third_person" selects whether the agent model's OWN default hands are drawn: 1 hides them
		// under a custom glove model (required, or the custom glove never shows), 0 draws them. It was
		// written as 1 for every pick, so going back to the plain team gloves (which have no glove model)
		// left the character with no hands until real playback reset the group (measured 2026-10-09).
		if ((dTeam == 2 || dTeam == 3) && g_fns.setBodyGroup) {
			__try {
				g_fns.setBodyGroup(pawn, "first_or_third_person", paintKit > 0 ? 1 : 0);
				dBodyGroupStr = 1;
			} __except (1) {
				dBodyFaulted = 1;
				if (trace) MvmDebugLog_LinefAlways("glove.swap", "step=setBodyGroup.FAULT idx=%d", pawnIdx);
			}
		}
	}
	const int dFaulted = dCoreFaulted ? 1 : 0;
	bool haveGloveModel = false;
	ptrdiff_t defMapOff = g_itemDefMapOffset;
	if (dCoreFaulted) {
		if (trace) {
			MvmCrashWatch_Arm(pawnIdx, "glove-core-fault");
			MvmDebugLog_LinefAlways("glove.swap", "ABORT idx=%d coreFault=1 team=%d", pawnIdx, dTeam);
			MvmDebugLog_LinefAlways("cosmetics.glove",
				"def=%d paint=%d ABORT coreFault=1 team=%d pawnIdx=%d", gloveDef, paintKit, dTeam, pawnIdx);
		}
		return false;
	}
	haveGloveModel = TryGetModelFromStaticData(glove, gloveModel, sizeof(gloveModel), &modelFail);
	defMapOff = g_itemDefMapOffset;
	if (trace) {
		MvmCrashWatch_Arm(pawnIdx, haveGloveModel ? gloveModel : "glove");
		MvmDebugLog_LinefAlways("glove.swap", "BEGIN idx=%d pawn=%p def=%d paint=%d team=%d model='%s'",
			pawnIdx, (void*)pawn, gloveDef, paintKit, dTeam, haveGloveModel ? gloveModel : "");
	}
	if (haveGloveModel)
		SafePrecacheModel(gloveModel);
	const ptrdiff_t offArms = o.C_CSPlayerPawn.m_hHudModelArms;
	uint32_t armsHandle = 0;
	int armsIdx = -1;
	if (pawn && offArms) {
		__try {
			armsHandle = *(uint32_t*)(pawn + offArms);
			SOURCESDK::CS2::CBaseHandle h(armsHandle);
			if (h.IsValid())
				armsIdx = h.GetEntryIndex();
		} __except (1) { armsHandle = 0; }
	}
	unsigned char* arms = nullptr;
	bool armsResolved = false;
	if (composePaintKit) {
		// The HUD-arms entity is client-only and commonly uses a 0x4000+ handle. Resolve it through the
		// full-range shared helper for diagnostics only. It is an ARMS viewmodel, not a glove entity:
		// replacing its model with glove_sporty.vmdl removes the player's hands entirely. The pawn's
		// embedded glove econ item plus m_bNeedToReApplyGloves drive the engine-owned refresh.
		arms = HudArmsForPawn(pawn);
		armsResolved = arms != nullptr;
		if (arms) {
			__try {
				SOURCESDK::CS2::CBaseHandle ah = ((CEntityInstance*)arms)->GetHandle();
				if (ah.IsValid())
					armsIdx = ah.GetEntryIndex();
			} __except (1) { armsIdx = -1; }
		}
	}
	// Read live glove state after writes (persist check; networked first, then local attrs).
	int liveDef = 0, livePaint = 0;
	float liveWear = 0.0f;
	int liveSeed = 0;
	__try {
		liveDef = (int)*(uint16_t*)(glove + o.C_EconItemView.m_iItemDefinitionIndex);
		ReadGlovePaintAttrs(glove, o.C_EconItemView.m_NetworkedDynamicAttributes, &livePaint, &liveSeed, &liveWear);
		if (livePaint == 0)
			ReadGlovePaintAttrs(glove, o.C_EconItemView.m_AttributeList, &livePaint, &liveSeed, &liveWear);
	} __except (1) {
	}
	// Diagnostic line (always flushed during mvm_debug). Tells which sub-step broke: dAttrVec=0 -> the demo
	// glove has NO networked attribute vector (paint can't be written -> default skin); bodyGroupFn=0
	// -> UpdateBodyGroupChoice unresolved; arms=0 -> first-person glove model not refreshed; haveModel=0
	// -> glove def's model path didn't resolve.
	if (MvmDebugLog_Active()) {
		MvmDebugLog_LinefAlways("cosmetics.glove",
			"def=%d paint=%d seed=%d wear=%.3f faulted=%d attrVec=%d attrCount=%d wrote(p=%d s=%d w=%d) "
			"namedSetter=%d bootstrapped=%d bodyGroupFn=%d bodyGroupNum=%d bodyGroupStr=%d bodyChoice=%d bodyFault=%d team=%d "
			"needReApplyOff=%d initOff=%d haveModel=%d arms=%d model='%s' "
			"liveDef=%d livePaint=%d liveSeed=%d liveWear=%.3f",
			gloveDef, paintKit, seed, wear, dFaulted, dAttrVec, dAttrCount, dPaintW, dSeedW, dWearW,
			dNamedSetter, dBootstrapped, g_fns.updateBodyGroupChoice ? 1 : 0,
			dBodyGroupNum, dBodyGroupStr, 0, dBodyFaulted, dTeam,
			o.C_CSPlayerPawn.m_bNeedToReApplyGloves ? 1 : 0,
			o.C_EconItemView.m_bInitialized ? 1 : 0,
			haveGloveModel ? 1 : 0, armsResolved ? 1 : 0, haveGloveModel ? gloveModel : "",
			liveDef, livePaint, liveSeed, liveWear);
		char data[720];
		std::snprintf(data, sizeof(data),
			"\"wantDef\":%d,\"wantPaint\":%d,\"liveDef\":%d,\"livePaint\":%d,"
			"\"wrotePaint\":%d,\"namedSetter\":%d,\"constructPk\":%d,\"bodyGroupNum\":%d,"
			"\"bodyGroupStr\":%d,\"bodyChoice\":%d,\"bodyFault\":%d,\"team\":%d,\"glovesChanged\":%d,"
			"\"attrVec\":%d,\"haveModel\":%d,\"modelFail\":\"%s\","
			"\"arms\":%d,\"armsHandle\":%u,\"armsIdx\":%d,\"pawnIdx\":%d,\"getStaticData\":%d,\"getItemDefByIndex\":%d,\"defMapOff\":%lld,"
			"\"disallowSocOff\":%d,\"restoreMatOff\":%d,\"itemIdOff\":%d,\"faulted\":%d",
			gloveDef, paintKit, liveDef, livePaint, dPaintW, dNamedSetter, dConstructPk, dBodyGroupNum,
			dBodyGroupStr, 0, dBodyFaulted, dTeam, dGlovesChanged,
			dAttrVec,
			haveGloveModel ? 1 : 0, modelFail ? modelFail : "",
			armsResolved ? 1 : 0, armsHandle, armsIdx, pawnIdx,
			g_fns.getStaticData ? 1 : 0, g_fns.getItemDefByIndex ? 1 : 0, (long long)defMapOff,
			o.C_EconItemView.m_bDisallowSOC ? 1 : 0,
			o.C_EconItemView.m_bRestoreCustomMaterialAfterPrecache ? 1 : 0,
			o.C_EconItemView.m_iItemID ? 1 : 0,
			dFaulted);
		MvmAgentLog(liveDef == gloveDef && (paintKit <= 0 || livePaint == paintKit) ? "H5" : "H4",
			"CosmeticModelSwap.cpp:SafeApplyGloves", "apply_done", data);
		if (trace)
			MvmDebugLog_LinefAlways("glove.swap", "END idx=%d coreFault=%d bodyFault=%d arms=%d compose=%d",
				pawnIdx, dCoreFaulted, dBodyFaulted, armsResolved ? 1 : 0, composePaintKit ? 1 : 0);
	}
	return !dCoreFaulted;
}

} // namespace

// ---- paused glove model rebuild --------------------------------------------------------------------
//
// The third-person / preview glove is a C_WorldModelGloves entity that C_CSPlayerPawn's per-tick client
// think creates from m_EconGloves when m_bNeedToReApplyGloves is set (it is also the code that spawns
// "MapPlayerPreview gloves" for the Customize preview pawn). A paused demo runs no client think, so a
// glove change while paused never reached the third-person model, and the Customize preview character
// had NO hands at all until playback resumed (CS2 agents have no hands of their own; the glove model is
// the hands). Running that same per-pawn glove step on a paused frame fixes both.
//
// Resolved from its only call site inside the think: `lea rcx,[rbx+disp32]; call fn; mov rcx,rbx;
// call ...; mov rdi,[rbx+disp32]; mov r14,[rsp+...]` (unique in build 2000930). The lea displacement is
// the glove helper's offset in the pawn; the helper stores its owning pawn at the displacement of the
// function's own `mov r14,[rcx+disp32]` (byte 21 of the prologue), which is checked before every call.

typedef void(__fastcall* PawnGloveUpdate_t)(void* helper);

struct PawnGloveUpdateFns {
	bool attempted = false;
	PawnGloveUpdate_t fn = nullptr;
	ptrdiff_t helperOff = 0;
	ptrdiff_t ownerOff = 0;
};
PawnGloveUpdateFns g_gloveUpdate;

const PawnGloveUpdateFns& ResolvePawnGloveUpdate() {
	if (g_gloveUpdate.attempted)
		return g_gloveUpdate;
	g_gloveUpdate.attempted = true;
	const size_t site = FindClientPattern(
		"48 8D 8B ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 8B CB E8 ?? ?? ?? ?? 48 8B BB ?? ?? ?? ?? 4C 8B B4 24");
	if (!site)
		return g_gloveUpdate;
	const ptrdiff_t helperOff = *(int32_t*)(site + 3);
	unsigned char* fn = (unsigned char*)ResolveRelCall(site + 7);
	// Prologue check: ... 4C 8B B1 <disp32> (mov r14, [rcx+disp32]) at byte 21.
	if (!fn || fn[21] != 0x4C || fn[22] != 0x8B || fn[23] != 0xB1 || helperOff <= 0 || helperOff > 0x4000)
		return g_gloveUpdate;
	g_gloveUpdate.fn = (PawnGloveUpdate_t)fn;
	g_gloveUpdate.helperOff = helperOff;
	g_gloveUpdate.ownerOff = *(int32_t*)(fn + 24);
	return g_gloveUpdate;
}

} // namespace modelswap

using namespace modelswap;

// The Customize preview pawn (C_CSGO_PreviewPlayer) receives its equipped gloves inside its own per-tick
// client think, which a paused demo never runs -- so while paused the preview character had no glove
// model and therefore no hands. Its think is the C_CSPlayerPawn think slot overridden by the preview
// class; the slot is found as the C_CSPlayerPawn vtable entry holding the function that contains the
// glove call site above, and the call is made only on objects whose vtable IS C_CSGO_PreviewPlayer's.
namespace {
struct PreviewThink {
	bool attempted = false;
	void** previewVt = nullptr;
	int slot = -1;
};
PreviewThink g_previewThink;

const PreviewThink& ResolvePreviewThink() {
	if (g_previewThink.attempted)
		return g_previewThink;
	g_previewThink.attempted = true;
	HMODULE client = GetModuleHandleA("client.dll");
	if (!client)
		return g_previewThink;
	const size_t site = FindClientPattern(
		"48 8D 8B ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 8B CB E8 ?? ?? ?? ?? 48 8B BB ?? ?? ?? ?? 4C 8B B4 24");
	if (!site)
		return g_previewThink;
	DWORD64 imageBase = 0;
	PRUNTIME_FUNCTION rf = RtlLookupFunctionEntry((DWORD64)site, &imageBase, nullptr);
	if (!rf)
		return g_previewThink;
	// The site may sit in a cold fragment: follow UNW_FLAG_CHAININFO to the primary entry.
	for (int guard = 0; guard < 8; ++guard) {
		const unsigned char* ui = (const unsigned char*)(imageBase + rf->UnwindData);
		if (!((ui[0] >> 3) & 4))
			break;
		rf = (PRUNTIME_FUNCTION)(ui + 4 + ((ui[2] + 1) & ~1) * 2);
	}
	const uintptr_t thinkFn = (uintptr_t)imageBase + rf->BeginAddress;
	void** pawnVt = (void**)Afx::BinUtils::FindClassVtable(client, ".?AVC_CSPlayerPawn@@", 0, 0);
	void** previewVt = (void**)Afx::BinUtils::FindClassVtable(client, ".?AVC_CSGO_PreviewPlayer@@", 0, 0);
	if (!pawnVt || !previewVt)
		return g_previewThink;
	for (int i = 0; i < 600; ++i) {
		if ((uintptr_t)pawnVt[i] == thinkFn) {
			g_previewThink.slot = i;
			break;
		}
	}
	g_previewThink.previewVt = previewVt;
	if (MvmDebugLog_Active())
		MvmDebugLog_LinefAlways("preview.gloves", "resolve think=%p slot=%d pawnVt=%p previewVt=%p",
			(void*)thinkFn, g_previewThink.slot, (void*)pawnVt, (void*)previewVt);
	return g_previewThink;
}
} // namespace

int PawnGloveEntityIndex(unsigned char* pawn) {
	ResolveModelSwapFns();
	const PawnGloveUpdateFns& g = ResolvePawnGloveUpdate();
	if (!pawn || !g.fn)
		return -1;
	__try {
		if (*(unsigned char**)(pawn + g.helperOff + g.ownerOff) != pawn)
			return -1;
		const int32_t h = *(int32_t*)(pawn + g.helperOff);
		return (h == -1 || h == -2) ? -1 : (int)(h & 0x7FFF);
	} __except (1) {
		return -1;
	}
}

bool RunPreviewPawnThink(unsigned char* pawn) {
	ResolveModelSwapFns();
	const PreviewThink& t = ResolvePreviewThink();
	if (!pawn || !t.previewVt || t.slot < 0)
		return false;
	__try {
		void** vt = *(void***)pawn;
		if (vt != t.previewVt)
			return false;
		((void(__fastcall*)(void*))vt[t.slot])(pawn);
		static unsigned s_thinkLog = 0;
		if (MvmDebugLog_Active() && *(unsigned char*)(pawn + 4480) && (++s_thinkLog % 20) == 0) {
			const ClientDllOffsets_t& o = g_clientDllOffsets;
			const PawnGloveUpdateFns& g = ResolvePawnGloveUpdate();
			unsigned char* gl = pawn + o.C_CSPlayerPawn.m_EconGloves;
			MvmDebugLog_LinefAlways("preview.gloves.state",
				"pawn=%p enabled=%d gloveDef=%d init=%d flag=%d gloveHandle=%d",
				(void*)pawn, (int)*(unsigned char*)(pawn + 4480),
				(int)*(uint16_t*)(gl + o.C_EconItemView.m_iItemDefinitionIndex),
				(int)*(bool*)(gl + o.C_EconItemView.m_bInitialized),
				(int)*(bool*)(pawn + o.C_CSPlayerPawn.m_bNeedToReApplyGloves),
				g.fn ? *(int32_t*)(pawn + g.helperOff) : -9);
		}
		return true;
	} __except (1) {
		return false;
	}
}

bool UpdatePawnGlovesNow(unsigned char* pawn, bool onlyIfFlagged) {
	ResolveModelSwapFns();
	const PawnGloveUpdateFns& g = ResolvePawnGloveUpdate();
	if (!pawn || !g.fn)
		return false;
	const ClientDllOffsets_t& o = g_clientDllOffsets;
	unsigned char* helper = pawn + g.helperOff;
	__try {
		if (*(unsigned char**)(helper + g.ownerOff) != pawn)
			return false; // not a C_CSPlayerPawn-shaped object, or the layout moved
		if (onlyIfFlagged) {
			const bool flagged = o.C_CSPlayerPawn.m_bNeedToReApplyGloves
				&& *(bool*)(pawn + o.C_CSPlayerPawn.m_bNeedToReApplyGloves);
			const bool noGloveEntity = *(int32_t*)helper == -1;
			if (!flagged && !noGloveEntity)
				return false;
		}
		const int32_t before = *(int32_t*)helper;
		const bool flagBefore = o.C_CSPlayerPawn.m_bNeedToReApplyGloves
			&& *(bool*)(pawn + o.C_CSPlayerPawn.m_bNeedToReApplyGloves);
		g.fn(helper);
		// A glove model spawned on a paused frame is never drawn: the renderer skips entities whose
		// m_bHasSuccessfullyInterpolated is still false, and that only flips on a game tick (measured
		// 2026-10-09 by diffing the glove entity paused vs after one tick). Use the engine's own override.
		const int32_t after = *(int32_t*)helper;
		const ptrdiff_t offRender = o.C_BaseEntity.m_bRenderEvenWhenNotSuccessfullyInterpolated;
		if (after != -1 && after != before && offRender && g_pEntityList && *g_pEntityList && g_GetEntityFromIndex) {
			unsigned char* ge = (unsigned char*)g_GetEntityFromIndex(*g_pEntityList, after & 0x7FFF);
			if (ge)
				*(bool*)(ge + offRender) = true;
		}
		if (MvmDebugLog_Active()) {
			unsigned char* gl = o.C_CSPlayerPawn.m_EconGloves ? pawn + o.C_CSPlayerPawn.m_EconGloves : nullptr;
			const int def = (gl && o.C_EconItemView.m_iItemDefinitionIndex)
				? (int)*(uint16_t*)(gl + o.C_EconItemView.m_iItemDefinitionIndex) : -1;
			const int init = (gl && o.C_EconItemView.m_bInitialized)
				? (int)*(bool*)(gl + o.C_EconItemView.m_bInitialized) : -1;
			const bool flagAfter = o.C_CSPlayerPawn.m_bNeedToReApplyGloves
				&& *(bool*)(pawn + o.C_CSPlayerPawn.m_bNeedToReApplyGloves);
			MvmDebugLog_Linef("cosmetics.glove.update",
				"pawn=%p handle %d->%d flag %d->%d gloveDef=%d init=%d",
				(void*)pawn, before, *(int32_t*)helper, flagBefore ? 1 : 0, flagAfter ? 1 : 0, def, init);
		}
		return true;
	} __except (1) {
		return false;
	}
}

namespace modelswap {
} // namespace modelswap

using namespace modelswap;

bool ApplyGloveModel(unsigned char* pawn, int gloveDef, int paintKit, float wear, int seed,
	uint32_t accountId, bool composePaintKit) {
	ResolveModelSwapFns();
	if (!pawn || gloveDef <= 0) {
		if (MvmDebugLog_Active())
			MvmAgentLog("H3", "CosmeticModelSwap.cpp:ApplyGloveModel", "bad_args",
				pawn ? "\"pawn\":1" : "\"pawn\":0");
		return false;
	}
	return SafeApplyGloves(pawn, gloveDef, paintKit, wear, seed, accountId, composePaintKit);
}

} // namespace Filmmaker
