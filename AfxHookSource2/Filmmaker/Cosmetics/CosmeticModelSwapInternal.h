#pragma once

// INTERNAL shared surface of the model-swap subsystem. Not part of the public API --
// include CosmeticModelSwap.h for that. This header exists so the subsystem can be split
// into focused translation units without changing the public surface:
//
//   CosmeticFnResolver.cpp   client.dll byte-pattern resolution (the Fns table +
//                            ResolveModelSwapFns), the SEH-guarded native-call wrappers
//                            (precache / SetModel / SetMeshGroupMask / PostDataUpdate /
//                            UpdateSubclass), econ-schema lookups (item-def model path,
//                            paint-kit legacy flag), and the animgraph-reset experiment
//   CosmeticKnifeSwap.cpp    knife def -> model table + ApplyKnifeModelSwap /
//                            ResolveKnifeModelPath (the knife TYPE swap)
//   CosmeticGloveSwap.cpp    glove econ-view field/attribute writes + ApplyGloveModel
//   CosmeticModelSwap.cpp    core: HUD-arms / first-person-viewmodel resolution and
//                            mirroring, weapon mesh-mask + agent swaps
//
// Everything here lives in Filmmaker::modelswap (internal); the public entry points stay
// in Filmmaker as declared by CosmeticModelSwap.h.

#include "CosmeticModelSwap.h" // ModelSwapResolveStatus

#include <cstddef>
#include <cstdint>

namespace Filmmaker {
namespace modelswap {

// ---- resolved function typedefs (x64 __thiscall == __fastcall) --------------------------------------
typedef void (__fastcall* SetModel_t)(void* modelEntity, const char* model);
typedef void (__fastcall* SetMeshGroupMask_t)(void* sceneNode, uint64_t mask);
typedef void (__fastcall* UpdateSubclass_t)(void* weapon);
typedef void (__fastcall* SetBodyGroup_t)(void* pawn, const char* group, unsigned int unk);
typedef void (__fastcall* SetBodyGroupNumeric_t)(void* entity, uint64_t group, uint64_t value);
typedef void (__fastcall* RegenerateSkins_t)(void);
typedef void (__fastcall* UpdateBodyGroupChoice_t)(void* entity);
typedef void* (__fastcall* GetStaticData_t)(void* econItemView);
typedef void* (__fastcall* GetEconItemSystem_t)(void* unused);
// C_EconItemView::SetAttributeValueByName(view, name, float) -- the engine named-setter used to ADD a
// paint attribute to an item that has none (the default-glove / default-weapon attrWritten=0 case).
typedef void (__fastcall* SetAttributeValueByName_t)(void* itemView, const char* name, float value);
typedef void (__fastcall* ConstructPaintKit_t)(void* itemView);
typedef void* (__fastcall* SchemaGetItemDefByIndex_t)(void* schema, int defIndex);

struct Fns {
	SetModel_t setModel = nullptr;
	SetMeshGroupMask_t setMeshGroupMask = nullptr;
	UpdateSubclass_t updateSubclass = nullptr;
	SetBodyGroup_t setBodyGroup = nullptr;
	SetBodyGroupNumeric_t setBodyGroupNumeric = nullptr;
	RegenerateSkins_t regenerateSkins = nullptr;
	UpdateBodyGroupChoice_t updateBodyGroupChoice = nullptr;
	GetStaticData_t getStaticData = nullptr;
	GetEconItemSystem_t getEconItemSystem = nullptr;
	SetAttributeValueByName_t setAttributeValueByName = nullptr;
	ConstructPaintKit_t constructPaintKit = nullptr;
	SchemaGetItemDefByIndex_t getItemDefByIndex = nullptr;
};

// ---- CosmeticFnResolver.cpp -------------------------------------------------------------------------

extern Fns g_fns;
extern ModelSwapResolveStatus g_status;
// approach #2: blocking model precache before SetModel (the root-cause fix; default ON).
extern bool g_precacheOn;
extern uint64_t g_precacheCalls; // diagnostics-only counter (precache breadcrumbs)
// CEconItemSchema item-definition CUtlMap offset (first hit is cached; 0 = not yet found).
extern ptrdiff_t g_itemDefMapOffset;

bool IsSupportedModelPath(const char* model);
bool IsSupportedPlayerModelPath(const char* model);
// SEH-guarded: read the target model path from a (def-already-set) econ item view via GetStaticData,
// validating the returned pointer/string. failureReason (optional) is a short code for diagnostics.
bool TryGetModelFromStaticData(unsigned char* itemView, char* out, size_t outSize,
	const char** failureReason = nullptr);
bool SafePrecacheModel(const char* model);
bool SafeSetModel(unsigned char* entity, const char* model);
// client.dll byte-pattern scan (wildcards are "??") and E8 rel32 call-target resolution.
size_t FindClientPattern(const char* pattern);
void* ResolveRelCall(size_t callAddr);
bool SafeSetMeshMask(unsigned char* entity, uint64_t mask);
bool SafePostDataUpdate(unsigned char* entity);
bool SafeUpdateSubclass(unsigned char* weapon, int targetDef);
// Animgraph-reset experiment (approach #1, default OFF -- see CosmeticModelSwap.h).
void ResetAnimGraph(unsigned char* entity, int entityIndex, const char* tag);

// ---- CosmeticModelSwap.cpp (core: HUD-arms / viewmodel) ----------------------------------------------

unsigned char* HudArmsForPawn(unsigned char* pawn);
bool HudArmsOwnedByPawnIndex(unsigned char* arms, int pawnIdx);
// Mirror a model/mesh write onto the FIRST-PERSON viewmodel of the weapon we just changed
// (class-matched only -- see the safety comment at its definition). worldWeapon == null means
// "no model/mesh change, just refresh the renderables". Returns how many first-person copies matched.
// written (a mirror window, MirrorWeaponMeshToViewmodel): a copy this window already wrote once and
// that reads back meshMask keeps it untouched, so a repeated mirror never re-invalidates its material.
// Null forces every write, as a pooled copy's readback alone cannot be trusted.
int RefreshViewmodelWeapons(unsigned char* pawn, const char* model, uint64_t meshMask,
	unsigned char* worldWeapon, int entityIndex = -1, bool releaseComposite = false,
	ViewmodelMirrorWrites* written = nullptr);

} // namespace modelswap
} // namespace Filmmaker
