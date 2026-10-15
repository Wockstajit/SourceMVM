#pragma once

// INTERNAL shared surface of the cosmetics debug/diagnostics subsystem. Not part of the
// public API -- include CosmeticOverrideSystem.h for that. This header exists so the
// subsystem can be split into focused translation units:
//
//   CosmeticDebug.cpp        read-only diagnostics PRINTING ("cosmetics status" /
//                            "debug" / "visualdiag" console dumps) + spectate resolution.
//                            Also implements the shared readers declared below.
//   CosmeticDebugEvents.cpp  structured EVENT logging into the mvm_debug log
//                            (uiclick / glove-pick / weapon-pick / spectate-switch /
//                            skin.live snapshots + the per-frame change detector).
//   ../Debug/MvmDebugLog.cpp the shared log TRANSPORT (file writer, VEH crash breadcrumb,
//                            agent ingest). Deliberately <windows.h>-isolated; carries
//                            no cosmetics domain logic.
//
// Everything here is a read-only, SEH-guarded POD reader: fixed-size out-structs filled
// inside __try/__except (no C++ objects needing unwind), faults swallowed, so a stale or
// misclassified entity pointer degrades to ok=false instead of an access violation.

#include "CosmeticCatalog.h" // CosmeticSlot

#include <cstdint>

class CEntityInstance;

namespace Filmmaker {
namespace CosmeticDebugRead {

// Same bounds-checked entity resolve used by CosmeticOverrideSystem.cpp / MirvCosmetics.cpp.
CEntityInstance* EntFromIndex(int index);

// POD dump of one CAttributeList's contents (defIndex -> value pairs). Filled inside a
// __try/__except, so no C++ objects -- a fixed array, no std::vector.
struct AttrListDump {
	bool ok = false;
	bool resolved = false; // the list offset itself was non-zero (field exists on this build)
	int count = 0;
	struct { int def; float val; } items[24] = {};
};

// Walks the embedded attribute vector at (itemView + listOff + C_AttributeList.m_Attributes) and
// copies up to 24 {defIndex,value} pairs out. Accepts both observed vector layouts (count+ptr and
// ptr+count). SEH-guarded; listOff==0 means "field not present on this build".
void ReadAttrList(unsigned char* itemView, ptrdiff_t listOff, AttrListDump* out);

// Extracts a specific attribute def's value out of an already-read AttrListDump. Returns false if
// the def is not present (e.g. StatTrak on a non-ST gun).
bool AttrValueForDef(const AttrListDump& d, int def, float* outVal);

// POD dump of one weapon entity's full visual/econ cache state. `have*` flags distinguish
// "offset unresolved on this build" (false) from "read 0" (true, value 0).
struct WeaponVisualDiag {
	bool ok = false;
	uint64_t ownerXuid = 0;
	int defIndex = 0;
	int32_t itemIdHigh = 0;
	uint32_t itemIdLow = 0;
	uint32_t accountId = 0;
	int32_t fbPaint = 0;
	float fbWear = 0.0f;
	int32_t fbSeed = 0;
	int32_t fbStat = 0;
	bool haveVisualsData = false;  unsigned char visualsDataSet = 0;
	bool haveClearUgc = false;     unsigned char clearUgc = 0;
	bool haveReloadEvent = false;  int32_t reloadEvent = 0;
	bool haveAttrInit = false;     unsigned char attrInit = 0;
};

// Reads everything in WeaponVisualDiag off a weapon entity. Caller resolves itemView.
void ReadWeaponVisualDiag(unsigned char* w, unsigned char* itemView, WeaponVisualDiag* out);

// POD result of the read-only model-name read (CModelState::m_ModelName chain).
struct ModelNameResult {
	bool ok = false;
	char name[256] = {};
};

// Player-pawn variant (validates IsPlayerPawn) and the generalized any-entity variant (a weapon
// entity is a C_BaseAnimGraph and carries the same body-component/skeleton chain).
void ReadPawnModelName(int pawnIndex, ModelNameResult* out);
void ReadEntityModelName(CEntityInstance* ent, ModelNameResult* out);

const char* SlotLabel(CosmeticSlot slot);

} // namespace CosmeticDebugRead
} // namespace Filmmaker
