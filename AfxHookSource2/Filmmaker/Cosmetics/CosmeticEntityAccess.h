#pragma once

// Low-level, SEH-guarded access to CS2 entity cosmetic data.
//
// WHY THIS IS ITS OWN MODULE. Every function here pokes raw offsets into engine memory. The
// class-name heuristics are only heuristics, the offsets come from a schema dump that a CS2
// update can invalidate, and a demo can hand us a misclassified entity at any time -- so every
// read and write is wrapped in __try/__except and reports failure through a return value instead
// of crashing the game.
//
// That constraint shapes the code in a way that does not mix with ordinary logic: __try/__except
// may not contain C++ objects that need unwinding, so every body here is POD-only and every
// pointer/offset must be resolved by the CALLER before entering. Keeping these together, away
// from CosmeticOverrideSystem's per-frame policy, is what lets that file be written normally.
//
// Nothing here knows about overrides, players, or frames -- it is purely "read/write this field
// on this entity, safely".

#include "CosmeticRebuildFlags.h"

#include <cstddef>
#include <cstdint>

class CEntityInstance;
namespace Filmmaker { struct CosmeticThemePawnIdentity; }

namespace Filmmaker {

// ---- entity lookup -------------------------------------------------------------------------

// Resolves an entity by index the same way MirvCosmetics.cpp / CameraEditorHud.cpp do: bounds +
// null-list checks before touching the (possibly stale) global pointers.
CEntityInstance* CosmeticEntFromIndex(int index);

// Matches both server-style "weapon_*" class names and the client "CWeaponX" / "C_WeaponX"
// schema class names, so it is resilient to either naming scheme being present at runtime.
// A HEURISTIC -- callers must still guard the reads that follow.
bool CosmeticLooksLikeWeaponEntity(CEntityInstance* entity);

unsigned char* CosmeticPawnForSteamId(uint64_t steamId);

// Every connected player controller in the current demo, as (steamId, team) pairs read from the
// controller's pawn. Team is the raw CS team number (2 = T, 3 = CT, 0 when the pawn could not be
// resolved -- a spectator, or a controller whose pawn has not spawned yet). Used by the
// "apply to everyone" cosmetic actions, which must not put a T model on a CT player.
struct CosmeticPlayerRef { uint64_t steamId; int team; uint32_t controllerHandle; };
void CosmeticEnumeratePlayers(CosmeticPlayerRef* out, int maxOut, int* outCount);

// ---- guarded scalar reads ------------------------------------------------------------------

float CosmeticSafeReadEntityFloat(unsigned char* base, ptrdiff_t off, float fallback);
int CosmeticSafeReadGloveDefIndex(unsigned char* pawn);

// Full pawn/controller bindings + correctly sized team and signed health. -1 = unknown/read
// failure, 0 = definite identity mismatch, 1 = valid. Output is written only on success.
int CosmeticReadThemePawnIdentity(CEntityInstance* controller, CEntityInstance* pawn,
	CosmeticThemePawnIdentity* out, int* health);
int CosmeticResolveThemePawn(uint32_t controllerHandle, CEntityInstance** pawn,
	CosmeticThemePawnIdentity* out, int* health);

// Reads an entity's LIVE rendered model path (CModelState::m_ModelName) via the same
// body-component/skeleton chain CosmeticDebug.cpp uses. Lets the diagnostic log show whether a
// knife/weapon entity is actually rendering the swapped model or still the original.
// out[0] = '\0' on any failure.
void CosmeticReadEntityModelPath(CEntityInstance* ent, char* out, size_t outSize);

// SEH-isolated vtable call for the OPTIONAL forced re-composite. A wrong index access-violates
// HERE and is reported via the return value. Signature matches
// CBasePlayerWeapon::UpdateComposite(bool) -- on x64 __thiscall == __fastcall.
bool CosmeticSafeVCall(void* obj, int idx, int arg);

// ---- econ identity -------------------------------------------------------------------------

struct WeaponEconRead {
	uint64_t xuid = 0;
	unsigned char* itemView = nullptr;
	int liveDef = 0;
};

// Reads a weapon entity's econ identity (owner XUID + embedded C_EconItemView pointer + live
// item-definition index). CosmeticLooksLikeWeaponEntity is only a class-name heuristic, so a
// "weapon_" entity that is not actually a C_EconEntity would access-violate if read directly --
// guarding here means such an entity is skipped, never crashing the game.
bool CosmeticTryReadWeaponEconInfo(unsigned char* w, ptrdiff_t offXuidLow, ptrdiff_t offXuidHigh,
	ptrdiff_t offAttrMgr, ptrdiff_t offItem, ptrdiff_t offDef, WeaponEconRead* out);

// ---- the skin write ------------------------------------------------------------------------

// `changed` is true when at least one value actually differed from the target (drives the
// re-composite), `written` counts matched attributes. `hasPaint` is true when a paint-kit
// attribute (def 6) was present to overwrite -- when FALSE the item has no networked paint
// attribute (default knives/gloves, freshly-picked-up or viewmodel entities), so the override
// must be applied via the engine named-setter fallback instead.
struct AttrWriteResult {
	bool ok = false;
	int written = 0;
	bool changed = false;
	bool hasPaint = false;
};

// THE real skin write. A networked demo weapon stores its actual skin in
// m_NetworkedDynamicAttributes (def 6 = paint kit, 7 = seed, 8 = wear, 80 = StatTrak) -- the
// m_nFallback* fields are only consulted when the item has NO networked attributes, so on a demo
// item they are ignored. This OVERWRITES the matching attribute VALUES in place; it cannot add
// new ones, so a missing def (e.g. StatTrak on a non-ST gun) is skipped.
void CosmeticWriteNetworkedSkinAttributes(unsigned char* itemView, ptrdiff_t networkedOff,
	ptrdiff_t attrSubOff, int stride, ptrdiff_t defOff, ptrdiff_t valOff,
	int paintKit, int seed, float wear, int statTrak, AttrWriteResult* out);

struct ApplyResult {
	bool patched = false;
	bool reverted = false;
	bool needComposite = false; // a (re)composite is warranted this frame
};

// The proven write recipe, generalized to an arbitrary econ entity + arbitrary fallback offsets.
// Every pointer/offset is resolved by the caller BEFORE entering so the __try body only ever
// touches POD pointers/primitives.
//
// knifeDefOverride > 0 requests a numeric def swap (best-effort; NOT a model load).
// markStaleEnabled = do the visuals-stale field writes when a value changed (per-frame rebuild).
// forceStale       = do them unconditionally (the manual "rebuild once" path), even if unchanged.
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
	CosmeticRebuildFlags flags);

} // namespace Filmmaker
