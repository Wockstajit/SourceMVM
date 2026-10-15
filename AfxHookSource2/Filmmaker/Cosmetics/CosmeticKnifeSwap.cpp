// Knife TYPE swap: the built-in knife def -> world-model fallback table and the
// ApplyKnifeModelSwap sequence (precache -> SetModel -> mesh mask -> subclass ->
// viewmodel mirror -> renderable refresh). The swap rebuilds the weapon's animation
// set, which is the crash class the CosmeticAnimFix detour exists for. See the
// step-breadcrumb comments below and Docs/cosmetics/cosmetics-cs2-methodology-notes.md.

#include "CosmeticModelSwapInternal.h"

#include "../Debug/MvmDebugLog.h"           // MvmDebugLog_* + MvmCrashWatch_Arm
#include "CosmeticAnimFix.h"            // EnsureAnimCrashFixInstalled (knife-swap anim crash fix detour)
#include "../../SceneSystem.h"          // g_pCResourceSystem (precache breadcrumb)

#include <cstdio>

namespace Filmmaker {
namespace modelswap {

namespace {

// ---- built-in knife def -> world model fallback (used if GetStaticData is unavailable / invalid) ----
// Stable Valve asset paths; mirror the CEconItemDefinition::m_pszModelName values (Andromeda live dump
// confirmed def 500 -> weapons/models/knife/knife_bayonet/weapon_knife_bayonet.vmdl).
struct KnifeModel { int def; const char* model; };
const KnifeModel kKnifeModels[] = {
	{ 42,  "weapons/models/knife/knife_default_ct/weapon_knife_default_ct.vmdl" },
	{ 59,  "weapons/models/knife/knife_default_t/weapon_knife_default_t.vmdl" },
	{ 500, "weapons/models/knife/knife_bayonet/weapon_knife_bayonet.vmdl" },
	{ 503, "weapons/models/knife/knife_css/weapon_knife_css.vmdl" },
	{ 505, "weapons/models/knife/knife_flip/weapon_knife_flip.vmdl" },
	{ 506, "weapons/models/knife/knife_gut/weapon_knife_gut.vmdl" },
	{ 507, "weapons/models/knife/knife_karambit/weapon_knife_karambit.vmdl" },
	{ 508, "weapons/models/knife/knife_m9/weapon_knife_m9.vmdl" },
	{ 509, "weapons/models/knife/knife_tactical/weapon_knife_tactical.vmdl" },
	{ 512, "weapons/models/knife/knife_falchion/weapon_knife_falchion.vmdl" },
	{ 514, "weapons/models/knife/knife_bowie/weapon_knife_bowie.vmdl" },
	{ 515, "weapons/models/knife/knife_butterfly/weapon_knife_butterfly.vmdl" },
	{ 516, "weapons/models/knife/knife_push/weapon_knife_push.vmdl" },
	{ 517, "weapons/models/knife/knife_cord/weapon_knife_cord.vmdl" },
	{ 518, "weapons/models/knife/knife_canis/weapon_knife_canis.vmdl" },
	{ 519, "weapons/models/knife/knife_ursus/weapon_knife_ursus.vmdl" },
	{ 520, "weapons/models/knife/knife_navaja/weapon_knife_navaja.vmdl" },
	{ 521, "weapons/models/knife/knife_outdoor/weapon_knife_outdoor.vmdl" },
	{ 522, "weapons/models/knife/knife_stiletto/weapon_knife_stiletto.vmdl" },
	{ 523, "weapons/models/knife/knife_talon/weapon_knife_talon.vmdl" },
	{ 525, "weapons/models/knife/knife_skeleton/weapon_knife_skeleton.vmdl" },
	{ 526, "weapons/models/knife/knife_kukri/weapon_knife_kukri.vmdl" },
};

const char* KnifeModelForDef(int def) {
	for (const KnifeModel& k : kKnifeModels)
		if (k.def == def)
			return k.model;
	return nullptr;
}

} // namespace

} // namespace modelswap

using namespace modelswap;

bool ResolveKnifeModelPath(int targetDef, unsigned char* itemView, char* out, size_t outSize) {
	if (!out || outSize == 0)
		return false;
	out[0] = '\0';
	if (targetDef <= 0)
		return false;
	ResolveModelSwapFns();
	// Same resolution order as ApplyKnifeModelSwap: live econ definition first, built-in table fallback.
	if (TryGetModelFromStaticData(itemView, out, outSize) && out[0])
		return true;
	const char* tbl = KnifeModelForDef(targetDef);
	if (!tbl)
		return false;
	std::snprintf(out, outSize, "%s", tbl);
	return out[0] != '\0';
}

bool ApplyKnifeModelSwap(unsigned char* weapon, unsigned char* itemView,
	unsigned char* pawnForViewmodel, int targetDef, uint64_t meshMask, int entityIndex) {
	ResolveModelSwapFns();
	// Approach #3 (the working fix): make sure the anim-builder detour is installed before we SetModel the
	// world weapon to a possibly-unloaded knife model, so the engine's later anim pass can't null-deref.
	EnsureAnimCrashFixInstalled();
	// Always-flushed step breadcrumbs (only when mvm_debug is running). The point: the knife type swap
	// rebuilds the weapon's animation set, and re-firing it onto an entity the engine recreated during a
	// quick weapon switch faults -- often inside one of these native calls, or in the engine's NEXT
	// animation frame. Each step is written + flushed BEFORE the call, so the LAST "knife.swap" line in
	// the log names exactly which call/state preceded the crash (an unmatched "...begin" = it faulted in
	// that call; a clean "END" followed by the crash = it faulted later in the engine's deploy/anim pass).
	const bool trace = MvmDebugLog_Active();
	if (!g_status.CoreOk() || !weapon || targetDef <= 0) {
		if (trace)
			MvmDebugLog_LinefAlways("knife.swap", "ABORT idx=%d coreOk=%d weapon=%d targetDef=%d (functions unresolved or bad args -> no swap)",
				entityIndex, g_status.CoreOk() ? 1 : 0, weapon ? 1 : 0, targetDef);
		return false;
	}

	// Resolve the target model: prefer the live econ definition (GetStaticData on the def-already-set
	// item view), fall back to the built-in knife table.
	char model[260];
	bool haveModel = TryGetModelFromStaticData(itemView, model, sizeof(model));
	if (!haveModel) {
		const char* tbl = KnifeModelForDef(targetDef);
		if (!tbl) {
			if (trace)
				MvmDebugLog_LinefAlways("knife.swap", "ABORT idx=%d targetDef=%d (no model from econ and no table entry -> no swap)",
					entityIndex, targetDef);
			return false;
		}
		std::snprintf(model, sizeof(model), "%s", tbl);
	}

	// Open the crash-watch window so the vectored handler records any access violation in the next few
	// seconds (the post-swap animation/render frames where these crashes land) with its module+offset.
	if (trace) {
		MvmCrashWatch_Arm(entityIndex, model);
		MvmDebugLog_LinefAlways("knife.swap", "BEGIN idx=%d weapon=%p hasViewmodelPawn=%d targetDef=%d meshMask=%llu haveModel=%d model='%s'",
			entityIndex, (void*)weapon, pawnForViewmodel ? 1 : 0, targetDef, (unsigned long long)meshMask, haveModel ? 1 : 0, model);
	}

	// Approach #2 (root-cause fix): blocking-load the target model + its anim data BEFORE SetModel, so the
	// engine's later async anim pass finds a non-null per-model table instead of crashing (see header / §11).
	if (trace) MvmDebugLog_LinefAlways("knife.swap", "step=precache.begin idx=%d on=%d resSys=%d model='%s'",
		entityIndex, g_precacheOn ? 1 : 0, g_pCResourceSystem ? 1 : 0, model);
	bool precached = SafePrecacheModel(model);
	if (trace) MvmDebugLog_LinefAlways("knife.swap", "step=precache.end idx=%d fired=%d totalCalls=%llu",
		entityIndex, precached ? 1 : 0, (unsigned long long)g_precacheCalls);

	if (trace) MvmDebugLog_LinefAlways("knife.swap", "step=setModel.begin idx=%d model='%s'", entityIndex, model);
	bool any = SafeSetModel(weapon, model);
	if (trace) MvmDebugLog_LinefAlways("knife.swap", "step=setModel.end idx=%d ok=%d", entityIndex, any ? 1 : 0);

	if (meshMask) {
		if (trace) MvmDebugLog_LinefAlways("knife.swap", "step=meshMask.begin idx=%d mask=%llu", entityIndex, (unsigned long long)meshMask);
		SafeSetMeshMask(weapon, meshMask);
		if (trace) MvmDebugLog_LinefAlways("knife.swap", "step=meshMask.end idx=%d", entityIndex);
	}

	if (trace) MvmDebugLog_LinefAlways("knife.swap", "step=subclass.begin idx=%d def=%d", entityIndex, targetDef);
	SafeUpdateSubclass(weapon, targetDef);
	if (trace) MvmDebugLog_LinefAlways("knife.swap", "step=subclass.end idx=%d", entityIndex);

	// Approach #1: reset the WORLD weapon's animgraph so the engine rebuilds it for the new model rather
	// than posing it with stale per-model data (the worker-thread null-deref this whole investigation chased).
	ResetAnimGraph(weapon, entityIndex, "world");

	if (pawnForViewmodel) {
		// PRIME crash suspect: this walks the pawn's HUD-arms children (every weapon's first-person
		// viewmodel) and writes the knife model onto the matching one. During a knife->AK switch the AK's
		// viewmodel is mid-deploy; see RefreshViewmodelWeapons for the per-child breadcrumbs.
		if (trace) MvmDebugLog_LinefAlways("knife.swap", "step=viewmodel.begin idx=%d (mirror onto first-person viewmodel)", entityIndex);
		RefreshViewmodelWeapons(pawnForViewmodel, model, meshMask, weapon, entityIndex);
		if (trace) MvmDebugLog_LinefAlways("knife.swap", "step=viewmodel.end idx=%d", entityIndex);
	}

	// Do not call CGameSceneNode::PostDataUpdate here. On the current client it queues
	// renderer work against the freshly rebuilt knife scene node and the client dies
	// before returning (the last crash log ended at postData.begin). The normal weapon
	// paint path uses direct model-state writes for the same reason. The live frame loop
	// will reassert the model/mesh state without invoking this invalidating callback.
	if (trace) MvmDebugLog_LinefAlways("knife.swap", "step=postData.skip idx=%d reason=current-client-unsafe", entityIndex);
	if (trace) MvmDebugLog_LinefAlways("knife.swap", "END idx=%d ok=%d", entityIndex, any ? 1 : 0);
	return any;
}

} // namespace Filmmaker
