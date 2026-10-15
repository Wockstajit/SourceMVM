#pragma once

// Item-definition CLASSIFIER for the cosmetics backend.
//
// NOTE: the user-facing skin catalog (every released finish per weapon) is the GENERATED file
// Panorama/CameraEditorCosmeticsCatalog.inc, which is COMPILED INTO the UI -- it is not read at
// runtime by the DLL. The native backend does not need that data to apply an override; it only
// needs to classify a live item-definition index into a loadout slot so the right profile slot is
// applied to the right entity. That is what this file provides (a tiny, allocation-free lookup,
// mirroring SlotForWeaponDef in Cosmetics/CosmeticUiQueries.cpp).

namespace Filmmaker {

enum class CosmeticSlot {
	None,
	Primary,    // rifles, SMGs, shotguns, MGs
	Secondary,  // pistols
	Knife,      // melee
	Gloves,     // hand wraps
	Agent       // player model (not an econ weapon entity)
};

namespace CosmeticCatalog {

// Classify a weapon/item definition index (C_EconItemView::m_iItemDefinitionIndex) into a slot.
// Returns CosmeticSlot::None for anything that is not an overridable loadout item.
CosmeticSlot SlotForDefIndex(int defIndex);

// True if defIndex is any knife/melee definition (default knives included).
bool IsKnifeDef(int defIndex);

// True if defIndex is a glove definition (5027..5040 range etc.).
bool IsGloveDef(int defIndex);

// True for the two team default knives (42 = CT default, 59 = T default). A knife paint override
// must still apply when the held weapon is the default knife, so the apply loop treats these as a
// knife slot regardless of which team default is equipped.
bool IsDefaultKnifeDef(int defIndex);

} // namespace CosmeticCatalog

} // namespace Filmmaker
