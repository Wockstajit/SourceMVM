#include "RenderWorldOnly.h"
#include "RenderPassPlan.h"
#include "../../RenderSystemDX11Hooks.h"

#include "../Debug/MvmDebugLog.h" // MvmDebugLog_* (mandatory runtime-diagnosis channel)
#include "../../ClientEntitySystem.h"      // CEntityInstance, g_pEntityList, g_GetEntityFromIndex
#include "../../SchemaSystem.h"            // g_clientDllOffsets (C_BaseEntity::m_hOwnerEntity)
#include "../../SceneSystem.h"             // SceneSystem_ReportDrawCoverage (draw-path coverage)
#include "../../WrpConsole.h"
#include "../../../Shared/AfxConsole.h"

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "../../../Deps/release/prop/cs2/sdk_src/public/cdll_int.h" // ISource2EngineToClient
#include "../../../Deps/release/prop/cs2/sdk_src/public/icvar.h"    // ICvar / Cvar_s

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

extern SOURCESDK::CS2::ISource2EngineToClient* g_pEngineToClient;

namespace Filmmaker {
namespace {

// Subject classes World Only has to tell apart. Deliberately coarse: the only decision downstream
// is keep-or-drop, and every extra class is another rule that can misfire on a map nobody tested.
enum class WoClass : int {
	World = 0,      // brushes, terrain, decals, sky, water, everything unclaimed -> KEEP
	Props = 1,      // map props (crates, cables, foliage) -> KEEP, they ARE the map
	Players = 2,    // player/agent bodies, ragdolls, worn gear -> DROP
	Viewmodel = 3,  // first-person arms/gloves, and third-person hands (same asset) -> DROP
	Weapons = 4,    // held, dropped, thrown, and econ-composited weapon materials -> DROP
	// The C4. Its own class because it is the ONE subject whose verdict depends on game state:
	// a PLANTED bomb is scene furniture -- the objective the shot is about, sitting on the site
	// with nobody holding it -- while a carried or loose one is a player's weapon like any other.
	// Kept only while the bomb is actually planted; see BombPlanted().
	Bomb = 5,
	Count = 6
};

const char* ClassName(WoClass c) {
	switch (c) {
	case WoClass::World: return "world";
	case WoClass::Props: return "props";
	case WoClass::Players: return "players";
	case WoClass::Viewmodel: return "viewmodel";
	case WoClass::Weapons: return "weapons";
	case WoClass::Bomb: return "bomb";
	default: return "?";
	}
}

bool StartsWith(const char* value, const char* prefix) {
	return value && prefix && 0 == std::strncmp(value, prefix, std::strlen(prefix));
}

// The material-path vocabulary below is the surviving, measured half of the removed pass system's
// classifier (git b8f2448d^ SceneRenderClassification.h). Every rule here was established against
// live draws; do not "tidy" one away without re-probing with `render worldonly probe`.
WoClass Classify(const char* materialName) {
	if (!materialName || !*materialName) return WoClass::World;

	// The arms/gloves set is SHARED between the first-person viewmodel and a third-person player's
	// hands. The earlier pass system needed an entity-attachment probe to tell those apart because
	// it kept one and dropped the other. World Only drops BOTH, so the ambiguity is irrelevant
	// here -- and that is precisely why this pass needs no entity resolution on the render thread.
	if (StartsWith(materialName, "characters/models/shared/arms/"))
		return WoClass::Viewmodel;
	if (StartsWith(materialName, "characters/"))
		return WoClass::Players;

	// The bomb, before the general weapon rules below would swallow it. Both spellings, same as
	// every other weapon: `weapons/models/c4/...` and `.../w_models/w_c4...`. MEASURED on a
	// planted bomb (mvm_debug render.worldonly, de_mirage round 3): the draws are
	// `weapons/models/c4/materials/weapon_c4.vmat` and `weapon_c4_digits.vmat` (the LED panel).
	if (StartsWith(materialName, "weapons/models/c4/")
		|| std::strstr(materialName, "models/weapons/w_c4")
		|| std::strstr(materialName, "w_models/w_c4"))
		return WoClass::Bomb;

	// CS2 spells weapon materials two ways and the segments are REVERSED between them:
	//   `weapons/models/<gun>/materials/...`      viewmodel / econ form
	//   `materials/models/weapons/w_models/...`   world models and equipment
	// A prefix test alone therefore misses the second form -- that gap is what once left the
	// defuse kit (`.../w_models/w_eq_defuser/w_eq_defuser.vmat`) classified as world and floating
	// in the frame after its owner was removed. `compmat_` is the runtime-composited econ skin
	// material, which carries no path at all.
	// `weapons/` as a whole, not just `weapons/models/`. MEASURED 2026-08-10 (de_mirage, pre-plant
	// round): weapon CHARMS live at `weapons/keychains/<charm>/materials/...`, which
	// `weapons/models/` missed -- so a keychain stayed in the plate, dangling in mid-air where the
	// gun it hangs off had just been removed. `weapons/` is the econ/weapon asset namespace; world
	// and map materials all live under `materials/` (audited: of 142 distinct KEPT materials across
	// six test frames, the only non-`materials/` entries were these charms and the C4).
	if (StartsWith(materialName, "weapons/")
		|| std::strstr(materialName, "models/weapons/")
		|| StartsWith(materialName, "compmat_"))
		return WoClass::Weapons;

	if (std::strstr(materialName, "models/props/"))
		return WoClass::Props;

	return WoClass::World;
}

// ---- "is the bomb planted?" ---------------------------------------------------------------------
//
// The ONE piece of game state this filter consults, and deliberately a GLOBAL bit rather than a
// per-draw question. Per-draw entity identity was measured unreliable (shared render references),
// but the global answer is exact and sufficient:
//
//   planted   -> the only C4 in the world IS the planted one. Nobody carries it. Keep it.
//   not planted -> every C4 draw is a weapon a player is holding, or one lying loose. Drop it,
//                  exactly like every other held or dropped weapon. A bomb kept here would hover
//                  in mid-air on the back of a player World Only just removed.
//
// So "keep the bomb" can never leak a carried bomb, and it needs no attachment probe.
std::atomic<bool> g_bombPlanted{false};

// Verification control, not a feature knob. `off` reproduces the pre-2026-08-10 behaviour (the
// bomb dropped with every other weapon) so a paused frame can be captured both ways and diffed:
// if anything OUTSIDE the bomb changes between the two, the bomb rule is admitting more than the
// bomb. Default auto = planted-gated. See Automation/verify/verify-render-worldonly.ps1.
std::atomic<bool> g_bombRuleEnabled{true};

bool BombPlanted() {
	return g_bombRuleEnabled.load(std::memory_order_relaxed)
		&& g_bombPlanted.load(std::memory_order_relaxed);
}

// The bomb has three states and only ONE of them should be hidden:
//
//   planted  -> `planted_c4` / C_PlantedC4 exists. Scene furniture. KEEP.
//   dropped  -> `weapon_c4` / C_C4 exists with NO owner: it is lying on the ground where somebody
//               dropped it, exactly like a body or any other object resting in the scene. KEEP.
//   carried  -> `weapon_c4` exists WITH an owner. It is on the back of a player World Only has
//               just removed, so drawing it would leave a bomb hanging in mid-air. DROP.
//
// This stays a GLOBAL bit rather than a per-draw question, which is what makes it reliable: there
// is only ever one bomb in a round, so "is the bomb visible" is answerable once per frame from the
// entity list, and no draw ever has to be traced back to an entity (which was measured unreliable).
// The same trick does NOT generalise to other weapons -- there are many at once and they share
// materials, so telling a dropped AK from a carried one would need per-draw entity identity.
//
// Networked entities only -- client-only indices start at 0x4000 and the bomb is never one.
constexpr int kMaxNetworkedIndex = 0x4000;
// Throttled: plant/defuse are seconds-scale events, and a scan per frame would walk 16k slots for
// a bit that changes twice a round. Matches PreviewRenderFlag's rescan cadence.
constexpr uint64_t kBombScanFrames = 16;

uint64_t g_frame = 0;
uint64_t g_lastBombScanFrame = 0;
int g_bombEntityIndex = -1;            // diagnostics only
uint32_t g_bombOwnerHandle = 0xFFFFFFFFu; // diagnostics only
int g_bombState = 0;                   // diagnostics only (BombState)

CEntityInstance* SafeEntity(int index) {
	if (index < 0 || index >= kMaxNetworkedIndex || !g_pEntityList || !*g_pEntityList || !g_GetEntityFromIndex)
		return nullptr;
	__try { return (CEntityInstance*)g_GetEntityFromIndex(*g_pEntityList, index); }
	__except (1) { return nullptr; }
}

const char* SafeClassName(CEntityInstance* e) {
	if (!e) return nullptr;
	__try { return e->GetClassName(); } __except (1) { return nullptr; }
}

const char* SafeClientClassName(CEntityInstance* e) {
	if (!e) return nullptr;
	__try { return e->GetClientClassName(); } __except (1) { return nullptr; }
}

bool ContainsNoCase(const char* haystack, const char* needle) {
	if (!haystack || !needle || !*needle) return false;
	const size_t n = std::strlen(needle);
	for (const char* p = haystack; *p; ++p)
		if (0 == _strnicmp(p, needle, n)) return true;
	return false;
}

// Both spellings, because the two accessors disagree for different entity kinds (map entities
// answer GetClassName, networked classes answer GetClientClassName) and this has to catch either.
bool IsPlantedBomb(CEntityInstance* e) {
	return ContainsNoCase(SafeClassName(e), "planted_c4")
		|| ContainsNoCase(SafeClientClassName(e), "plantedc4");
}

// The carryable bomb. "planted_c4" does not contain "weapon_c4", so these two never overlap.
bool IsCarryableBomb(CEntityInstance* e) {
	return ContainsNoCase(SafeClassName(e), "weapon_c4")
		|| ContainsNoCase(SafeClientClassName(e), "c4");
}

bool ReadOwnerHandle(CEntityInstance* e, uint32_t* out) {
	*out = 0xFFFFFFFFu;
	const ptrdiff_t off = g_clientDllOffsets.C_BaseEntity.m_hOwnerEntity;
	if (!e || off == 0) return false;
	__try { *out = *(uint32_t*)((unsigned char*)e + off); return true; }
	__except (1) { return false; }
}

// A CHandle's entry index lives in the low bits; an unset handle is all-ones. "No owner" means the
// bomb is lying on the ground rather than riding on somebody's back.
bool HandleIsSet(uint32_t handle) {
	return handle != 0xFFFFFFFFu && (handle & 0x7FFFu) != 0x7FFFu;
}

// ---- player shadows -----------------------------------------------------------------------------
//
// Dropping a player's draws removes the PLAYER, not the player's SHADOW. CS2 renders character
// shadows in a separate shadow-map pass that the scene draw hooks never see, so with the material
// filter alone the map still carried two soft contact blobs on the floor under every removed
// player's feet (reported and reproduced 2026-08-10). No amount of draw classification can reach
// them -- the shadow pass is upstream of the draw we filter.
//
// `r_drawcsplayers 0` removes the players before that pass runs, so the shadows go with them.
// MEASURED to fix it live. The material filter is still what removes gear, weapons and the
// viewmodel (this convar does not touch those), so the two mechanisms are complementary, not
// redundant.
//
// Applied and restored from the MAIN-THREAD pump: ExecuteClientCmd is main-thread only, while
// SetArmed can be called from either the console or the render-job driver. Arming records the
// intent; the pump reconciles it.
constexpr const char* kDrawPlayersCvar = "r_drawcsplayers";

bool g_cvarApplied = false;      // are we currently forcing the convar?
std::string g_cvarPrevValue;     // the user's value, restored verbatim on disarm

void RunClientCmd(const char* cmd) {
	if (g_pEngineToClient) g_pEngineToClient->ExecuteClientCmd(0, cmd, true);
}

std::string ReadBoolCvarValue(const char* name, const char* fallback) {
	using namespace SOURCESDK::CS2;
	if (!g_pCVar) return fallback;
	Cvar_s* handle = g_pCVar->GetCvar(g_pCVar->FindConVar(name, false).Get());
	if (!handle) return fallback;
	return handle->m_Value.m_bValue ? "1" : "0";
}

// Reconciles the convar with the armed state. Idempotent, so it is safe to call every frame, and
// it never writes when nothing has to change.
void ReconcilePlayerDrawCvar(bool armed) {
	if (armed == g_cvarApplied) return;
	char cmd[64];
	if (armed) {
		g_cvarPrevValue = ReadBoolCvarValue(kDrawPlayersCvar, "1");
		snprintf(cmd, sizeof(cmd), "%s 0", kDrawPlayersCvar);
		RunClientCmd(cmd);
		g_cvarApplied = true;
	} else {
		snprintf(cmd, sizeof(cmd), "%s %s", kDrawPlayersCvar,
			g_cvarPrevValue.empty() ? "1" : g_cvarPrevValue.c_str());
		RunClientCmd(cmd);
		g_cvarApplied = false;
	}
	if (MvmDebugLog_Active())
		MvmDebugLog_LinefAlways("render.worldonly", "%s -> '%s' (player shadows %s)",
			kDrawPlayersCvar, armed ? "0" : (g_cvarPrevValue.empty() ? "1" : g_cvarPrevValue.c_str()),
			armed ? "removed" : "restored");
}

enum class BombState { None = 0, Carried, Dropped, Planted };

const char* BombStateName(BombState s) {
	switch (s) {
	case BombState::Carried: return "carried";
	case BombState::Dropped: return "dropped on the ground";
	case BombState::Planted: return "planted";
	default: return "none";
	}
}

// One pass over the networked entities. Planted wins outright; otherwise the carryable bomb's
// owner handle decides. Also reports the entity index and raw handle for `status` / mvm_debug,
// because "is this handle really unset?" is the kind of thing that must be read, not assumed.
BombState ScanBomb(int* outIndex, uint32_t* outOwner) {
	*outIndex = -1;
	*outOwner = 0xFFFFFFFFu;
	BombState best = BombState::None;
	for (int i = 0; i < kMaxNetworkedIndex; ++i) {
		CEntityInstance* e = SafeEntity(i);
		if (!e) continue;
		if (IsPlantedBomb(e)) {
			*outIndex = i;
			*outOwner = 0xFFFFFFFFu;
			return BombState::Planted; // nothing outranks a planted bomb
		}
		if (best == BombState::None && IsCarryableBomb(e)) {
			uint32_t owner = 0xFFFFFFFFu;
			ReadOwnerHandle(e, &owner);
			*outIndex = i;
			*outOwner = owner;
			best = HandleIsSet(owner) ? BombState::Carried : BombState::Dropped;
		}
	}
	return best;
}

// Bomb is intentionally NOT folded in here: it is the one class whose verdict is not a property of
// the material, so it is answered at the call site where the game-state bit is read.
bool ClassIsWorld(WoClass c) {
	return c == WoClass::World || c == WoClass::Props;
}

bool ClassIsKept(WoClass c) {
	if (c == WoClass::Bomb) return BombPlanted();
	return ClassIsWorld(c);
}

// ---- state ------------------------------------------------------------------------------------

std::atomic<bool> g_armed{false};
// Multipass: whether a job has a World Only render (keeps the bomb scan alive without the live arm).
std::atomic<bool> g_jobUsesPass{false};
char g_reason[64] = "";
std::atomic<unsigned long long> g_kept[(int)WoClass::Count] = {};
std::atomic<unsigned long long> g_dropped[(int)WoClass::Count] = {};

// ---- bounded distinct-material probe -----------------------------------------------------------
//
// Counters say "12 player draws were dropped this frame"; they cannot say WHICH material survived
// when something is still visible in the plate, and the fix for a leftover object is always a rule
// keyed on the material path. So the probe logs each distinct path once. Per-draw logging would
// flush a line per draw and stall the render workers, hence the distinct-name budget.
//
// The draw hook runs on several render worker threads, so the name table needs a lock -- taken
// ONLY while the probe has budget left, which is why the fast path above is lock-free.
constexpr int kProbeNameMax = 128;
constexpr int kProbeNameLen = 112;

std::atomic<int> g_probeBudget{0};
std::mutex g_probeMutex;
char g_probeNames[kProbeNameMax][kProbeNameLen];
int g_probeNameCount = 0;

void ProbeNote(const char* materialName, WoClass cls, bool kept, const char* source) {
	if (g_probeBudget.load(std::memory_order_relaxed) <= 0) return;
	if (!MvmDebugLog_Active()) return;
	std::lock_guard<std::mutex> lock(g_probeMutex);
	if (g_probeBudget.load(std::memory_order_relaxed) <= 0) return;
	const char* name = (materialName && *materialName) ? materialName : "<none>";
	// Keyed on path+material: the same material can arrive through more than one submission path
	// and "which path drew it" is the question this probe exists to answer.
	char key[kProbeNameLen];
	_snprintf_s(key, sizeof(key), _TRUNCATE, "%s|%s", source ? source : "?", name);
	for (int i = 0; i < g_probeNameCount; ++i)
		if (0 == std::strcmp(g_probeNames[i], key)) return; // already reported
	if (g_probeNameCount >= kProbeNameMax) return;
	std::strncpy(g_probeNames[g_probeNameCount], key, kProbeNameLen - 1);
	g_probeNames[g_probeNameCount][kProbeNameLen - 1] = '\0';
	++g_probeNameCount;
	g_probeBudget.fetch_sub(1, std::memory_order_relaxed);
	MvmDebugLog_LinefAlways("render.worldonly", "%s via=%s class=%s material='%s'",
		kept ? "KEEP" : "DROP", source ? source : "?", ClassName(cls), name);
}

void ResetCounters() {
	for (int i = 0; i < (int)WoClass::Count; ++i) {
		g_kept[i].store(0, std::memory_order_relaxed);
		g_dropped[i].store(0, std::memory_order_relaxed);
	}
}

} // namespace

void RenderWorldOnly_RunMainThreadFrame() {
	++g_frame;
	const bool armed = g_armed.load(std::memory_order_relaxed);
	// Player shadows: the convar half of the filter. Reconciled first and unconditionally, so
	// disarming puts the user's value back even on the frame the module goes idle.
	ReconcilePlayerDrawCvar(armed);
	// Only while the filter can be live: the bit is meaningless otherwise, and an idle World Only
	// must not walk the entity list at all.
	if (!armed && !g_jobUsesPass.load(std::memory_order_relaxed)) {
		g_bombPlanted.store(false, std::memory_order_relaxed);
		g_bombEntityIndex = -1;
		return;
	}
	if (g_frame - g_lastBombScanFrame < kBombScanFrames) return;
	g_lastBombScanFrame = g_frame;

	int index = -1;
	uint32_t owner = 0xFFFFFFFFu;
	const BombState state = ScanBomb(&index, &owner);
	// Visible unless somebody is carrying it: a planted bomb and a bomb lying on the ground are
	// both objects resting in the scene, and only a carried one would hang in mid-air once its
	// carrier is removed.
	const bool visible = (state == BombState::Planted) || (state == BombState::Dropped);
	const bool was = g_bombPlanted.exchange(visible, std::memory_order_relaxed);
	g_bombEntityIndex = index;
	g_bombOwnerHandle = owner;
	g_bombState = (int)state;
	if (was != visible && MvmDebugLog_Active())
		MvmDebugLog_LinefAlways("render.worldonly",
			"BOMB %s -> C4 draws %s (entityIndex=%d ownerHandle=0x%08x)",
			BombStateName(state), visible ? "KEPT" : "dropped", index, owner);
}

void RenderWorldOnly_SetArmed(bool armed, const char* reason) {
	const bool was = g_armed.load(std::memory_order_relaxed);
	if (was == armed) return;
	if (armed) ResetCounters();
	std::strncpy(g_reason, reason ? reason : "", sizeof(g_reason) - 1);
	g_reason[sizeof(g_reason) - 1] = '\0';
	// Release so a render worker that observes the armed flag also observes the reset counters.
	g_armed.store(armed, std::memory_order_release);
	if (MvmDebugLog_Active())
		MvmDebugLog_LinefAlways("render.worldonly", "%s reason='%s'",
			armed ? "ARM" : "DISARM", g_reason);
}

bool RenderWorldOnly_IsArmed() {
	return g_armed.load(std::memory_order_acquire);
}

bool RenderWorldOnly_Active() {
	return g_armed.load(std::memory_order_relaxed)
		|| AfxStreams_CurrentPassRenderKey() == (int)RenderKey::WorldOnly;
}

void RenderWorldOnly_SetJobUsesPass(bool uses) {
	g_jobUsesPass.store(uses, std::memory_order_release);
}

bool RenderWorldOnly_RejectsMaterialFrom(const char* materialName, const char* source) {
	const WoClass cls = Classify(materialName);
	const bool keep = ClassIsKept(cls);
	(keep ? g_kept : g_dropped)[(int)cls].fetch_add(1, std::memory_order_relaxed);
	ProbeNote(materialName, cls, keep, source);
	return !keep;
}

bool RenderWorldOnly_RejectsMaterial(const char* materialName) {
	return RenderWorldOnly_RejectsMaterialFrom(materialName, "base");
}

bool RenderWorldOnly_RejectsBatchFrom(const char* const* names, int count, const char* source) {
	if (!names || count <= 0) return false;

	// One pass over the batch: reject only if nothing in it is world.
	bool anyKept = false;
	for (int i = 0; i < count; ++i) {
		const WoClass cls = Classify(names[i]);
		const bool keep = ClassIsKept(cls);
		(keep ? g_kept : g_dropped)[(int)cls].fetch_add(1, std::memory_order_relaxed);
		ProbeNote(names[i], cls, keep, source);
		if (keep) anyKept = true;
	}
	if (!anyKept) return true;

	// A batch that mixes world with subjects is kept whole. Worth reporting: it is the shape that
	// silently ate the map before, and if it ever becomes common the whole-batch skip is the wrong
	// granularity and the subject needs removing some other way.
	if (count > 1 && MvmDebugLog_Active()) {
		for (int i = 0; i < count; ++i) {
			if (ClassIsKept(Classify(names[i]))) continue;
			static std::atomic<int> s_mixedReports{0};
			if (s_mixedReports.fetch_add(1, std::memory_order_relaxed) < 24)
				MvmDebugLog_LinefAlways("render.worldonly",
					"MIXED batch kept whole via=%s count=%d subject='%s' (a subject stays in the"
					" plate so the world sharing its batch is not lost)",
					source ? source : "?", count, names[i] ? names[i] : "<none>");
			break;
		}
	}
	return false;
}

void RenderWorldOnly_ArmProbe(int limit) {
	std::lock_guard<std::mutex> lock(g_probeMutex);
	g_probeNameCount = 0;
	g_probeBudget.store(limit > 0 ? limit : 0, std::memory_order_relaxed);
}

void RenderWorldOnly_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd) {
	// `mirv_filmmaker render worldonly <sub> [value]` -- ArgV(0..2) are the dispatcher's own words.
	const char* sub = (argc >= 4) ? args->ArgV(3) : "status";

	if (0 == _stricmp(sub, "on") || 0 == _stricmp(sub, "1")) {
		RenderWorldOnly_SetArmed(true, "console");
		advancedfx::Message("%s render worldonly: ON -- players, player gear, weapons and the"
			" viewmodel are dropped from every scene draw.\n", cmd);
		return;
	}
	if (0 == _stricmp(sub, "off") || 0 == _stricmp(sub, "0")) {
		RenderWorldOnly_SetArmed(false, "console");
		advancedfx::Message("%s render worldonly: off.\n", cmd);
		return;
	}
	if (0 == _stricmp(sub, "probe")) {
		const int limit = (argc >= 5) ? atoi(args->ArgV(4)) : 64;
		RenderWorldOnly_ArmProbe(limit);
		if (limit > 0)
			advancedfx::Message("%s render worldonly probe: armed for %d distinct material(s)."
				" Start mvm_debug and read channel render.worldonly.\n", cmd, limit);
		else
			advancedfx::Message("%s render worldonly probe: disarmed.\n", cmd);
		return;
	}
	if (0 == _stricmp(sub, "bomb")) {
		const char* val = (argc >= 5) ? args->ArgV(4) : nullptr;
		if (val && (0 == _stricmp(val, "off") || 0 == _stricmp(val, "0")))
			g_bombRuleEnabled.store(false, std::memory_order_relaxed);
		else if (val && (0 == _stricmp(val, "auto") || 0 == _stricmp(val, "on") || 0 == _stricmp(val, "1")))
			g_bombRuleEnabled.store(true, std::memory_order_relaxed);
		else if (val) {
			advancedfx::Warning("usage: %s render worldonly bomb auto|off\n", cmd);
			return;
		}
		advancedfx::Message("%s render worldonly bomb: %s -- a PLANTED bomb is %s;"
			" a carried or loose one is always dropped.\n", cmd,
			g_bombRuleEnabled.load(std::memory_order_relaxed) ? "auto" : "off",
			g_bombRuleEnabled.load(std::memory_order_relaxed) ? "kept" : "dropped too (control)");
		return;
	}
	if (0 == _stricmp(sub, "coverage")) {
		// "Why is that object still there?" has exactly two shapes: a classification rule missed it
		// (the probe log shows a KEEP) or the draw never reached a detour at all (nothing in the
		// log). This answers the second one directly instead of by elimination.
		SceneSystem_ReportDrawCoverage();
		return;
	}
	if (0 == _stricmp(sub, "status")) {
		advancedfx::Message("%s render worldonly: %s (armed by '%s') mvm_debug=%s\n",
			cmd, RenderWorldOnly_IsArmed() ? "ON" : "off", g_reason,
			MvmDebugLog_Active() ? "on" : "off");
		advancedfx::Message("   bomb: %s (entity index %d, ownerHandle 0x%08x) -- C4 draws are %s\n",
			BombStateName((BombState)g_bombState), g_bombEntityIndex,
			(unsigned)g_bombOwnerHandle, BombPlanted() ? "KEPT" : "dropped");
		for (int i = 0; i < (int)WoClass::Count; ++i)
			advancedfx::Message("   %-10s kept=%llu dropped=%llu\n", ClassName((WoClass)i),
				(unsigned long long)g_kept[i].load(std::memory_order_relaxed),
				(unsigned long long)g_dropped[i].load(std::memory_order_relaxed));
		return;
	}

	advancedfx::Message(
		"%s render worldonly on|off    - keep the map, drop players/gear/weapons/viewmodel.\n"
		"%s render worldonly probe <n> - log the next n distinct materials and their verdicts\n"
		"                                 to the mvm_debug channel render.worldonly.\n"
		"%s render worldonly status    - state plus per-class kept/dropped draw counts.\n"
		"%s render worldonly coverage  - which scenesystem descriptor classes are hooked at all.\n"
		"%s render worldonly bomb auto|off - verification control: 'off' drops the planted bomb\n"
		"                                 too, so a diff against 'auto' shows what the bomb rule adds.\n",
		cmd, cmd, cmd, cmd, cmd);
}

} // namespace Filmmaker
